package com.xport;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// the app's only activity. It first shows the launcher (libgame's runtime/os/launcher.h:
// drawn natively on its own SurfaceView, with the touches and pad keys passed to it); its Jugar
// calls launcherPlay with the chosen options, and the game starts (libgame.so on its own native
// thread). A second SurfaceView takes the game's image (the native Vulkan renderer) and the
// runtime's status text is shown over it, refreshed twice a second. The disc goes in
// <external files>/game: the launcher's Importar ISO unpacks it there from the .iso, or
//   adb shell cp -r /data/local/tmp/xport/game /sdcard/Android/data/com.xport.<game>/files/
// Extra runtime options (they come after the launcher's, so they win):
//   adb shell am start -n com.xport.<game>/com.xport.MainActivity --es env "XPORT_AUTOPRESS=1"
// XPORT_LAUNCHER=0 among them skips the launcher (measurement scripts) and starts the game with
// the files/ switches as they are.
public class MainActivity extends Activity {
    // The game's library: the runtime and the code recompiled from the game's executable (one game
    // per APK, tools/build-apk.sh).
    private void loadGameLibrary() {
        System.loadLibrary("game");
    }

    private static native void nativeStart(String gameDir, String saveDir, String options);
    private static native String nativeStatus();
    private static native float[] nativeFrameCounter();
    private static native void nativeSetSurface(Surface surface);
    private static native void nativeSetPad(int buttons, int leftTrigger, int rightTrigger,
            int thumbLX, int thumbLY, int thumbRX, int thumbRY);
    private static native void nativeSetFocus(boolean focus);
    private static native void nativeSetBackground(boolean background);

    // The launcher (runtime/os/launcher.cpp).
    private static native void nativeLauncherStart(android.content.res.AssetManager assets,
            String externalDir, String internalDir, String nativeLibDir, String version,
            String device, String locale, float density, long availMem, long totalMem);
    private static native void nativeLauncherSurface(Surface surface);
    private static native void nativeLauncherTouch(int action, float x, float y);
    private static native void nativeLauncherKey(int key, boolean down);
    private static native void nativeLauncherGamepad(String name);
    private static native void nativeLauncherInsets(int left, int top, int right, int bottom);
    private static native void nativeLauncherDriverImported(String message, boolean ok);
    private static native void nativeLauncherIsoPicked(int fd, String message);

    // Keys the launcher understands (launcher.cpp's Key).
    private static final int KEY_UP = 1, KEY_DOWN = 2, KEY_LEFT = 3, KEY_RIGHT = 4, KEY_ACCEPT = 5,
            KEY_BACK = 6;
    private static final int PICK_DRIVER = 1, PICK_ISO = 2;

    // The activity the launcher's thread calls back into.
    private static MainActivity instance;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private String intentOptions;
    private SurfaceView launcherView;
    private int launcherPointer = -1;
    // The pad's direction as launcher keys (hat or left stick), to send each change once.
    private int padDirection;
    private InputManager inputManager;
    private InputManager.InputDeviceListener inputListener;
    private TextView text;
    // The counter over the game, top right, as the launcher's Contador de FPS says (XPORT_FPS):
    // 0 none; 1 Simple, the rate alone; 2 Detallado, a panel that adds the average and longest frame
    // of the last half second (a stutter shows there first), the game's share of the CPU, its memory
    // and the battery's temperature. The rate is green at 29+ (the 30 cap), then yellow, orange, red.
    private TextView fps;
    private int fpsMode;
    private long counterCpuJiffies, counterCpuTime;
    private int counterTick;
    private String counterSystem = "";
    // XPORT_SURFACE_FPS=N in the options: the frame rate the game's surface declares to
    // SurfaceFlinger (Android 11+). 120 (the default since 2026-10-27): a 30 fps frame is 4
    // refreshes of 8.3 ms, so one that comes late shows 41.7 ms instead of the 50 ms of a 60 Hz
    // panel and the micro stutter is half as large. 60 saves some display power. Not 30: a 30 Hz
    // panel would show a 25 fps game in uneven 33/66 ms steps. 0 declares nothing.
    private float surfaceFps;
    // The game runs (nativeStart called): focus changes are passed on from then.
    private boolean started;
    // The touch controls and when they show: XPORT_TOUCH=1 always, 0 never, 2 unless a pad is
    // connected.
    private TouchControls controls;
    private int touchMode = 1;
    // Pad 0 is the touch controls and the physical pad together (sendPad): buttons ORed, the
    // larger trigger, and for each stick the one pushed further.
    private final GamepadInput gamepad = new GamepadInput();
    private int touchButtons, touchLeftTrigger, touchRightTrigger, touchLX, touchLY, touchRX, touchRY;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        instance = this;
        loadGameLibrary();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Ask the display for its game mode: no picture post-processing (motion smoothing, image
        // enhancement) between the game's frames and the panel.
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            getWindow().setPreferMinimalPostProcessing(true);
        }
        // Draw under the camera cutout too (in landscape it left a black band along the short
        // edge: the surface was 2450 of the screen's 2712 pixels).
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(16, 18, 22));
        setContentView(root);
        hideSystemBars();

        inputManager = getSystemService(InputManager.class);
        inputListener = new InputManager.InputDeviceListener() {
            @Override
            public void onInputDeviceAdded(int deviceId) {
                gamepadsChanged();
            }

            @Override
            public void onInputDeviceRemoved(int deviceId) {
                // Whatever the pad held when it went away is released.
                gamepad.reset();
                sendPad();
                gamepadsChanged();
            }

            @Override
            public void onInputDeviceChanged(int deviceId) {
                gamepadsChanged();
            }
        };
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(inputListener, handler);
        }

        intentOptions = getIntent().getStringExtra("env");
        if (optionFloat(intentOptions, "XPORT_LAUNCHER", 1) == 0) {
            startGame("");
        } else {
            showLauncher();
        }
    }

    // ---------- the launcher ----------

    private void showLauncher() {
        launcherView = new SurfaceView(this);
        launcherView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                nativeLauncherSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                nativeLauncherSurface(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeLauncherSurface(null);
            }
        });
        // One finger drives the launcher (taps and scrolls); the others are ignored.
        launcherView.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View view, MotionEvent event) {
                int action = event.getActionMasked();
                if (action == MotionEvent.ACTION_DOWN) {
                    launcherPointer = event.getPointerId(0);
                    nativeLauncherTouch(0, event.getX(0), event.getY(0));
                    return true;
                }
                int index = event.findPointerIndex(launcherPointer);
                if (index < 0) {
                    return true;
                }
                if (action == MotionEvent.ACTION_MOVE) {
                    nativeLauncherTouch(1, event.getX(index), event.getY(index));
                } else if (action == MotionEvent.ACTION_UP || (action == MotionEvent.ACTION_POINTER_UP
                        && event.getActionIndex() == index)) {
                    nativeLauncherTouch(2, event.getX(index), event.getY(index));
                    launcherPointer = -1;
                } else if (action == MotionEvent.ACTION_CANCEL) {
                    nativeLauncherTouch(3, event.getX(index), event.getY(index));
                    launcherPointer = -1;
                }
                return true;
            }
        });
        // The camera hole: the launcher keeps its tiles clear of it.
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
                @Override
                public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                    android.view.DisplayCutout cutout = insets.getDisplayCutout();
                    if (cutout != null) {
                        nativeLauncherInsets(cutout.getSafeInsetLeft(), cutout.getSafeInsetTop(),
                                cutout.getSafeInsetRight(), cutout.getSafeInsetBottom());
                    } else {
                        nativeLauncherInsets(0, 0, 0, 0);
                    }
                    return insets;
                }
            });
        }
        root.addView(launcherView);
        root.requestApplyInsets();

        File files = getExternalFilesDir(null);
        String version = "";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            android.util.Log.w("xport", "no version name: " + e);
        }
        android.app.ActivityManager.MemoryInfo memory = new android.app.ActivityManager.MemoryInfo();
        android.app.ActivityManager activities = getSystemService(android.app.ActivityManager.class);
        if (activities != null) {
            activities.getMemoryInfo(memory);
        }
        nativeLauncherStart(getAssets(), files.getAbsolutePath(), getFilesDir().getAbsolutePath(),
                getApplicationInfo().nativeLibraryDir, version,
                android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL,
                java.util.Locale.getDefault().getLanguage(),
                getResources().getDisplayMetrics().density, memory.availMem, memory.totalMem);
        nativeLauncherGamepad(connectedGamepad());
    }

    // Called by the launcher's thread once it has stopped drawing: Jugar with these options.
    static void launcherPlay(final String options) {
        final MainActivity activity = instance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.closeLauncher();
                activity.startGame(options);
            }
        });
    }

    // The launcher was left with the back key.
    static void launcherRestart() {
        final MainActivity activity = instance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.startActivity(android.content.Intent.makeRestartActivityTask(
                        new android.content.ComponentName(activity, MainActivity.class)));
                Runtime.getRuntime().exit(0);
            }
        });
    }

    static void launcherExit() {
        final MainActivity activity = instance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.finish();
            }
        });
    }

    // The launcher's Copiar in Último error (Gears of War 3 only): the text on the phone's clipboard.
    static void launcherCopyText(final String text) {
        final MainActivity activity = instance;
        if (activity == null || !(activity.getPackageName().equals("com.xport.gears_of_war_3") || activity.getPackageName().equals("com.xport.halo3"))) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                android.content.ClipboardManager clipboard = (android.content.ClipboardManager)
                        activity.getSystemService(android.content.Context.CLIPBOARD_SERVICE);
                if (clipboard != null) {
                    clipboard.setPrimaryClip(android.content.ClipData.newPlainText("Informe de error", text));
                }
            }
        });
    }

    // The launcher's Importar driver: the system file picker for a .zip.
    static void launcherPickDriver() {
        final MainActivity activity = instance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip",
                        "application/x-zip-compressed", "application/octet-stream"});
                try {
                    activity.startActivityForResult(intent, PICK_DRIVER);
                } catch (android.content.ActivityNotFoundException e) {
                    nativeLauncherDriverImported("picker", false);
                }
            }
        });
    }

    // The launcher's Importar ISO: the system file picker for the game's disc image. Any type: the
    // providers name an .iso's type in different ways (x-iso9660-image, octet-stream...).
    static void launcherPickIso() {
        final MainActivity activity = instance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                try {
                    activity.startActivityForResult(intent, PICK_ISO);
                } catch (android.content.ActivityNotFoundException e) {
                    nativeLauncherIsoPicked(-1, "picker");
                }
            }
        });
    }

    private void closeLauncher() {
        if (launcherView != null) {
            root.removeView(launcherView);
            launcherView = null;
        }
        root.setOnApplyWindowInsetsListener(null);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        if (requestCode == PICK_ISO) {
            new Thread(new Runnable() {
                @Override
                public void run() {
                    openIso(uri);
                }
            }, "IsoOpen").start();
            return;
        }
        if (requestCode != PICK_DRIVER) {
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                importDriver(uri);
            }
        }, "DriverImport").start();
    }

    // Unpacks a driver .zip (an AdrenoTools package: meta.json and the .so it names) into
    // files/drivers/<zip name>/ in the app's internal storage, the only place its code may be
    // loaded from. Entries leaving the folder are refused. The launcher gets the driver's name, or
    // the failure as a code (path, size, meta, library, folder, move) it shows in its language.
    private void importDriver(Uri uri) {
        File drivers = new File(getFilesDir(), "drivers");
        File temp = new File(drivers, ".import");
        try {
            String name = displayName(uri);
            if (name.toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) {
                name = name.substring(0, name.length() - 4);
            }
            name = name.replaceAll("[^A-Za-z0-9._-]", "_");
            if (name.isEmpty() || name.startsWith(".")) {
                name = "driver" + name;
            }
            deleteTree(temp);
            if (!temp.mkdirs()) {
                throw new IOException("folder");
            }
            String base = temp.getCanonicalPath() + File.separator;
            long total = 0;
            byte[] buffer = new byte[1 << 16];
            try (InputStream input = getContentResolver().openInputStream(uri);
                    ZipInputStream zip = new ZipInputStream(input)) {
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    File out = new File(temp, entry.getName());
                    if (!out.getCanonicalPath().startsWith(base)) {
                        throw new IOException("path");
                    }
                    if (entry.isDirectory()) {
                        out.mkdirs();
                        continue;
                    }
                    out.getParentFile().mkdirs();
                    try (OutputStream output = new java.io.FileOutputStream(out)) {
                        int read;
                        while ((read = zip.read(buffer)) > 0) {
                            total += read;
                            if (total > 512L << 20) {
                                throw new IOException("size");
                            }
                            output.write(buffer, 0, read);
                        }
                    }
                }
            }
            // meta.json at the top, or in the zip's only folder.
            File package_ = temp;
            if (!new File(package_, "meta.json").isFile()) {
                File[] children = temp.listFiles();
                if (children != null && children.length == 1 && children[0].isDirectory()) {
                    package_ = children[0];
                }
            }
            File meta = new File(package_, "meta.json");
            if (!meta.isFile()) {
                throw new IOException("meta");
            }
            org.json.JSONObject json = new org.json.JSONObject(new String(
                    java.nio.file.Files.readAllBytes(meta.toPath()), "UTF-8"));
            String library = json.optString("libraryName", "");
            if (library.isEmpty() || !new File(package_, library).isFile()) {
                throw new IOException("library");
            }
            File target = new File(drivers, name);
            deleteTree(target);
            if (!package_.renameTo(target)) {
                throw new IOException("move");
            }
            deleteTree(temp);
            String title = json.optString("name", name);
            android.util.Log.i("xport", "driver imported: " + title + " in " + target);
            nativeLauncherDriverImported(title, true);
        } catch (Exception e) {
            deleteTree(temp);
            android.util.Log.w("xport", "driver import failed: " + e);
            nativeLauncherDriverImported(String.valueOf(e.getMessage()), false);
        }
    }

    // The picked disc image goes to the launcher as a file descriptor: it reads the .iso where it
    // is (runtime/os/launcher_iso.cpp, which closes it) instead of a copy.
    private void openIso(Uri uri) {
        try {
            android.os.ParcelFileDescriptor file = getContentResolver().openFileDescriptor(uri, "r");
            if (file == null) {
                throw new IOException("no descriptor");
            }
            nativeLauncherIsoPicked(file.detachFd(), displayName(uri));
        } catch (Exception e) {
            android.util.Log.w("xport", "the disc image could not be opened: " + e);
            nativeLauncherIsoPicked(-1, "open");
        }
    }

    private String displayName(Uri uri) {
        try (android.database.Cursor cursor = getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getString(0);
            }
        } catch (RuntimeException e) {
            android.util.Log.w("xport", "no display name for " + uri + ": " + e);
        }
        String last = uri.getLastPathSegment();
        return last == null ? "driver" : last.substring(last.lastIndexOf('/') + 1);
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    // The first connected pad's name, or "" without one. A pad has sticks: the Redmi Note 11's
    // fingerprint reader (uinput-goodix) says it is a keyboard and a gamepad, without a joystick.
    private static String connectedGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) {
                continue;
            }
            int sources = device.getSources();
            if ((sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                    && device.getMotionRange(MotionEvent.AXIS_X) != null
                    && !device.getName().startsWith("uinput")) {
                return device.getName();
            }
        }
        return "";
    }

    private void gamepadsChanged() {
        String pad = connectedGamepad();
        if (launcherView != null) {
            nativeLauncherGamepad(pad);
        }
        updateTouchControls(pad);
    }

    private void updateTouchControls(String pad) {
        if (controls == null) {
            return;
        }
        boolean shown = touchMode == 1 || (touchMode == 2 && pad.isEmpty());
        controls.setVisibility(shown ? View.VISIBLE : View.GONE);
    }

    private static int launcherKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_DPAD_UP:
                return KEY_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return KEY_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return KEY_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return KEY_RIGHT;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
                return KEY_ACCEPT;
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_ESCAPE:
                return KEY_BACK;
            default:
                return 0;
        }
    }

    // The touch controls' state merged with the physical pad's, as pad 0 of the game.
    private void sendPad() {
        if (!started) {
            return;
        }
        int lx = touchLX, ly = touchLY, rx = touchRX, ry = touchRY;
        if ((long) gamepad.lx * gamepad.lx + (long) gamepad.ly * gamepad.ly > (long) lx * lx + (long) ly * ly) {
            lx = gamepad.lx;
            ly = gamepad.ly;
        }
        if ((long) gamepad.rx * gamepad.rx + (long) gamepad.ry * gamepad.ry > (long) rx * rx + (long) ry * ry) {
            rx = gamepad.rx;
            ry = gamepad.ry;
        }
        nativeSetPad(touchButtons | gamepad.buttons, Math.max(touchLeftTrigger, gamepad.leftTrigger),
                Math.max(touchRightTrigger, gamepad.rightTrigger), lx, ly, rx, ry);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // In the game, the pad's buttons are the game's pad (a held button's repeats change nothing).
        if (launcherView == null && started && GamepadInput.fromPad(event.getSource())) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() > 0) {
                if (gamepad.isPadKey(event.getKeyCode())) {
                    return true;
                }
            } else if (gamepad.onKey(event)) {
                sendPad();
                return true;
            }
        }
        if (launcherView != null) {
            int key = launcherKey(event.getKeyCode());
            if (key != 0) {
                boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
                if (!down || event.getRepeatCount() == 0) {
                    nativeLauncherKey(key, down);
                }
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    // In the game, the pad's sticks, triggers and hat are the game's pad. In the launcher, its hat
    // (and left stick) move the focus like the d-pad keys.
    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (launcherView == null && started && gamepad.onMotion(event)) {
            sendPad();
            return true;
        }
        if (launcherView == null || (event.getSource() & InputDevice.SOURCE_JOYSTICK) == 0
                || event.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return super.onGenericMotionEvent(event);
        }
        float x = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float y = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (Math.abs(x) < 0.5f && Math.abs(y) < 0.5f) {
            x = event.getAxisValue(MotionEvent.AXIS_X);
            y = event.getAxisValue(MotionEvent.AXIS_Y);
        }
        int direction = 0;
        if (y < -0.6f) {
            direction |= 1 << KEY_UP;
        } else if (y > 0.6f) {
            direction |= 1 << KEY_DOWN;
        }
        if (x < -0.6f) {
            direction |= 1 << KEY_LEFT;
        } else if (x > 0.6f) {
            direction |= 1 << KEY_RIGHT;
        }
        for (int key = KEY_UP; key <= KEY_RIGHT; ++key) {
            int bit = 1 << key;
            if ((direction & bit) != (padDirection & bit)) {
                nativeLauncherKey(key, (direction & bit) != 0);
            }
        }
        padDirection = direction;
        return true;
    }

    // ---------- the game ----------

    // Starts the game with the launcher's options followed by the intent's.
    private void startGame(String launcherOptions) {
        String options = launcherOptions;
        if (intentOptions != null && !intentOptions.isEmpty()) {
            options = options.isEmpty() ? intentOptions : options + " " + intentOptions;
        }

        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                declareFrameRate(holder.getSurface());
                nativeSetSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                declareFrameRate(holder.getSurface());
                nativeSetSurface(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeSetSurface(null);
            }
        });

        text = new TextView(this);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextSize(11);
        text.setTextColor(Color.rgb(220, 220, 220));
        text.setShadowLayer(3, 1, 1, Color.BLACK);
        text.setPadding(32, 24, 32, 24);

        fps = new TextView(this);
        fps.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        fps.setTextSize(15);
        fps.setShadowLayer(4, 1, 1, Color.BLACK);
        fps.setGravity(android.view.Gravity.END);
        fps.setPadding(26, 12, 26, 12);

        // Modern rounded badge background for legibility over any scene/sky
        android.graphics.drawable.GradientDrawable fpsBg = new android.graphics.drawable.GradientDrawable();
        fpsBg.setColor(Color.argb(215, 14, 18, 26)); // Dark semi-transparent
        fpsBg.setCornerRadius(16);
        fpsBg.setStroke(2, Color.argb(85, 255, 255, 255)); // Subtle border
        fps.setTag(fpsBg);
        fps.setVisibility(View.GONE);

        root.addView(surface);
        root.addView(text);
        FrameLayout.LayoutParams fpsParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                android.view.Gravity.TOP | android.view.Gravity.END);
        fpsParams.setMargins(0, 28, 48, 0); // Avoid camera cutout and screen curve
        root.addView(fps, fpsParams);

        File files = getExternalFilesDir(null);
        File game = new File(files, "game");
        if (!new File(game, "default.xex").exists()) {
            text.setText("Falta el disco del juego.\n\nCópialo con:\nadb shell cp -r /data/local/tmp/xport/game " + files.getAbsolutePath() + "/\n");
            return;
        }

        surfaceFps = optionFloat(options, "XPORT_SURFACE_FPS", 120);
        // The screen's landscape size, known before the surface exists: the native renderer sizes
        // the game's device surfaces from it at boot (rexglue_glue/xd3d/d3d_hooks.cpp).
        android.graphics.Point screen = new android.graphics.Point();
        getWindowManager().getDefaultDisplay().getRealSize(screen);
        String display = "XPORT_DISPLAY_SIZE=" + Math.max(screen.x, screen.y) + "x"
                + Math.min(screen.x, screen.y);
        // The game's surface buffers are the size of the image it draws: 720p fitted to the
        // screen's shape the way the renderer fits it (Fit in rexglue_glue/xd3d/
        // xport_aspect_policy.h), 1280x575 on the 2712x1220 screen. The presenter copies it 1:1
        // and the display hardware scales it up as it composites the layer, which it already does
        // to rotate it (DEVICE composition with ROT_90, 2026-10-26). Until 0.6.0D the buffers were
        // half the screen (1356x610) and the presenter first stretched the image by 6%, a second
        // resample that blurred the text. XPORT_SURFACE_SCALE between 0 and 1 sizes them as that
        // fraction of the screen instead; 1 keeps the screen's size (the presenter scales).
        final int screenWidth = Math.max(screen.x, screen.y);
        final int screenHeight = Math.min(screen.x, screen.y);
        final float surfaceScale = optionFloat(options, "XPORT_SURFACE_SCALE", 0);
        if (surfaceScale > 0 && surfaceScale < 1) {
            surface.getHolder().setFixedSize(Math.round(screenWidth * surfaceScale),
                    Math.round(screenHeight * surfaceScale));
        } else if (surfaceScale == 0) {
            long width = 1280;
            long height = 720;
            if (getPackageName().equals("com.xport.halo3")) {
                // Halo 3 draws every frame, menus and HUD at 1152x640 whatever the surface. Fitted
                // as 720p (1280x575 here) the presenter shrank its 640 lines to 575 before the
                // display stretched them to the screen: the menu text came out thin and grey. Keep
                // the 640 lines (1422x640 on 2712x1220): the presenter copies them 1:1 and the
                // display hardware does the only scaling.
                height = 640;
                width = height * screenWidth / screenHeight;
            } else if (width * screenHeight > height * screenWidth) {
                width = height * screenWidth / screenHeight;
            } else {
                height = width * screenHeight / screenWidth;
            }
            surface.getHolder().setFixedSize((int) width, (int) height);
        }
        // Textures converted once are kept in the app's internal storage (faster than the shared
        // storage the game files are on); XPORT_TEXTURE_CACHE_DIR= in the options disables it.
        display += " XPORT_TEXTURE_CACHE_DIR="
                + new File(getFilesDir(), "texture_cache").getAbsolutePath();
        // The status text (debugging) starts hidden: XPORT_STATUS=1 shows it, and then a tap on
        // the screen outside the touch controls shows or hides it. XPORT_STATUS=0 also ignores taps
        // (measurements: the text view costs GPU time).
        final boolean statusAllowed = options.contains("XPORT_STATUS=1");
        if (!statusAllowed) {
            text.setVisibility(View.GONE);
        }
        root.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                if (!statusAllowed) {
                    return;
                }
                text.setVisibility(text.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
                android.util.Log.i("xport", "status text "
                        + (text.getVisibility() == View.VISIBLE ? "shown" : "hidden")
                        + " by a tap");
            }
        });
        // On-screen pad over the game: XPORT_TOUCH=0 hides it (e.g. for measurements), 2 hides it
        // while a pad is connected; XPORT_TOUCH_ALPHA, _SCALE and _HAPTIC come from the launcher.
        touchMode = Math.round(optionFloat(options, "XPORT_TOUCH", 1));
        if (touchMode != 0) {
            controls = new TouchControls(this);
            controls.setAlpha(Math.max(0.1f, Math.min(1, optionFloat(options, "XPORT_TOUCH_ALPHA", 1))));
            controls.setScale(Math.max(0.5f, Math.min(1.5f, optionFloat(options, "XPORT_TOUCH_SCALE", 1))));
            controls.setHaptics(optionFloat(options, "XPORT_TOUCH_HAPTIC", 1) != 0);
            // Gears of War 3: stick moves redraw the pad at most every 33 ms (the game's 30 fps),
            // XPORT_TOUCH_REDRAW_MS=N changes it (0 = every move, as the other games do).
            if (getPackageName().equals("com.xport.gears_of_war_3") || getPackageName().equals("com.xport.halo3")) {
                controls.setStickRedrawLimit(Math.round(optionFloat(options, "XPORT_TOUCH_REDRAW_MS", 33)));
            }
            controls.setListener(new TouchControls.Listener() {
                @Override
                public void onPad(int buttons, int leftTrigger, int rightTrigger, int lx, int ly,
                        int rx, int ry) {
                    touchButtons = buttons;
                    touchLeftTrigger = leftTrigger;
                    touchRightTrigger = rightTrigger;
                    touchLX = lx;
                    touchLY = ly;
                    touchRX = rx;
                    touchRY = ry;
                    sendPad();
                }
            });
            root.addView(controls);
            updateTouchControls(connectedGamepad());
        }
        nativeStart(game.getAbsolutePath(), new File(files, "save").getAbsolutePath(),
                options.isEmpty() ? display : display + " " + options);
        started = true;
        reportGameState(hasWindowFocus());
        fpsMode = Math.round(optionFloat(options, "XPORT_FPS", 1));
        if (fpsMode != 0) {
            fps.setVisibility(View.VISIBLE);
            if (fpsMode >= 2) {
                fps.setBackground((android.graphics.drawable.Drawable) fps.getTag());
            }
        }
        startCounters();
    }

    // The status text and the frame counter refresh themselves while the activity is visible.
    private void startCounters() {
        handler.removeCallbacks(refresh);
        handler.removeCallbacks(frameCounter);
        handler.post(refresh);
        if (fpsMode != 0) {
            handler.post(frameCounter);
        }
    }

    // The home screen or another app covers the game: the counters stop, and libgame holds the
    // game's threads (os/background.h) until the activity is visible again.
    @Override
    protected void onStop() {
        if (started) {
            handler.removeCallbacks(refresh);
            handler.removeCallbacks(frameCounter);
            nativeSetBackground(true);
        }
        super.onStop();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (started) {
            nativeSetBackground(false);
            startCounters();
        }
    }

    // The value of the last NAME=value in the space-separated options (the intent's come after
    // the launcher's), or fallback without one (0 if it does not parse).
    private static float optionFloat(String options, String name, float fallback) {
        if (options == null) {
            return fallback;
        }
        float value = fallback;
        for (String pair : options.split(" ")) {
            if (pair.startsWith(name + "=")) {
                try {
                    value = Float.parseFloat(pair.substring(name.length() + 1));
                } catch (NumberFormatException e) {
                    value = 0;
                }
            }
        }
        return value;
    }

    private void declareFrameRate(Surface surface) {
        if (surfaceFps <= 0 || surface == null || !surface.isValid()
                || android.os.Build.VERSION.SDK_INT < 30) {
            return;
        }
        surface.setFrameRate(surfaceFps, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        android.util.Log.i("xport", "surface frame rate declared: " + surfaceFps);
    }

    // Immersive full screen: no status or navigation bar, so edge swipes don't leave the game.
    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
        // The notification shade, the recent apps or another app took the screen: the game
        // opens its pause menu (libgame presses START), and resumes from there.
        if (started) {
            nativeSetFocus(hasFocus);
            reportGameState(hasFocus);
            android.util.Log.i("xport", "window focus " + (hasFocus ? "regained" : "lost"));
        }
    }

    // Tells Android (13+) that a game is being played, so the system and the maker's game
    // service (HyperOS Game Turbo) can give it priority; with the focus gone it can be
    // interrupted. Needs android:appCategory="game" in the manifest.
    private void reportGameState(boolean playing) {
        if (android.os.Build.VERSION.SDK_INT < 33) {
            return;
        }
        android.app.GameManager games = getSystemService(android.app.GameManager.class);
        if (games == null) {
            return;
        }
        games.setGameState(new android.app.GameState(false, playing
                ? android.app.GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE
                : android.app.GameState.MODE_GAMEPLAY_INTERRUPTIBLE));
    }

    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            text.setText(nativeStatus());
            handler.postDelayed(this, 500);
        }
    };

    private final Runnable frameCounter = new Runnable() {
        @Override
        public void run() {
            float[] values = nativeFrameCounter();
            if (values != null && values.length == 2) {
                showFrameCounter(values[0], values[1]);
            }
            handler.postDelayed(this, 500);
        }
    };

    private void showFrameCounter(float rate, float worst) {
        int color;
        if (rate <= 0.1f) {
            color = Color.rgb(180, 180, 180);
        } else if (rate >= 28.5f) {
            color = Color.rgb(0, 230, 118);
        } else if (rate >= 23.5f) {
            color = Color.rgb(255, 214, 0);
        } else if (rate >= 17.5f) {
            color = Color.rgb(255, 145, 0);
        } else {
            color = Color.rgb(255, 23, 68);
        }
        String head = rate <= 0.1f ? "-- FPS" : String.format(java.util.Locale.ROOT, "%.0f FPS", rate);
        if (fpsMode == 1) {
            fps.setText(head);
            fps.setTextColor(color);
            return;
        }
        // CPU, memory and battery change slowly, and reading them is not free: every 2 s.
        if (counterTick++ % 4 == 0) {
            counterSystem = readSystemCounters();
        }
        String frame = rate <= 0.1f ? "-- ms" : String.format(java.util.Locale.ROOT,
                "%.0f ms  max %.0f", 1000 / rate, worst);
        android.text.SpannableString panel = new android.text.SpannableString(
                head + "\n" + frame + counterSystem);
        panel.setSpan(new android.text.style.ForegroundColorSpan(color), 0, head.length(), 0);
        panel.setSpan(new android.text.style.RelativeSizeSpan(1.35f), 0, head.length(), 0);
        fps.setText(panel);
        fps.setTextColor(Color.rgb(225, 230, 238));
    }

    // "\nCPU 42 %\nRAM 3.1 GB\nBAT 38 °C": the lines it could read.
    private String readSystemCounters() {
        StringBuilder lines = new StringBuilder();
        try {
            // /proc/self/stat: utime and stime are fields 14 and 15, counted after the ")" of the name.
            String stat = readProcLine("/proc/self/stat");
            String[] fields = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
            long jiffies = Long.parseLong(fields[11]) + Long.parseLong(fields[12]);
            long now = android.os.SystemClock.elapsedRealtime();
            if (counterCpuTime != 0 && now > counterCpuTime) {
                double seconds = (now - counterCpuTime) / 1000.0;
                double ticks = android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK);
                lines.append(String.format(java.util.Locale.ROOT, "\nCPU %.0f %%",
                        100 * (jiffies - counterCpuJiffies) / (ticks * seconds * Runtime.getRuntime().availableProcessors())));
            }
            counterCpuJiffies = jiffies;
            counterCpuTime = now;
            // /proc/self/statm: resident pages are the second field.
            long pages = Long.parseLong(readProcLine("/proc/self/statm").split(" ")[1]);
            double bytes = pages * (double) android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
            lines.append(String.format(java.util.Locale.ROOT, "\nRAM %.1f GB", bytes / (1024.0 * 1024 * 1024)));
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("xport", "counter: " + e);
        }
        Intent battery = registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int tenths = battery == null ? -1 : battery.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, -1);
        if (tenths >= 0) {
            lines.append(String.format(java.util.Locale.ROOT, "\nBAT %.0f °C", tenths / 10.0));
        }
        return lines.toString();
    }

    private static String readProcLine(String path) throws IOException {
        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(path))) {
            return reader.readLine();
        }
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(frameCounter);
        handler.removeCallbacks(refresh);
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(inputListener);
        }
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }
}
