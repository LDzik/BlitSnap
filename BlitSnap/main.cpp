#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdint.h>
#include <wchar.h>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

constexpr UINT WM_APP_TRIGGER_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 2;

constexpr wchar_t kMutexName[] = L"BlitSnap_SingleInstance_Mutex";
constexpr wchar_t kMsgWndClass[] = L"BlitSnap_MessageOnly_Class";

struct CaptureBuffer {
    HBITMAP   hBitmap = nullptr;
    uint32_t* pixels = nullptr;
    int       vx = 0;
    int       vy = 0;
    int       width = 0;
    int       height = 0;
};

static HWND          g_hMsgWnd = nullptr;
static HHOOK         g_hKeyHook = nullptr;
static CaptureBuffer g_capture = {};

static void FreeCaptureBuffer(CaptureBuffer& cap) {
    if (cap.hBitmap) {
        DeleteObject(cap.hBitmap);
        cap.hBitmap = nullptr;
    }
    cap.pixels = nullptr;
    cap.width = 0;
    cap.height = 0;
}

static bool CaptureVirtualScreen(CaptureBuffer& outCap) {
    FreeCaptureBuffer(outCap);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);

    HBITMAP hDib = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hDib || !pBits) {
        if (hDib) DeleteObject(hDib);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }

    HGDIOBJ hOldObj = SelectObject(hMemDC, hDib);
    BOOL ok = BitBlt(hMemDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY | CAPTUREBLT);
    SelectObject(hMemDC, hOldObj);

    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    if (!ok) {
        DeleteObject(hDib);
        return false;
    }

    outCap.hBitmap = hDib;
    outCap.pixels = static_cast<uint32_t*>(pBits);
    outCap.vx = vx;
    outCap.vy = vy;
    outCap.width = vw;
    outCap.height = vh;
    return true;
}

static void SuppressStartMenu() {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_NONAME; // 0xFC
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        // Ignore our own injected dummy keystrokes
        if ((kb->flags & LLKHF_INJECTED) == 0 && kb->vkCode == 'S') {
            bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
            bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            if (winDown && shiftDown && !ctrlDown && !altDown) {
                SuppressStartMenu();
                if (g_hMsgWnd) {
                    PostMessageW(g_hMsgWnd, WM_APP_TRIGGER_CAPTURE, 0, 0);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(g_hKeyHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TRIGGER_CAPTURE: {
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        if (CaptureVirtualScreen(g_capture)) {
            QueryPerformanceCounter(&t1);
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

            wchar_t buf[160];
            swprintf_s(buf, L"[BlitSnap] Captured %dx%d at (%d,%d) in %.2f ms\n",
                g_capture.width, g_capture.height, g_capture.vx, g_capture.vy, ms);
            OutputDebugStringW(buf);
            Beep(880, 35); // Test
        }
        return 0;
    }

    case WM_APP_SHOW_SETTINGS:
        // Test
        Beep(520, 40);
        return 0;

    case WM_CLOSE:
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

    bool killRequested = (pCmdLine && wcsstr(pCmdLine, L"--kill") != nullptr);

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(kMsgWndClass, nullptr);
        if (hExisting) {
            PostMessageW(hExisting, killRequested ? WM_CLOSE : WM_APP_SHOW_SETTINGS, 0, 0);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    if (killRequested) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kMsgWndClass;
    RegisterClassExW(&wc);

    g_hMsgWnd = CreateWindowExW(0, kMsgWndClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hMsgWnd) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    g_hKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!g_hKeyHook) {
        DestroyWindow(g_hMsgWnd);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hKeyHook);
    FreeCaptureBuffer(g_capture);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}