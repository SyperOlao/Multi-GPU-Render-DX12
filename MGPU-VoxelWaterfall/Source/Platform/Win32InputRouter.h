#pragma once

#include <Windows.h>

class KeyboardDevice;
class Mousepad;

class Win32InputRouter
{
public:
    LRESULT Route(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                  KeyboardDevice& keyboard, Mousepad& mouse, bool& handled) const;
};

