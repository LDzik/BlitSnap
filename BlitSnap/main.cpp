#include "blitsnap.h"
#include "overlay.h"
#include "toolbar.h"
#include "toast.h"
#include <wchar.h>
#include <objbase.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

AppState g_app = {};

static void SuppressStartMenu() {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_NONAME;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        if ((kb->flags & LLKHF_INJECTED) == 0) {
            if (g_app.isCapturing && kb->vkCode == VK_ESCAPE) {
                if (g_app.hMsgWnd) PostMessageW(g_app.hMsgWnd, WM_APP_CANCEL_CAPTURE, 0, 0);
                return 1;
            }

            if (kb->vkCode == 'S') {
                bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
                bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

                if (winDown && shiftDown && !ctrlDown && !altDown) {
                    SuppressStartMenu();
                    if (g_app.hMsgWnd) PostMessageW(g_app.hMsgWnd, WM_APP_TRIGGER_CAPTURE, 0, 0);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_app.hKeyHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TRIGGER_CAPTURE:
        BeginCaptureSession();
        return 0;

    case WM_APP_CANCEL_CAPTURE:
        if (g_app.isCapturing) EndCaptureSession();
        return 0;

    case WM_APP_SHOW_SETTINGS:
        Beep(520, 40);
        return 0;

    case WM_APP_LAUNCH_EDITOR:
        LaunchEditorForLatestCapture();
        return 0;

    case WM_CLOSE:
        if (g_app.isCapturing) EndCaptureSession();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR pCmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_app.hInst = hInst;

    bool killRequested = (pCmdLine && wcsstr(pCmdLine, L"--kill") != nullptr);

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        bool isToastActivated = (pCmdLine && wcsstr(pCmdLine, L"-ToastActivated") != nullptr);

        if (!isToastActivated || killRequested) {
            HWND hExisting = FindWindowW(kMsgWndClass, nullptr);
            if (hExisting) {
                PostMessageW(hExisting, killRequested ? WM_CLOSE : WM_APP_SHOW_SETTINGS, 0, 0);
            }
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    if (killRequested) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSEXW wcMsg = {};
    wcMsg.cbSize = sizeof(wcMsg);
    wcMsg.lpfnWndProc = MsgWndProc;
    wcMsg.hInstance = hInst;
    wcMsg.lpszClassName = kMsgWndClass;
    RegisterClassExW(&wcMsg);

    RegisterOverlayClass(hInst);
    RegisterToolbarClass(hInst);

    g_app.hMsgWnd = CreateWindowExW(0, kMsgWndClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_app.hMsgWnd) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    InitToastSubsystem();

    g_app.hKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!g_app.hKeyHook) {
        DestroyWindow(g_app.hMsgWnd);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownToastSubsystem();
    UnhookWindowsHookEx(g_app.hKeyHook);
    EndCaptureSession();
    if (hMutex) CloseHandle(hMutex);
    CoUninitialize();
    return 0;
}