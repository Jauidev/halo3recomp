package com.xport;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;

// on-screen Xbox 360 pad drawn over the game (no physical pad needed). Every change is
// sent to the runtime as one pad state (MainActivity.nativeSetPad -> os::touch_pad ->
// XamInputGetState / XamInputGetKeystrokeEx in runtime/kernel/xam.cpp).
//   - Left half: floating left stick (move, steer); its centre is where the finger lands.
//   - Right half: floating right stick (camera); the centre follows the finger past the edge, so
//     dragging keeps turning.
//   - Buttons: A B X Y bottom right, RT/LT above them (both on the right, so braking works while
//     steering with the left thumb), LB/RB and L3/R3 at the top corners, the d-pad on the left
//     (the phone is d-pad up), BACK and START at the top centre.
// A finger that lands on a button keeps it pressed until it is lifted.
public class TouchControls extends View {
    // XAMINPUT_GAMEPAD_* bits.
    static final int DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008;
    static final int START = 0x0010, BACK = 0x0020, LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080;
    static final int LEFT_SHOULDER = 0x0100, RIGHT_SHOULDER = 0x0200;
    static final int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    // Pseudo-bits for the triggers (sent as 255 in the trigger values, not as buttons).
    static final int LEFT_TRIGGER = 0x10000, RIGHT_TRIGGER = 0x20000;

    interface Listener {
        void onPad(int buttons, int leftTrigger, int rightTrigger, int lx, int ly, int rx, int ry);
    }

    private static final class Button {
        final String label;
        final int bits;
        final float fx, fy, radiusDp;
        final int color;
        float cx, cy, radius;
        int pointer = -1;

        Button(String label, int bits, float fx, float fy, float radiusDp, int color) {
            this.label = label;
            this.bits = bits;
            this.fx = fx;
            this.fy = fy;
            this.radiusDp = radiusDp;
            this.color = color;
        }
    }

    private static final class Stick {
        int pointer = -1;
        float originX, originY, x, y;
        float defaultX, defaultY;
        float radius;
        boolean follow;

        float valueX() {
            return pointer < 0 ? 0 : clamp((x - originX) / radius);
        }

        float valueY() {
            return pointer < 0 ? 0 : clamp((originY - y) / radius);
        }
    }

    private final ArrayList<Button> buttons = new ArrayList<>();
    private final Stick leftStick = new Stick();
    private final Stick rightStick = new Stick();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float density;
    // The launcher's Tamaño de botones (0.85, 1, 1.2) and Vibración.
    private float scale = 1;
    private boolean haptics = true;
    private Listener listener;
    private int lastButtons = -1, lastLX, lastLY, lastRX, lastRY;
    // setStickRedrawLimit: stick moves redraw at most every this many ms (0 = on every move). Following
    // the finger at the panel's 120 Hz redrew the whole overlay, labels included, four times per
    // 30 fps game frame: ~11% of a core on the UI threads plus GPU time beside the game's (Gears of
    // War 3, POCO X6 Pro, 2026-11-03). Presses, releases and the pad state sent to the game are not
    // delayed.
    private long stickRedrawMs;
    private long lastRedraw;
    private boolean redrawPosted;
    private final Runnable delayedRedraw = new Runnable() {
        @Override
        public void run() {
            redrawPosted = false;
            lastRedraw = android.os.SystemClock.uptimeMillis();
            invalidate();
        }
    };

    public TouchControls(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2 * density);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.DEFAULT_BOLD);
        text.setColor(Color.argb(200, 255, 255, 255));

        int green = Color.rgb(80, 200, 90), red = Color.rgb(230, 70, 60);
        int blue = Color.rgb(70, 130, 240), yellow = Color.rgb(240, 200, 50);
        int gray = Color.rgb(200, 200, 200);
        // Positions as fractions of the view size, radii in dp.
        buttons.add(new Button("A", A, 0.875f, 0.86f, 30, green));
        buttons.add(new Button("B", B, 0.945f, 0.70f, 30, red));
        buttons.add(new Button("X", X, 0.805f, 0.70f, 30, blue));
        buttons.add(new Button("Y", Y, 0.875f, 0.54f, 30, yellow));
        buttons.add(new Button("RT", RIGHT_TRIGGER, 0.945f, 0.36f, 32, gray));
        buttons.add(new Button("LT", LEFT_TRIGGER, 0.805f, 0.36f, 32, gray));
        buttons.add(new Button("RB", RIGHT_SHOULDER, 0.945f, 0.13f, 26, gray));
        buttons.add(new Button("R3", RIGHT_THUMB, 0.805f, 0.13f, 22, gray));
        buttons.add(new Button("LB", LEFT_SHOULDER, 0.055f, 0.13f, 26, gray));
        buttons.add(new Button("L3", LEFT_THUMB, 0.17f, 0.13f, 22, gray));
        buttons.add(new Button("BACK", BACK, 0.43f, 0.07f, 22, gray));
        buttons.add(new Button("START", START, 0.57f, 0.07f, 22, gray));
        buttons.add(new Button("▲", DPAD_UP, 0.055f, 0.31f, 18, gray));
        buttons.add(new Button("▼", DPAD_DOWN, 0.055f, 0.53f, 18, gray));
        buttons.add(new Button("◀", DPAD_LEFT, 0.020f, 0.42f, 18, gray));
        buttons.add(new Button("▶", DPAD_RIGHT, 0.090f, 0.42f, 18, gray));
        rightStick.follow = true;
    }

    void setListener(Listener listener) {
        this.listener = listener;
    }

    void setScale(float scale) {
        this.scale = scale;
        if (getWidth() > 0) {
            onSizeChanged(getWidth(), getHeight(), getWidth(), getHeight());
            invalidate();
        }
    }

    void setHaptics(boolean haptics) {
        this.haptics = haptics;
    }

    void setStickRedrawLimit(long milliseconds) {
        stickRedrawMs = Math.max(0, milliseconds);
    }

    // At once for presses and releases; stick moves are spaced stickRedrawMs apart, the last one
    // always drawn.
    private void redraw(boolean now) {
        long time = android.os.SystemClock.uptimeMillis();
        if (now || stickRedrawMs == 0 || time - lastRedraw >= stickRedrawMs) {
            if (redrawPosted) {
                removeCallbacks(delayedRedraw);
                redrawPosted = false;
            }
            lastRedraw = time;
            invalidate();
        } else if (!redrawPosted) {
            redrawPosted = true;
            postDelayed(delayedRedraw, stickRedrawMs - (time - lastRedraw));
        }
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        for (Button button : buttons) {
            button.cx = button.fx * width;
            button.cy = button.fy * height;
            button.radius = button.radiusDp * density * scale;
        }
        leftStick.radius = 64 * density * scale;
        leftStick.defaultX = 0.17f * width;
        leftStick.defaultY = 0.72f * height;
        rightStick.radius = 48 * density * scale;
        rightStick.defaultX = 0.62f * width;
        rightStick.defaultY = 0.72f * height;
        text.setTextSize(13 * density * scale);
    }

    private static float clamp(float value) {
        return Math.max(-1, Math.min(1, value));
    }

    private Button hitButton(float x, float y) {
        Button best = null;
        float bestDistance = Float.MAX_VALUE;
        for (Button button : buttons) {
            float dx = x - button.cx, dy = y - button.cy;
            float distance = dx * dx + dy * dy;
            float reach = button.radius * 1.3f;
            if (distance <= reach * reach && distance < bestDistance) {
                best = button;
                bestDistance = distance;
            }
        }
        return best;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int pointer = event.getPointerId(index);
                float x = event.getX(index), y = event.getY(index);
                Button button = hitButton(x, y);
                if (button != null && button.pointer < 0) {
                    button.pointer = pointer;
                    if (haptics) {
                        performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                    }
                } else {
                    Stick stick = x < getWidth() * 0.5f ? leftStick : rightStick;
                    if (stick.pointer < 0) {
                        stick.pointer = pointer;
                        stick.originX = stick.x = x;
                        stick.originY = stick.y = y;
                    }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    int pointer = event.getPointerId(i);
                    for (Stick stick : new Stick[] {leftStick, rightStick}) {
                        if (stick.pointer != pointer) {
                            continue;
                        }
                        stick.x = event.getX(i);
                        stick.y = event.getY(i);
                        float dx = stick.x - stick.originX, dy = stick.y - stick.originY;
                        float length = (float) Math.sqrt(dx * dx + dy * dy);
                        if (stick.follow && length > stick.radius) {
                            // The centre follows the finger, so the stick stays at full tilt.
                            float excess = (length - stick.radius) / length;
                            stick.originX += dx * excess;
                            stick.originY += dy * excess;
                        }
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (action != MotionEvent.ACTION_CANCEL && i != index) {
                        continue;
                    }
                    int pointer = event.getPointerId(i);
                    for (Button button : buttons) {
                        if (button.pointer == pointer) {
                            button.pointer = -1;
                        }
                    }
                    if (leftStick.pointer == pointer) {
                        leftStick.pointer = -1;
                    }
                    if (rightStick.pointer == pointer) {
                        rightStick.pointer = -1;
                    }
                }
                break;
            }
            default:
                return true;
        }
        sendState(action != MotionEvent.ACTION_MOVE);
        return true;
    }

    private void sendState(boolean pressOrRelease) {
        int bits = 0;
        for (Button button : buttons) {
            if (button.pointer >= 0) {
                bits |= button.bits;
            }
        }
        int lx = Math.round(leftStick.valueX() * 32767), ly = Math.round(leftStick.valueY() * 32767);
        int rx = Math.round(rightStick.valueX() * 32767), ry = Math.round(rightStick.valueY() * 32767);
        boolean changed = bits != lastButtons || lx != lastLX || ly != lastLY || rx != lastRX
                || ry != lastRY;
        if (!changed) {
            return;
        }
        boolean buttonsChanged = bits != lastButtons;
        lastButtons = bits;
        lastLX = lx;
        lastLY = ly;
        lastRX = rx;
        lastRY = ry;
        if (listener != null) {
            listener.onPad(bits & 0xFFFF, (bits & LEFT_TRIGGER) != 0 ? 255 : 0,
                    (bits & RIGHT_TRIGGER) != 0 ? 255 : 0, lx, ly, rx, ry);
        }
        redraw(pressOrRelease || buttonsChanged);
    }

    private void drawStick(Canvas canvas, Stick stick) {
        float baseX = stick.pointer >= 0 ? stick.originX : stick.defaultX;
        float baseY = stick.pointer >= 0 ? stick.originY : stick.defaultY;
        fill.setColor(Color.argb(stick.pointer >= 0 ? 60 : 30, 255, 255, 255));
        canvas.drawCircle(baseX, baseY, stick.radius, fill);
        stroke.setColor(Color.argb(110, 255, 255, 255));
        canvas.drawCircle(baseX, baseY, stick.radius, stroke);
        float knobX = baseX + stick.valueX() * stick.radius;
        float knobY = baseY - stick.valueY() * stick.radius;
        fill.setColor(Color.argb(stick.pointer >= 0 ? 150 : 70, 255, 255, 255));
        canvas.drawCircle(knobX, knobY, stick.radius * 0.42f, fill);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        drawStick(canvas, leftStick);
        drawStick(canvas, rightStick);
        for (Button button : buttons) {
            boolean pressed = button.pointer >= 0;
            fill.setColor(Color.argb(pressed ? 170 : 55, Color.red(button.color),
                    Color.green(button.color), Color.blue(button.color)));
            canvas.drawCircle(button.cx, button.cy, button.radius, fill);
            stroke.setColor(Color.argb(pressed ? 230 : 120, 255, 255, 255));
            canvas.drawCircle(button.cx, button.cy, button.radius, stroke);
            float size = (button.label.length() > 2 ? 9 : 13) * density * scale;
            text.setTextSize(size);
            canvas.drawText(button.label, button.cx, button.cy + size * 0.35f, text);
        }
    }
}
