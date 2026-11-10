#include <game.h>
// JNI entry points of libgame.so for the Android app (android/src/com/xport/
// MainActivity.java). The game runs headless on its own thread; the activity shows
// os::status::Describe() as text until the renderer exists.
#if defined(__ANDROID__) && defined(XPORT_SHARED)
#include <stdafx.h>
#include <jni.h>
#include <pthread.h>
#include <android/native_window_jni.h>
#include "status.h"
#include "touch_pad.h"
#include "perf_hint.h"
#include "swap_in.h"
#include "background.h"
#include <kernel/memory.h>

int XportMain();
void SetGpuSurface(ANativeWindow* window); // runtime/gpu/command_processor.cpp

namespace
{
    std::string JavaString(JNIEnv* env, jstring value)
    {
        if (value == nullptr)
            return {};
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string result = chars;
        env->ReleaseStringUTFChars(value, chars);
        return result;
    }

    void* GameThread(void*)
    {
        XportMain();
        LOGN("game thread finished");
        return nullptr;
    }
}

// options: extra "NAME=value" pairs separated by spaces (XPORT_AUTOPRESS=1, XPORT_LOG_FILES=1...),
// passed with: adb shell am start -n com.xport/.MainActivity --es env "XPORT_AUTOPRESS=1"
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeStart(JNIEnv* env, jclass, jstring gameDir, jstring saveDir, jstring options)
{
    static bool s_started;
    if (s_started)
        return;
    s_started = true;

    setenv("XPORT_GAME_DIR", JavaString(env, gameDir).c_str(), 1);
    setenv("XPORT_SAVE_DIR", JavaString(env, saveDir).c_str(), 1);
    // ReXGlue's user folder (native profiler exports, SPIR-V dumps) is $XDG_DATA_HOME, else
    // $HOME/.local/share = /data/.local/share, which the app cannot write: the profile was lost
    // without a message. Use the app's external files folder, next to save/ (readable with adb).
    setenv("XDG_DATA_HOME", std::filesystem::path(JavaString(env, saveDir)).parent_path().c_str(), 1);
    // The native Vulkan renderer (rexglue_glue/ + third_party/rexglue).
    setenv("XPORT_GPU", "native", 1);
    setenv("XPORT_AFFINITY", "1", 1);
    setenv("XNATIVE_CVARS",
           "xnative_merge_up_max=64,"
           "xnative_merge_indexed_draws=true,"
           "xnative_early_fragment_tests=false,"
           "xnative_fs_relaxed_precision=false,"
           "xnative_lazy_resolves=true,"
           "xnative_resolve_swap=true,"
           "xnative_gpu_detile=true,"
           "xnative_pipelined_recording=true,"
           "xnative_async_command_retire=true", 1);
    // Direct resolves (color, 2x2 box, float24 depth): same image, less GPU work on the Mali.
    setenv("XPORT_XENIA_DIRECT_RESOLVE", "7", 1);
    // RAM (the phone has 7.5 GB and the game used ~2.4): textures unused for 10 s are freed once
    // the texture cache passes 128 MB, all old ones past 384 MB (Xenia: 30 s, 384 and 768 MB).
    // Every texture in its own memory (shared blocks were left ~40% empty by freed textures).
    setenv("XPORT_XENIA_TEXTURE_DEDICATED", "1", 1);
    setenv("XPORT_XENIA_CVARS_DEFAULTS",
           "texture_cache_memory_limit_soft=128,texture_cache_memory_limit_soft_lifetime=10,"
           "texture_cache_memory_limit_hard=384", 1);

    const std::string list = JavaString(env, options);
    size_t start = 0;
    while (start < list.size())
    {
        size_t end = list.find(' ', start);
        if (end == std::string::npos)
            end = list.size();
        const std::string pair = list.substr(start, end - start);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos)
            setenv(pair.substr(0, equals).c_str(), pair.substr(equals + 1).c_str(), 1);
        start = end + 1;
    }

    // Recompiled functions use large host stack frames; the console's main thread is deep too.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 64 * 1024 * 1024);
    pthread_t thread;
    StartSwapInThread();
    pthread_create(&thread, &attributes, GameThread, nullptr);
    pthread_attr_destroy(&attributes);
    pthread_detach(thread);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_xport_MainActivity_nativeStatus(JNIEnv* env, jclass)
{
    return env->NewStringUTF(os::status::Describe().c_str());
}

// The frame counter over the game (MainActivity): {frames per second, longest frame in ms} since
// the previous call.
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_xport_MainActivity_nativeFrameCounter(JNIEnv* env, jclass)
{
    float values[2];
    FrameCounterRead(values[0], values[1]);
    jfloatArray result = env->NewFloatArray(2);
    if (result)
        env->SetFloatArrayRegion(result, 0, 2, values);
    return result;
}

// The SurfaceView's surface for the GPU (created, resized, or null when destroyed). The GPU is
// detached from the old window before it is released.
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeSetSurface(JNIEnv* env, jclass, jobject surface)
{
    static ANativeWindow* s_window;
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (window && window == s_window)
    {
        ANativeWindow_release(window); // same window resized: keep the reference already held
        window = s_window;
    }
    SetGpuSurface(window);
    if (s_window && s_window != window)
        ANativeWindow_release(s_window);
    s_window = window;
}
// The touch controls' virtual pad (android/src/com/xport/TouchControls.java), set on every
// change: XAMINPUT_GAMEPAD_* buttons, triggers 0..255, sticks -32768..32767 (Y up).
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeSetPad(JNIEnv*, jclass, jint buttons, jint leftTrigger,
    jint rightTrigger, jint thumbLX, jint thumbLY, jint thumbRX, jint thumbRY)
{
    os::touch_pad::State state;
    state.buttons = uint16_t(buttons);
    state.leftTrigger = uint8_t(std::clamp(leftTrigger, 0, 255));
    state.rightTrigger = uint8_t(std::clamp(rightTrigger, 0, 255));
    state.thumbLX = int16_t(std::clamp(thumbLX, -32768, 32767));
    state.thumbLY = int16_t(std::clamp(thumbLY, -32768, 32767));
    state.thumbRX = int16_t(std::clamp(thumbRX, -32768, 32767));
    state.thumbRY = int16_t(std::clamp(thumbRY, -32768, 32767));
    os::touch_pad::Set(state);
}

// The activity's window lost or regained input focus: the notification shade came down, the
// recent apps opened, the app went to the background. The title pauses only from its own pause menu
// (it ignores the console Guide's XN_SYS_UI, tried 2026-10-27), so a focus loss presses START for
// 200 ms unless that menu is already open. The game resumes from the menu, as after a START.
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeSetFocus(JNIEnv*, jclass, jboolean focus)
{
    if (focus || !g_memory.base)
        return;
    const bool menu = game::PauseMenuOpen();
    LOGFN("focus lost, pause menu {}", menu ? "already open" : "opened (START)");
    if (!menu)
        os::touch_pad::Press(XAMINPUT_GAMEPAD_START, 200);
}

// The activity stopped (the home screen or another app covers it) or started again. Stopped, the
// game is held (os/background.h) once the pause menu that the focus loss asked for is open, or
// after 1.5 s without it (loading screens): the START press has to reach the game first.
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeSetBackground(JNIEnv*, jclass, jboolean background)
{
    if (!background || !g_memory.base)
    {
        ReleaseBackgroundHold();
        return;
    }
    const uint32_t ticket = RequestBackgroundHold();
    std::thread([ticket]
    {
        for (int waited = 0; waited < 1500 && !game::PauseMenuOpen(); waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EngageBackgroundHold(ticket);
    }).detach();
}
#endif
