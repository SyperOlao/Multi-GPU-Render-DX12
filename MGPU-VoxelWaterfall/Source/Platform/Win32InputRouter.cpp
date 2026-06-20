#include "Source/Platform/Win32InputRouter.h"

#include "KeyboardDevice.h"
#include "Mousepad.h"

#include <memory>

LRESULT Win32InputRouter::Route(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                KeyboardDevice& keyboard, Mousepad& mouse, bool& handled) const
{
    handled = true;

    switch (msg)
    {
    case WM_INPUT:
        {
            UINT dataSize = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &dataSize,
                            sizeof(RAWINPUTHEADER));

            if (dataSize > 0)
            {
                const auto rawdata = std::make_unique<BYTE[]>(dataSize);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, rawdata.get(), &dataSize,
                                    sizeof(RAWINPUTHEADER)) == dataSize)
                {
                    auto raw = reinterpret_cast<RAWINPUT*>(rawdata.get());
                    if (raw->header.dwType == RIM_TYPEMOUSE)
                        mouse.OnMouseMoveRaw(raw->data.mouse.lLastX, raw->data.mouse.lLastY);
                }
            }

            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    case WM_MOUSEMOVE:
        mouse.OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_LBUTTONDOWN:
        mouse.OnLeftPressed(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_RBUTTONDOWN:
        mouse.OnRightPressed(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MBUTTONDOWN:
        mouse.OnMiddlePressed(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_LBUTTONUP:
        mouse.OnLeftReleased(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_RBUTTONUP:
        mouse.OnRightReleased(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MBUTTONUP:
        mouse.OnMiddleReleased(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_WHEEL_DELTA_WPARAM(wParam) > 0)
            mouse.OnWheelUp(LOWORD(lParam), HIWORD(lParam));
        else if (GET_WHEEL_DELTA_WPARAM(wParam) < 0)
            mouse.OnWheelDown(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_KEYUP:
        keyboard.OnKeyReleased(static_cast<unsigned char>(wParam));
        return 0;
    case WM_KEYDOWN:
        {
            const unsigned char keycode = static_cast<unsigned char>(wParam);
            if (keyboard.IsKeysAutoRepeat())
            {
                keyboard.OnKeyPressed(keycode);
            }
            else
            {
                const bool wasPressed = lParam & 0x40000000;
                if (!wasPressed)
                    keyboard.OnKeyPressed(keycode);
            }
        }
        [[fallthrough]];
    case WM_CHAR:
        {
            const unsigned char ch = static_cast<unsigned char>(wParam);
            if (keyboard.IsCharsAutoRepeat())
            {
                keyboard.OnChar(ch);
            }
            else
            {
                const bool wasPressed = lParam & 0x40000000;
                if (!wasPressed)
                    keyboard.OnChar(ch);
            }
            return 0;
        }
    default:
        handled = false;
        return 0;
    }
}

