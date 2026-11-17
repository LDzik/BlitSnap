#include "timer.h"
#include "overlay.h"
#include <wchar.h>
#include <atomic>

constexpr wchar_t kCountdownWndClass[] = L"BlitSnap_Countdown_Class";

static HWND              g_hCountdownWnd = nullptr;
static HANDLE            g_hTimerThread = nullptr;
static HANDLE            g_hStopEvent = nullptr;
static std::atomic<int>  g_secondsLeft = 0;
static bool              g_classReg = false;

static LRESULT CALLBACK CountdownWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc = {};
        GetClientRect(hWnd, &rc);

        HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        HBRUSH hBorder = CreateSolidBrush(kAccentColor);
        FrameRect(hdc, &rc, hBorder);
        DeleteObject(hBorder);

        int rem = g_secondsLeft.load();
        wchar_t numBuf[16] = {};
        swprintf_s(numBuf, L"%d", rem > 0 ? rem : 1);

        int fontH = (rc.bottom - rc.top) * 58 / 100;
        HFONT hFont = CreateFontW(-fontH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, hFont);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        DrawTextW(hdc, numBuf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);

        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static DWORD WINAPI WaitableTimerThreadProc(LPVOID) {
    HANDLE hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!hTimer) return 0;

    LARGE_INTEGER dueTime = {};
    dueTime.QuadPart = -10000000LL;
    SetWaitableTimer(hTimer, &dueTime, 1000, nullptr, nullptr, FALSE);

    HANDLE waits[2] = { g_hStopEvent, hTimer };
    while (true) {
        DWORD res = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (res != WAIT_OBJECT_0 + 1) {
            break;
        }
        int next = g_secondsLeft.fetch_sub(1) - 1;
        if (g_app.hMsgWnd) {
            PostMessageW(g_app.hMsgWnd, WM_APP_TIMER_TICK, static_cast<WPARAM>(next), 0);
        }
        if (next <= 0) {
            break;
        }
    }

    CancelWaitableTimer(hTimer);
    CloseHandle(hTimer);
    return 0;
}

static void DestroyCountdownBadge() {
    if (g_hCountdownWnd) {
        DestroyWindow(g_hCountdownWnd);
        g_hCountdownWnd = nullptr;
    }
}

bool IsDelayedCaptureActive() {
    return g_hTimerThread != nullptr;
}

void CancelDelayedCapture() {
    if (g_hStopEvent) {
        SetEvent(g_hStopEvent);
    }
    if (g_hTimerThread) {
        WaitForSingleObject(g_hTimerThread, 2000);
        CloseHandle(g_hTimerThread);
        g_hTimerThread = nullptr;
    }
    if (g_hStopEvent) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = nullptr;
    }
    g_secondsLeft = 0;
    DestroyCountdownBadge();
}

void StartDelayedCapture(int seconds) {
    CancelDelayedCapture();
    if (seconds <= 0) {
        BeginCaptureSession();
        return;
    }

    if (!g_classReg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CountdownWndProc;
        wc.hInstance = g_app.hInst;
        wc.lpszClassName = kCountdownWndClass;
        RegisterClassExW(&wc);
        g_classReg = true;
    }

    POINT origin = { 0, 0 };
    HMONITOR hMon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    if (dpi <= 0) dpi = 96;

    int boxSize = MulDiv(56, dpi, 96);
    int margin = MulDiv(18, dpi, 96);
    int x = mi.rcWork.right - boxSize - margin;
    int y = mi.rcWork.top + margin;

    g_secondsLeft = seconds;

    g_hCountdownWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kCountdownWndClass,
        L"",
        WS_POPUP,
        x, y, boxSize, boxSize,
        nullptr, nullptr, g_app.hInst, nullptr
    );
    ShowWindow(g_hCountdownWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hCountdownWnd);

    g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hTimerThread = CreateThread(nullptr, 0, WaitableTimerThreadProc, nullptr, 0, nullptr);
}

void HandleDelayedCaptureTick() {
    int rem = g_secondsLeft.load();
    if (rem > 0) {
        if (g_hCountdownWnd) {
            InvalidateRect(g_hCountdownWnd, nullptr, FALSE);
            UpdateWindow(g_hCountdownWnd);
        }
    }
    else {
        CancelDelayedCapture();
        Sleep(50);
        BeginCaptureSession();
    }
}