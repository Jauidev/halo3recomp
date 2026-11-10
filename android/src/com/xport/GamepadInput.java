package com.xport;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

// A physical pad (Bluetooth or USB) as the Xbox 360 pad the game reads: XAMINPUT_GAMEPAD_* buttons,
// triggers 0..255, sticks -32767..32767 with Y up. MainActivity passes it the pad's key and motion
// events while the game runs and sends its state merged with the touch controls' (nativeSetPad).
// Android's layouts for Xbox, PlayStation and most pads give the right stick as AXIS_Z/AXIS_RZ and
// the triggers as AXIS_LTRIGGER/AXIS_RTRIGGER (some as AXIS_BRAKE/AXIS_GAS); pads without a layout
// may give the right stick as AXIS_RX/AXIS_RY. The d-pad comes as keys or as the hat axes.
final class GamepadInput {
    int buttons, leftTrigger, rightTrigger, lx, ly, rx, ry;

    private int keyButtons, hatButtons;
    private boolean keyLeftTrigger, keyRightTrigger;
    private int axisLeftTrigger, axisRightTrigger;

    static boolean fromPad(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int button(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return TouchControls.A;
            case KeyEvent.KEYCODE_BUTTON_B: return TouchControls.B;
            case KeyEvent.KEYCODE_BUTTON_X: return TouchControls.X;
            case KeyEvent.KEYCODE_BUTTON_Y: return TouchControls.Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return TouchControls.LEFT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1: return TouchControls.RIGHT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return TouchControls.LEFT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return TouchControls.RIGHT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_MENU: return TouchControls.START;
            // Some pads send BACK for their select/view button.
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_BACK: return TouchControls.BACK;
            case KeyEvent.KEYCODE_DPAD_UP: return TouchControls.DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return TouchControls.DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return TouchControls.DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return TouchControls.DPAD_RIGHT;
            default: return 0;
        }
    }

    boolean isPadKey(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_BUTTON_L2 || keyCode == KeyEvent.KEYCODE_BUTTON_R2
                || button(keyCode) != 0;
    }

    // A pad key: true if it is one of the pad's (then the state changed and was updated).
    boolean onKey(KeyEvent event) {
        if (!fromPad(event.getSource())) {
            return false;
        }
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        int keyCode = event.getKeyCode();
        if (keyCode == KeyEvent.KEYCODE_BUTTON_L2) {
            keyLeftTrigger = down;
        } else if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) {
            keyRightTrigger = down;
        } else {
            int bit = button(keyCode);
            if (bit == 0) {
                return false;
            }
            keyButtons = down ? keyButtons | bit : keyButtons & ~bit;
        }
        update();
        return true;
    }

    // A joystick move: sticks, triggers and hat.
    boolean onMotion(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                || event.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return false;
        }
        InputDevice device = event.getDevice();
        lx = stick(event, device, MotionEvent.AXIS_X);
        ly = -stick(event, device, MotionEvent.AXIS_Y);
        boolean zStick = device == null
                || (device.getMotionRange(MotionEvent.AXIS_Z) != null
                        && device.getMotionRange(MotionEvent.AXIS_RZ) != null);
        rx = stick(event, device, zStick ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX);
        ry = -stick(event, device, zStick ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY);
        axisLeftTrigger = trigger(Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_BRAKE)));
        axisRightTrigger = trigger(Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_GAS)));
        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hatButtons = (hatX < -0.5f ? TouchControls.DPAD_LEFT : 0)
                | (hatX > 0.5f ? TouchControls.DPAD_RIGHT : 0)
                | (hatY < -0.5f ? TouchControls.DPAD_UP : 0)
                | (hatY > 0.5f ? TouchControls.DPAD_DOWN : 0);
        update();
        return true;
    }

    // The pad went away: nothing stays pressed.
    void reset() {
        keyButtons = hatButtons = 0;
        keyLeftTrigger = keyRightTrigger = false;
        axisLeftTrigger = axisRightTrigger = 0;
        lx = ly = rx = ry = 0;
        update();
    }

    private void update() {
        buttons = keyButtons | hatButtons;
        leftTrigger = keyLeftTrigger ? 255 : axisLeftTrigger;
        rightTrigger = keyRightTrigger ? 255 : axisRightTrigger;
    }

    // -32767..32767, 0 inside the axis' flat (its resting noise).
    private static int stick(MotionEvent event, InputDevice device, int axis) {
        float value = event.getAxisValue(axis);
        InputDevice.MotionRange range = device != null ? device.getMotionRange(axis, event.getSource()) : null;
        float flat = range != null ? range.getFlat() : 0;
        if (Math.abs(value) <= flat) {
            return 0;
        }
        return Math.round(Math.max(-1, Math.min(1, value)) * 32767);
    }

    private static int trigger(float value) {
        return Math.round(Math.max(0, Math.min(1, value)) * 255);
    }
}
