#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <emmintrin.h>
#include <stdint.h>
#include <wchar.h>
#include <vector>
#include <algorithm>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

constexpr UINT WM_APP_TRIGGER_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_CANCEL_CAPTURE = WM_APP + 2;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 3;

constexpr wchar_t kMutexName[] = L"BlitSnap_SingleInstance_Mutex";
constexpr wchar_t kMsgWndClass[] = L"BlitSnap_MessageOnly_Class";
constexpr wchar_t kOverlayWndClass[] = L"BlitSnap_Overlay_Class";

constexpr int      kCancelDeadzonePx = 5;
constexpr COLORREF kAccentColor = RGB(0, 120, 215);

struct DibSurface {
    HBITMAP   hBitmap = nullptr;
    HDC       hDC = nullptr;
    HGDIOBJ   hOldBmp = nullptr;
    uint32_t* pixels = nullptr;
    int       vx = 0;
    int       vy = 0;
    int       width = 0;
    int       height = 0;
};

struct MonitorOverlay {
    HWND    hWnd = nullptr;
    RECT    rect = {};
    HBITMAP hBackBmp = nullptr;
    HDC     hBackDC = nullptr;
    HGDIOBJ hOldBackBmp = nullptr;
};

static HINSTANCE                   g_hInst = nullptr;
static HWND                        g_hMsgWnd = nullptr;
static HHOOK                       g_hKeyHook = nullptr;

static DibSurface                  g_pristine = {};
static DibSurface                  g_dimmed = {};
static std::vector<MonitorOverlay> g_overlays;

static bool                        g_isCapturing = false;
static bool                        g_isDragging = false;
static bool                        g_dragCancelArm = false;
static POINT                       g_ptOrigin = {};
static POINT                       g_ptCurrent = {};

static void FreeDibSurface(DibSurface& surf) {
    if (surf.hDC) {
        if (surf.hOldBmp) SelectObject(surf.hDC, surf.hOldBmp);
        DeleteDC(surf.hDC);
        surf.hDC = nullptr;
    }
    if (surf.hBitmap) {
        DeleteObject(surf.hBitmap);
        surf.hBitmap = nullptr;
    }
    surf.hOldBmp = nullptr;
    surf.pixels = nullptr;
    surf.width = 0;
    surf.height = 0;
}

static bool CreateDibSurface(HDC hRefDC, int vx, int vy, int w, int h, DibSurface& outSurf) {
    FreeDibSurface(outSurf);
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hMemDC = CreateCompatibleDC(hRefDC);
    HBITMAP hDib = CreateDIBSection(hRefDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hMemDC || !hDib || !pBits) {
        if (hDib) DeleteObject(hDib);
        if (hMemDC) DeleteDC(hMemDC);
        return false;
    }

    outSurf.hOldBmp = SelectObject(hMemDC, hDib);
    outSurf.hDC = hMemDC;
    outSurf.hBitmap = hDib;
    outSurf.pixels = static_cast<uint32_t*>(pBits);
    outSurf.vx = vx;
    outSurf.vy = vy;
    outSurf.width = w;
    outSurf.height = h;
    return true;
}

static void DimPixelsSSE2(const uint32_t* src, uint32_t* dst, size_t pixelCount) {
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;

    for (; i + 4 <= pixelCount; i += 4) {
        __m128i px = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m128i dimmed = _mm_avg_epu8(px, zero);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), dimmed);
    }

    for (; i < pixelCount; ++i) {
        dst[i] = ((src[i] >> 1) & 0x7F7F7F7Fu) | 0xFF000000u;
    }
}

static bool CaptureAndPrepareBuffers() {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hScreenDC = GetDC(nullptr);
    if (!hScreenDC) return false;

    bool ok = CreateDibSurface(hScreenDC, vx, vy, vw, vh, g_pristine) &&
        CreateDibSurface(hScreenDC, vx, vy, vw, vh, g_dimmed);

    if (ok) {
        ok = BitBlt(g_pristine.hDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY | CAPTUREBLT) != FALSE;
    }

    ReleaseDC(nullptr, hScreenDC);

    if (!ok) {
        FreeDibSurface(g_pristine);
        FreeDibSurface(g_dimmed);
        return false;
    }

    DimPixelsSSE2(g_pristine.pixels, g_dimmed.pixels, static_cast<size_t>(vw) * static_cast<size_t>(vh));
    return true;
}

static void InvalidateAllOverlays() {
    for (const auto& mon : g_overlays) {
        if (mon.hWnd) {
            InvalidateRect(mon.hWnd, nullptr, FALSE);
            UpdateWindow(mon.hWnd);
        }
    }
}

static void EndCaptureSession() {
    if (GetCapture()) {
        ReleaseCapture();
    }

    for (auto& mon : g_overlays) {
        if (mon.hWnd) {
            DestroyWindow(mon.hWnd);
            mon.hWnd = nullptr;
        }
        if (mon.hBackDC) {
            if (mon.hOldBackBmp) SelectObject(mon.hBackDC, mon.hOldBackBmp);
            DeleteDC(mon.hBackDC);
            mon.hBackDC = nullptr;
        }
        if (mon.hBackBmp) {
            DeleteObject(mon.hBackBmp);
            mon.hBackBmp = nullptr;
        }
    }
    g_overlays.clear();

    FreeDibSurface(g_pristine);
    FreeDibSurface(g_dimmed);

    g_isCapturing = false;
    g_isDragging = false;
    g_dragCancelArm = false;

    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

static RECT GetNormalizedSelectionRect() {
    RECT rc;
    rc.left = std::min(g_ptOrigin.x, g_ptCurrent.x);
    rc.top = std::min(g_ptOrigin.y, g_ptCurrent.y);
    rc.right = std::max(g_ptOrigin.x, g_ptCurrent.x);
    rc.bottom = std::max(g_ptOrigin.y, g_ptCurrent.y);
    return rc;
}

static void PaintMonitorOverlay(MonitorOverlay& mon, HDC hPaintDC) {
    int monW = mon.rect.right - mon.rect.left;
    int monH = mon.rect.bottom - mon.rect.top;
    int srcMonX = mon.rect.left - g_dimmed.vx;
    int srcMonY = mon.rect.top - g_dimmed.vy;

    BitBlt(mon.hBackDC, 0, 0, monW, monH, g_dimmed.hDC, srcMonX, srcMonY, SRCCOPY);

    if (g_isDragging && !g_dragCancelArm) {
        RECT rcSel = GetNormalizedSelectionRect();
        RECT rcVis = {};
        if (IntersectRect(&rcVis, &rcSel, &mon.rect)) {
            int dstX = rcVis.left - mon.rect.left;
            int dstY = rcVis.top - mon.rect.top;
            int visW = rcVis.right - rcVis.left;
            int visH = rcVis.bottom - rcVis.top;

            int srcX = rcVis.left - g_pristine.vx;
            int srcY = rcVis.top - g_pristine.vy;

            BitBlt(mon.hBackDC, dstX, dstY, visW, visH, g_pristine.hDC, srcX, srcY, SRCCOPY);

            HBRUSH hBorderBrush = CreateSolidBrush(kAccentColor);
            if (rcSel.top >= mon.rect.top && rcSel.top < mon.rect.bottom) {
                RECT edge = { dstX, dstY, dstX + visW, dstY + 1 };
                FillRect(mon.hBackDC, &edge, hBorderBrush);
            }
            if (rcSel.bottom > mon.rect.top && rcSel.bottom <= mon.rect.bottom) {
                RECT edge = { dstX, dstY + visH - 1, dstX + visW, dstY + visH };
                FillRect(mon.hBackDC, &edge, hBorderBrush);
            }
            if (rcSel.left >= mon.rect.left && rcSel.left < mon.rect.right) {
                RECT edge = { dstX, dstY, dstX + 1, dstY + visH };
                FillRect(mon.hBackDC, &edge, hBorderBrush);
            }
            if (rcSel.right > mon.rect.left && rcSel.right <= mon.rect.right) {
                RECT edge = { dstX + visW - 1, dstY, dstX + visW, dstY + visH };
                FillRect(mon.hBackDC, &edge, hBorderBrush);
            }
            DeleteObject(hBorderBrush);
        }
    }

    BitBlt(hPaintDC, 0, 0, monW, monH, mon.hBackDC, 0, 0, SRCCOPY);
}

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* pMon = reinterpret_cast<MonitorOverlay*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (pMon && pMon->hBackDC) {
            PaintMonitorOverlay(*pMon, hdc);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!pMon) return 0;
        POINT pt = {
            pMon->rect.left + GET_X_LPARAM(lParam),
            pMon->rect.top + GET_Y_LPARAM(lParam)
        };

        g_ptOrigin = pt;
        g_ptCurrent = pt;
        g_isDragging = true;
        g_dragCancelArm = true;

        SetCapture(hWnd);
        InvalidateAllOverlays();
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_isDragging && pMon) {
            g_ptCurrent.x = pMon->rect.left + GET_X_LPARAM(lParam);
            g_ptCurrent.y = pMon->rect.top + GET_Y_LPARAM(lParam);

            int dx = std::abs(g_ptCurrent.x - g_ptOrigin.x);
            int dy = std::abs(g_ptCurrent.y - g_ptOrigin.y);
            g_dragCancelArm = (dx <= kCancelDeadzonePx && dy <= kCancelDeadzonePx);

            InvalidateAllOverlays();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_isDragging) {
            ReleaseCapture();
            g_isDragging = false;

            int dx = std::abs(g_ptCurrent.x - g_ptOrigin.x);
            int dy = std::abs(g_ptCurrent.y - g_ptOrigin.y);
            bool cancel = g_dragCancelArm || (dx <= kCancelDeadzonePx && dy <= kCancelDeadzonePx);

            if (cancel) {
                g_dragCancelArm = false;
                InvalidateAllOverlays();
            }
            else {
                RECT rcFinal = GetNormalizedSelectionRect();
                int selW = rcFinal.right - rcFinal.left;
                int selH = rcFinal.bottom - rcFinal.top;

                wchar_t buf[160];
                swprintf_s(buf, L"[BlitSnap] Snipped region (%d, %d) -> %dx%d\n",
                    rcFinal.left, rcFinal.top, selW, selH);
                OutputDebugStringW(buf);
                Beep(1046, 35);

                EndCaptureSession();
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED: {
        if (g_isDragging) {
            g_isDragging = false;
            g_dragCancelArm = false;
            InvalidateAllOverlays();
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            EndCaptureSession();
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    MonitorOverlay item = {};
    item.rect = mi.rcMonitor;
    g_overlays.push_back(item);
    return TRUE;
}

static void BeginCaptureSession() {
    if (g_isCapturing) return;

    if (!CaptureAndPrepareBuffers()) return;

    g_overlays.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, 0);
    if (g_overlays.empty()) {
        FreeDibSurface(g_pristine);
        FreeDibSurface(g_dimmed);
        return;
    }

    HDC hScreenDC = GetDC(nullptr);
    for (size_t i = 0; i < g_overlays.size(); ++i) {
        auto& mon = g_overlays[i];
        int monW = mon.rect.right - mon.rect.left;
        int monH = mon.rect.bottom - mon.rect.top;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = monW;
        bmi.bmiHeader.biHeight = -monH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        mon.hBackDC = CreateCompatibleDC(hScreenDC);
        mon.hBackBmp = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        mon.hOldBackBmp = SelectObject(mon.hBackDC, mon.hBackBmp);

        mon.hWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayWndClass,
            L"",
            WS_POPUP,
            mon.rect.left, mon.rect.top, monW, monH,
            nullptr, nullptr, g_hInst, nullptr
        );
        SetWindowLongPtrW(mon.hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&g_overlays[i]));
    }
    ReleaseDC(nullptr, hScreenDC);

    g_isCapturing = true;
    g_isDragging = false;
    g_dragCancelArm = false;

    for (auto& mon : g_overlays) {
        ShowWindow(mon.hWnd, SW_SHOWNA);
        UpdateWindow(mon.hWnd);
    }
}

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
            if (g_isCapturing && kb->vkCode == VK_ESCAPE) {
                if (g_hMsgWnd) PostMessageW(g_hMsgWnd, WM_APP_CANCEL_CAPTURE, 0, 0);
                return 1;
            }

            if (kb->vkCode == 'S') {
                bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
                bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

                if (winDown && shiftDown && !ctrlDown && !altDown) {
                    SuppressStartMenu();
                    if (g_hMsgWnd) PostMessageW(g_hMsgWnd, WM_APP_TRIGGER_CAPTURE, 0, 0);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hKeyHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TRIGGER_CAPTURE:
        BeginCaptureSession();
        return 0;

    case WM_APP_CANCEL_CAPTURE:
        if (g_isCapturing) EndCaptureSession();
        return 0;

    case WM_APP_SHOW_SETTINGS:
        Beep(520, 40);
        return 0;

    case WM_CLOSE:
        if (g_isCapturing) EndCaptureSession();
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
    g_hInst = hInst;

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

    WNDCLASSEXW wcMsg = {};
    wcMsg.cbSize = sizeof(wcMsg);
    wcMsg.lpfnWndProc = MsgWndProc;
    wcMsg.hInstance = hInst;
    wcMsg.lpszClassName = kMsgWndClass;
    RegisterClassExW(&wcMsg);

    WNDCLASSEXW wcOverlay = {};
    wcOverlay.cbSize = sizeof(wcOverlay);
    wcOverlay.lpfnWndProc = OverlayWndProc;
    wcOverlay.hInstance = hInst;
    wcOverlay.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wcOverlay.lpszClassName = kOverlayWndClass;
    RegisterClassExW(&wcOverlay);

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
    EndCaptureSession();
    if (hMutex) CloseHandle(hMutex);
    return 0;
}