// XPORT_PC_WINDOW=1 shows the game in a window on the PC and plays it with the keyboard or
// an Xbox pad. The PC build has no surface of its own (it exists to test the renderer), so this
// copies the presented frame to the window with GDI, about 30 times a second: good for judging the
// picture, not for measuring fps. Not built into the Android app.
//
// W A S D: left stick. Arrows: right stick (camera). E: RT (accelerate). Q: LT (brake). F: Y (enter
// or leave a car). Space: A. Ctrl: X (jump). C: B. R: RB (handbrake). Tab: LB. Enter: START.
// Backspace: BACK. I J K L: d-pad.
#ifdef _WIN32

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

#include <rex/ui/presenter.h>

#include "../runtime/os/touch_pad.h"

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")

namespace {

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

constexpr int kStickFull = 32767;
constexpr int kLeftDeadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
constexpr int kRightDeadzone = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;

struct KeyButton {
  int key;
  uint16_t button;
};
constexpr KeyButton kKeyButtons[] = {
    {'F', XINPUT_GAMEPAD_Y},
    {VK_SPACE, XINPUT_GAMEPAD_A},
    {VK_CONTROL, XINPUT_GAMEPAD_X},
    {'C', XINPUT_GAMEPAD_B},
    {'R', XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {VK_TAB, XINPUT_GAMEPAD_LEFT_SHOULDER},
    {VK_RETURN, XINPUT_GAMEPAD_START},
    {VK_BACK, XINPUT_GAMEPAD_BACK},
    {'I', XINPUT_GAMEPAD_DPAD_UP},
    {'K', XINPUT_GAMEPAD_DPAD_DOWN},
    {'J', XINPUT_GAMEPAD_DPAD_LEFT},
    {'L', XINPUT_GAMEPAD_DPAD_RIGHT},
};

bool Held(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

int16_t Axis(int negative, int positive) {
  return int16_t((Held(positive) ? kStickFull : 0) - (Held(negative) ? kStickFull : 0));
}

int16_t PadAxis(SHORT value, int deadzone) { return std::abs(int(value)) < deadzone ? 0 : value; }

void UpdatePad(HWND window, XInputGetStateFunction get_state) {
  os::touch_pad::State pad;
  if (GetForegroundWindow() == window) {
    for (const KeyButton& mapping : kKeyButtons) {
      if (Held(mapping.key)) pad.buttons |= mapping.button;
    }
    pad.thumbLX = Axis('A', 'D');
    pad.thumbLY = Axis('S', 'W');
    pad.thumbRX = Axis(VK_LEFT, VK_RIGHT);
    pad.thumbRY = Axis(VK_DOWN, VK_UP);
    pad.rightTrigger = Held('E') ? 255 : 0;
    pad.leftTrigger = Held('Q') ? 255 : 0;
  }
  XINPUT_STATE state{};
  if (get_state && get_state(0, &state) == ERROR_SUCCESS) {
    const XINPUT_GAMEPAD& gamepad = state.Gamepad;
    pad.buttons |= gamepad.wButtons;
    pad.leftTrigger = std::max(pad.leftTrigger, gamepad.bLeftTrigger);
    pad.rightTrigger = std::max(pad.rightTrigger, gamepad.bRightTrigger);
    if (!pad.thumbLX) pad.thumbLX = PadAxis(gamepad.sThumbLX, kLeftDeadzone);
    if (!pad.thumbLY) pad.thumbLY = PadAxis(gamepad.sThumbLY, kLeftDeadzone);
    if (!pad.thumbRX) pad.thumbRX = PadAxis(gamepad.sThumbRX, kRightDeadzone);
    if (!pad.thumbRY) pad.thumbRY = PadAxis(gamepad.sThumbRY, kRightDeadzone);
  }
  os::touch_pad::Set(pad);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CLOSE:
      TerminateProcess(GetCurrentProcess(), 0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void ResizeClient(HWND window, uint32_t width, uint32_t height) {
  RECT rect{0, 0, LONG(width), LONG(height)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
               SWP_NOMOVE | SWP_NOZORDER);
}

void Draw(HWND window, rex::ui::RawImage& image) {
  for (size_t y = 0; y < image.height; ++y) {
    uint8_t* row = image.data.data() + y * image.stride;
    for (size_t x = 0; x < image.width; ++x) {
      std::swap(row[x * 4], row[x * 4 + 2]);  // R8G8B8X8 to GDI's B8G8R8X8
    }
  }
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = LONG(image.stride / 4);
  info.bmiHeader.biHeight = -LONG(image.height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  RECT client;
  GetClientRect(window, &client);
  HDC context = GetDC(window);
  SetStretchBltMode(context, HALFTONE);
  StretchDIBits(context, 0, 0, client.right, client.bottom, 0, 0, int(image.width),
                int(image.height), image.data.data(), &info, DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(window, context);
}

}  // namespace

void PcWindowThread(std::function<rex::ui::Presenter*()> presenter) {
  SetProcessDPIAware();
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"XportPortPcWindow";
  RegisterClassW(&window_class);
  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"the title", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr,
                                window_class.hInstance, nullptr);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);

  HMODULE xinput = LoadLibraryA("xinput1_4.dll");
  const auto get_state = xinput ? reinterpret_cast<XInputGetStateFunction>(
                                      GetProcAddress(xinput, "XInputGetState"))
                                : nullptr;

  bool sized = false;
  auto next_frame = std::chrono::steady_clock::now();
  for (;;) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&message);
    }
    UpdatePad(window, get_state);
    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    next_frame = now + std::chrono::milliseconds(33);
    rex::ui::Presenter* current = presenter();
    rex::ui::RawImage image;
    if (!current || !current->CaptureGuestOutput(image)) continue;
    if (!sized) {
      ResizeClient(window, image.width, image.height);
      sized = true;
    }
    Draw(window, image);
  }
}

#endif  // _WIN32
