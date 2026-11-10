// the launcher's platform side (see launcher.h): the JNI entry points MainActivity calls,
// the launcher thread with its EGL context on the launcher's SurfaceView, the input queue, the
// background textures and the hand-off to the game. Nothing here runs once the game starts.
#if defined(__ANDROID__) && defined(XPORT_SHARED)
#include <stdafx.h>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <deque>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#pragma clang diagnostic pop
#include "launcher.h"

namespace launcher
{
namespace
{
    // Key codes MainActivity sends (nativeLauncherKey).
    enum Key { kKeyUp = 1, kKeyDown, kKeyLeft, kKeyRight, kKeyAccept, kKeyBack };

    struct Event
    {
        enum Type { Touch, Key, Gamepad, DriverImported, IsoPicked, Insets } type;
        int action = 0; // Touch: 0 down, 1 move, 2 up, 3 cancel. Key: the Key code. IsoPicked: the fd.
        float x = 0, y = 0, right = 0, bottom = 0; // Touch: x, y. Insets: x left, y top.
        bool flag = false; // Key: pressed. DriverImported: succeeded.
        std::string text;
    };

    JavaVM* g_vm;
    jclass g_activity;
    jmethodID g_playMethod, g_exitMethod, g_pickMethod, g_pickIsoMethod, g_restartMethod;
#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
    jmethodID g_copyMethod;
#endif
    jobject g_assetsRef;
    AAssetManager* g_assets;
    Platform g_platform;

    std::mutex g_mutex;
    std::condition_variable g_wake;    // the launcher thread waits on it for a window or events
    std::condition_variable g_handled; // nativeLauncherSurface waits on it for a window change
    std::deque<Event> g_events;
    ANativeWindow* g_pendingWindow;
    bool g_windowPending;
    bool g_running;

    // Launcher thread only.
    int g_request; // 1 play, 2 exit
    std::string g_playOptions;

    std::string JavaString(JNIEnv* env, jstring value)
    {
        if (!value)
            return {};
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string result = chars;
        env->ReleaseStringUTFChars(value, chars);
        return result;
    }

    struct Image
    {
        int width = 0, height = 0;
        std::vector<uint8_t> pixels; // RGBA8
    };

    Image Decode(const char* asset)
    {
        Image image;
        const std::vector<uint8_t> data = ReadAsset(asset);
        int components = 0;
        stbi_uc* pixels = data.empty() ? nullptr
            : stbi_load_from_memory(data.data(), int(data.size()), &image.width, &image.height, &components, 4);
        if (!pixels)
        {
            LOGFN_WARNING("launcher: {} could not be decoded", asset);
            return {};
        }
        image.pixels.assign(pixels, pixels + size_t(image.width) * image.height * 4);
        stbi_image_free(pixels);
        return image;
    }

    // The frosted-glass copy of the background: a quarter of its size, then three box blurs each
    // way (close to a gaussian). The panels draw their part of it under their tint.
    Image Blur(const Image& source)
    {
        Image out;
        if (source.pixels.empty())
            return out;
        constexpr int kStep = 4, kRadius = 7;
        out.width = std::max(1, source.width / kStep);
        out.height = std::max(1, source.height / kStep);
        std::vector<float> a(size_t(out.width) * out.height * 4), b(a.size());
        for (int y = 0; y < out.height; ++y)
        {
            for (int x = 0; x < out.width; ++x)
            {
                float sum[4] = {};
                for (int sy = 0; sy < kStep; ++sy)
                {
                    for (int sx = 0; sx < kStep; ++sx)
                    {
                        const uint8_t* p = &source.pixels[(size_t(y * kStep + sy) * source.width + (x * kStep + sx)) * 4];
                        for (int c = 0; c < 4; ++c)
                            sum[c] += p[c];
                    }
                }
                for (int c = 0; c < 4; ++c)
                    a[(size_t(y) * out.width + x) * 4 + c] = sum[c] / (kStep * kStep);
            }
        }
        auto pass = [&](const std::vector<float>& in, std::vector<float>& to, bool horizontal)
        {
            const int length = horizontal ? out.width : out.height;
            const int lines = horizontal ? out.height : out.width;
            for (int line = 0; line < lines; ++line)
            {
                for (int i = 0; i < length; ++i)
                {
                    float sum[4] = {};
                    for (int k = -kRadius; k <= kRadius; ++k)
                    {
                        const int j = std::clamp(i + k, 0, length - 1);
                        const size_t index = horizontal ? size_t(line) * out.width + j : size_t(j) * out.width + line;
                        for (int c = 0; c < 4; ++c)
                            sum[c] += in[index * 4 + c];
                    }
                    const size_t index = horizontal ? size_t(line) * out.width + i : size_t(i) * out.width + line;
                    for (int c = 0; c < 4; ++c)
                        to[index * 4 + c] = sum[c] / (2 * kRadius + 1);
                }
            }
        };
        for (int round = 0; round < 3; ++round)
        {
            pass(a, b, true);
            pass(b, a, false);
        }
        out.pixels.resize(a.size());
        for (size_t i = 0; i < a.size(); ++i)
            out.pixels[i] = uint8_t(std::clamp(a[i] + 0.5f, 0.f, 255.f));
        return out;
    }

    Texture Upload(const Image& image, bool mipmaps)
    {
        Texture texture;
        if (image.pixels.empty())
            return texture;
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
            image.pixels.data());
        if (mipmaps)
            glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        texture.id = id;
        texture.width = image.width;
        texture.height = image.height;
        return texture;
    }

    // A popup's backdrop (AddBackdropBlur): what is drawn so far, blurred and put back. It is copied
    // to a quarter of the screen's size, blurred there with two separable gaussian passes each way
    // and scaled back up; ImGui's OpenGL renderer calls it in its place among the draw commands.
    struct Backdrop
    {
        GLuint program = 0, vao = 0;
        GLint texel = -1;
        GLuint textures[2] = {}, framebuffers[2] = {};
        int width = 0, height = 0;
        bool failed = false;
    };
    Backdrop g_backdrop;

    GLuint CompileShader(GLenum type, const char* source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[512] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOGFN_WARNING("launcher: backdrop shader: {}", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    bool BackdropReady(int width, int height)
    {
        Backdrop& b = g_backdrop;
        if (b.failed)
            return false;
        if (!b.program)
        {
            // One triangle over the whole target; uv runs 0..1 across it.
            const GLuint vertex = CompileShader(GL_VERTEX_SHADER, "#version 300 es\n"
                "out vec2 uv;\n"
                "void main() {\n"
                "    uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                "    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
                "}\n");
            // Nine taps along texel's direction, in five fetches (linear sampling between pairs).
            const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, "#version 300 es\n"
                "precision mediump float;\n"
                "uniform sampler2D source;\n"
                "uniform vec2 texel;\n"
                "in vec2 uv;\n"
                "out vec4 color;\n"
                "void main() {\n"
                "    vec2 near = texel * 1.3846154, far = texel * 3.2307692;\n"
                "    color = texture(source, uv) * 0.2270270\n"
                "        + (texture(source, uv + near) + texture(source, uv - near)) * 0.3162162\n"
                "        + (texture(source, uv + far) + texture(source, uv - far)) * 0.0702703;\n"
                "}\n");
            GLint linked = 0;
            if (vertex && fragment)
            {
                b.program = glCreateProgram();
                glAttachShader(b.program, vertex);
                glAttachShader(b.program, fragment);
                glLinkProgram(b.program);
                glGetProgramiv(b.program, GL_LINK_STATUS, &linked);
            }
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            if (!linked)
            {
                LOGN_WARNING("launcher: no backdrop blur (its shaders did not build)");
                if (b.program)
                    glDeleteProgram(b.program);
                b.program = 0;
                b.failed = true;
                return false;
            }
            b.texel = glGetUniformLocation(b.program, "texel");
            glUseProgram(b.program);
            glUniform1i(glGetUniformLocation(b.program, "source"), 0);
            glGenVertexArrays(1, &b.vao);
        }
        if (b.width != width || b.height != height)
        {
            if (b.textures[0])
            {
                glDeleteFramebuffers(2, b.framebuffers);
                glDeleteTextures(2, b.textures);
            }
            glGenTextures(2, b.textures);
            glGenFramebuffers(2, b.framebuffers);
            for (int i = 0; i < 2; ++i)
            {
                glBindTexture(GL_TEXTURE_2D, b.textures[i]);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindFramebuffer(GL_FRAMEBUFFER, b.framebuffers[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.textures[i], 0);
            }
            b.width = width;
            b.height = height;
        }
        return true;
    }

    void BlurBackdrop(const ImDrawList*, const ImDrawCmd*)
    {
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        const int screenWidth = viewport[2], screenHeight = viewport[3];
        const int width = std::max(1, screenWidth / 4), height = std::max(1, screenHeight / 4);
        if (screenWidth <= 0 || !BackdropReady(width, height))
            return;
        Backdrop& b = g_backdrop;
        glDisable(GL_SCISSOR_TEST); // it limits blits too
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b.framebuffers[0]);
        glBlitFramebuffer(0, 0, screenWidth, screenHeight, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glViewport(0, 0, width, height);
        glUseProgram(b.program);
        glBindVertexArray(b.vao);
        glActiveTexture(GL_TEXTURE0);
        // Across, down, then both again twice as wide; the image ends where it started (textures[0]).
        for (int pass = 0; pass < 4; ++pass)
        {
            const int from = pass & 1, to = from ^ 1;
            const float spread = pass < 2 ? 1.f : 2.f;
            glBindFramebuffer(GL_FRAMEBUFFER, b.framebuffers[to]);
            glBindTexture(GL_TEXTURE_2D, b.textures[from]);
            glUniform2f(b.texel, from == 0 ? spread / width : 0.f, from == 1 ? spread / height : 0.f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, b.framebuffers[0]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, width, height, 0, 0, screenWidth, screenHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    void FreeBackdrop()
    {
        Backdrop& b = g_backdrop;
        if (b.textures[0])
        {
            glDeleteFramebuffers(2, b.framebuffers);
            glDeleteTextures(2, b.textures);
        }
        if (b.vao)
            glDeleteVertexArrays(1, &b.vao);
        if (b.program)
            glDeleteProgram(b.program);
        b = Backdrop{};
    }

    void Feed(const Event& event, bool& hoverOff)
    {
        ImGuiIO& io = ImGui::GetIO();
        switch (event.type)
        {
        case Event::Touch:
            UiInputSource(true);
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(event.x, event.y);
            if (event.action == 0)
                io.AddMouseButtonEvent(0, true);
            else if (event.action >= 2)
            {
                io.AddMouseButtonEvent(0, false);
                hoverOff = true; // a finger has no hover: lift it off the screen next frame
            }
            break;
        case Event::Key:
        {
            UiInputSource(false);
            if (event.action == kKeyBack)
            {
                if (event.flag)
                    UiBack();
                break;
            }
            static const ImGuiKey keys[] = {ImGuiKey_None, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
                ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown};
            if (event.action >= kKeyUp && event.action <= kKeyAccept)
                io.AddKeyEvent(keys[event.action], event.flag);
            break;
        }
        case Event::Gamepad:
            UiGamepad(event.text);
            break;
        case Event::DriverImported:
            UiDriverImported(event.text, event.flag);
            break;
        case Event::IsoPicked:
            UiIsoPicked(event.action, event.text);
            break;
        case Event::Insets:
            UiInsets(event.x, event.y, event.right, event.bottom);
            break;
        }
    }

    void CallJava(JNIEnv* env, jmethodID method, const std::string* text)
    {
        if (!method)
            return;
        if (text)
        {
            jstring value = env->NewStringUTF(text->c_str());
            env->CallStaticVoidMethod(g_activity, method, value);
            env->DeleteLocalRef(value);
        }
        else
        {
            env->CallStaticVoidMethod(g_activity, method);
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
    }

    void Run()
    {
        JNIEnv* env = nullptr;
        g_vm->AttachCurrentThread(&env, nullptr);

        // The probe answers once for the whole process; vk_driver.cpp reuses it at the game's start.
        const bool adreno = UiAdreno();
        const Image background = Decode(adreno ? "launcher/bg_adreno.png" : "launcher/bg_mali.png");
        const Image blurred = Blur(background);

        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);
        const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configs = 0;
        eglChooseConfig(display, configAttributes, &config, 1, &configs);
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext context = configs ? eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes) : EGL_NO_CONTEXT;
        if (context == EGL_NO_CONTEXT)
        {
            // No launcher: the game starts as it did before it, with the files/ switches as they are.
            LOGN_ERROR("launcher: no OpenGL ES 3 context, starting the game directly");
            g_playOptions.clear();
            g_request = 1;
        }

        ANativeWindow* window = nullptr;
        EGLSurface surface = EGL_NO_SURFACE;
        bool ready = false;
        Texture textures[2];
        bool hoverOff = false;
        auto last = std::chrono::steady_clock::now();
        auto lastInput = last;

        while (context != EGL_NO_CONTEXT && g_request == 0)
        {
            std::vector<Event> events;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                if (surface == EGL_NO_SURFACE)
                    g_wake.wait(lock, [] { return g_windowPending; });
                if (g_windowPending)
                {
                    ANativeWindow* next = g_pendingWindow;
                    g_pendingWindow = nullptr;
                    g_windowPending = false;
                    if (next != window)
                    {
                        if (surface != EGL_NO_SURFACE)
                        {
                            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                            eglDestroySurface(display, surface);
                            surface = EGL_NO_SURFACE;
                        }
                        if (window)
                            ANativeWindow_release(window);
                        window = next;
                        if (window)
                            surface = eglCreateWindowSurface(display, config, window, nullptr);
                    }
                    else if (next)
                    {
                        ANativeWindow_release(next); // the same window resized: one reference is enough
                    }
                    g_handled.notify_all();
                }
                events.assign(g_events.begin(), g_events.end());
                g_events.clear();
            }
            if (surface == EGL_NO_SURFACE)
                continue;
            if (!eglMakeCurrent(display, surface, surface, context))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (!ready)
            {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
                UiInit();
                ImGui_ImplOpenGL3_Init("#version 300 es");
                textures[0] = Upload(background, true);
                textures[1] = Upload(blurred, false);
                UiSetTextures(textures[0], textures[1]);
                eglSwapInterval(display, 1);
                ready = true;
            }

            ImGuiIO& io = ImGui::GetIO();
            if (hoverOff)
            {
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                hoverOff = false;
            }
            for (const Event& event : events)
                Feed(event, hoverOff);
            const auto now = std::chrono::steady_clock::now();
            if (!events.empty())
                lastInput = now;
            const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 1.f / 240, 0.25f);
            last = now;

            EGLint width = 0, height = 0;
            eglQuerySurface(display, surface, EGL_WIDTH, &width);
            eglQuerySurface(display, surface, EGL_HEIGHT, &height);
            io.DisplaySize = ImVec2(float(width), float(height));
            io.DeltaTime = dt;
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            UiFrame(float(width), float(height), dt);
            ImGui::Render();
            glViewport(0, 0, width, height);
            glClearColor(0.02f, 0.03f, 0.05f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            eglSwapBuffers(display, surface);
            // 60 frames a second at most (the panel may run at 120), 30 when idle: no input for
            // 3 s and nothing moving.
            const bool idle = !UiAnimating() && now - lastInput > std::chrono::seconds(3);
            std::this_thread::sleep_until(now + std::chrono::milliseconds(idle ? 33 : 16));
        }

        if (ready)
        {
            UiShutdown();
            FreeBackdrop();
            ImGui_ImplOpenGL3_Shutdown();
            for (Texture& texture : textures)
            {
                if (texture.id)
                    glDeleteTextures(1, &texture.id);
            }
            ImGui::DestroyContext();
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        eglTerminate(display);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_running = false;
            if (window)
                ANativeWindow_release(window);
            if (g_pendingWindow)
                ANativeWindow_release(g_pendingWindow);
            g_pendingWindow = nullptr;
            g_windowPending = false;
            g_handled.notify_all();
        }
        LOGFN("launcher: {}", g_request == 1 ? "starting the game" : "closed");
        if (g_request == 1)
            CallJava(env, g_playMethod, &g_playOptions);
        else
            CallJava(env, g_exitMethod, nullptr);
        g_vm->DetachCurrentThread();
    }

    bool Post(Event event)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running)
            return false;
        g_events.push_back(std::move(event));
        g_wake.notify_one();
        return true;
    }
}

const Platform& GetPlatform()
{
    return g_platform;
}

std::vector<uint8_t> ReadAsset(const char* name)
{
    std::vector<uint8_t> data;
    AAsset* asset = g_assets ? AAssetManager_open(g_assets, name, AASSET_MODE_BUFFER) : nullptr;
    if (!asset)
        return data;
    data.resize(size_t(AAsset_getLength64(asset)));
    if (AAsset_read(asset, data.data(), data.size()) != int(data.size()))
        data.clear();
    AAsset_close(asset);
    return data;
}

void RequestPlay(const std::string& options)
{
    g_playOptions = options;
    g_request = 1;
}

void RequestExit()
{
    g_request = 2;
}

void RequestDriverImport()
{
    JNIEnv* env = nullptr;
    if (g_vm && g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        CallJava(env, g_pickMethod, nullptr);
}

void AddBackdropBlur(ImDrawList* list)
{
    list->AddCallback(BlurBackdrop, nullptr);
    list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void RequestIsoImport()
{
    JNIEnv* env = nullptr;
    if (g_vm && g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        CallJava(env, g_pickIsoMethod, nullptr);
}

void RequestRestart()
{
    JNIEnv* env = nullptr;
    if (g_vm && g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        CallJava(env, g_restartMethod, nullptr);
}

#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
void RequestCopyText(const std::string& text)
{
    // NewStringUTF takes modified UTF-8: a byte that does not start a whole 1-3 byte UTF-8
    // character (or a NUL) becomes '?'.
    std::string clean;
    clean.reserve(text.size());
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 0;
        bool valid = c != 0 && length != 0 && i + length <= text.size();
        for (size_t k = 1; valid && k < length; ++k)
            valid = (static_cast<unsigned char>(text[i + k]) >> 6) == 0x2;
        if (valid)
            clean.append(text, i, length);
        else
            clean += '?';
        i += valid ? length : 1;
    }
    JNIEnv* env = nullptr;
    if (g_vm && g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        CallJava(env, g_copyMethod, &clean);
}
#endif
}

using namespace launcher;

// MainActivity.onCreate: the launcher starts; it draws once its SurfaceView has a surface.
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherStart(JNIEnv* env, jclass activity,
    jobject assets, jstring externalDir, jstring internalDir, jstring nativeLibDir, jstring version, jstring device,
    jstring locale, jfloat density, jlong availMem, jlong totalMem)
{
    if (g_running || g_vm)
        return;
    env->GetJavaVM(&g_vm);
    g_activity = static_cast<jclass>(env->NewGlobalRef(activity));
    g_playMethod = env->GetStaticMethodID(activity, "launcherPlay", "(Ljava/lang/String;)V");
    g_exitMethod = env->GetStaticMethodID(activity, "launcherExit", "()V");
    g_pickMethod = env->GetStaticMethodID(activity, "launcherPickDriver", "()V");
    if (env->ExceptionCheck())
        env->ExceptionClear();
    g_pickIsoMethod = env->GetStaticMethodID(activity, "launcherPickIso", "()V");
    g_restartMethod = env->GetStaticMethodID(activity, "launcherRestart", "()V");
    if (env->ExceptionCheck())
        env->ExceptionClear();
#if XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6
    g_copyMethod = env->GetStaticMethodID(activity, "launcherCopyText", "(Ljava/lang/String;)V");
    if (env->ExceptionCheck())
        env->ExceptionClear();
#endif
    g_assetsRef = env->NewGlobalRef(assets);
    g_assets = AAssetManager_fromJava(env, g_assetsRef);
    g_platform.externalDir = JavaString(env, externalDir);
    g_platform.internalDir = JavaString(env, internalDir);
    g_platform.nativeLibDir = JavaString(env, nativeLibDir);
    g_platform.version = JavaString(env, version);
    g_platform.device = JavaString(env, device);
    g_platform.locale = JavaString(env, locale);
    g_platform.density = density > 0 ? density : 2.75f;
    g_platform.availMem = uint64_t(std::max<jlong>(availMem, 0));
    g_platform.totalMem = uint64_t(std::max<jlong>(totalMem, 0));
    g_running = true;
    std::thread(Run).detach();
}

// The launcher's SurfaceView: a surface (created or resized) or null when it is destroyed. A null
// returns once the launcher stopped drawing to the old one (Android frees it after this call).
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherSurface(JNIEnv* env, jclass, jobject surface)
{
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_running)
    {
        if (window)
            ANativeWindow_release(window);
        return;
    }
    if (g_pendingWindow)
        ANativeWindow_release(g_pendingWindow);
    g_pendingWindow = window;
    g_windowPending = true;
    g_wake.notify_one();
    if (!window)
        g_handled.wait_for(lock, std::chrono::seconds(2), [] { return !g_windowPending || !g_running; });
}

extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherTouch(JNIEnv*, jclass, jint action,
    jfloat x, jfloat y)
{
    Post({Event::Touch, action, x, y});
}

extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherKey(JNIEnv*, jclass, jint key, jboolean down)
{
    Event event{Event::Key, key};
    event.flag = down;
    Post(std::move(event));
}

extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherGamepad(JNIEnv* env, jclass, jstring name)
{
    Event event{Event::Gamepad};
    event.text = JavaString(env, name);
    Post(std::move(event));
}

extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherInsets(JNIEnv*, jclass, jint left,
    jint top, jint right, jint bottom)
{
    Event event{Event::Insets, 0, float(left), float(top)};
    event.right = float(right);
    event.bottom = float(bottom);
    Post(std::move(event));
}

extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherDriverImported(JNIEnv* env, jclass,
    jstring message, jboolean ok)
{
    Event event{Event::DriverImported};
    event.text = JavaString(env, message);
    event.flag = ok;
    Post(std::move(event));
}

// The disc image picked for Importar ISO: its descriptor, which the launcher closes, or -1 and why.
extern "C" JNIEXPORT void JNICALL Java_com_xport_MainActivity_nativeLauncherIsoPicked(JNIEnv* env, jclass, jint fd,
    jstring message)
{
    Event event{Event::IsoPicked, fd};
    event.text = JavaString(env, message);
    if (!Post(std::move(event)) && fd >= 0)
        close(fd);
}
#endif
