#include "overlay.h"
#include "capture.h"
#include "toolbar.h"
#include "export.h"
#include <windowsx.h>
#include <wchar.h>
#include <algorithm>
#include <cmath>

static void InvalidateAllOverlays() {
    for (const auto& mon : g_app.overlays) {
        if (mon.hWnd) {
            InvalidateRect(mon.hWnd, nullptr, FALSE);
            UpdateWindow(mon.hWnd);
        }
    }
    RepaintToolbar();
}

static RECT GetNormalizedSelectionRect() {
    RECT rc;
    rc.left = std::min(g_app.ptOrigin.x, g_app.ptCurrent.x);
    rc.top = std::min(g_app.ptOrigin.y, g_app.ptCurrent.y);
    rc.right = std::max(g_app.ptOrigin.x, g_app.ptCurrent.x);
    rc.bottom = std::max(g_app.ptOrigin.y, g_app.ptCurrent.y);
    return rc;
}

static bool UpdateHoveredWindowAt(POINT pt) {
    RECT found = {};
    bool hit = false;
    for (const RECT& rc : g_app.windowRects) {
        if (PtInRect(&rc, pt)) {
            found = rc;
            hit = true;
            break;
        }
    }

    bool changed = (hit != g_app.hasHoveredWin) ||
        (hit && !EqualRect(&found, &g_app.rcHoveredWin));
    g_app.hasHoveredWin = hit;
    g_app.rcHoveredWin = found;
    return changed;
}

static void DrawSelectionBoxOnMonitor(MonitorOverlay& mon, const RECT& rcBox, int borderPx) {
    RECT rcVis = {};
    if (!IntersectRect(&rcVis, &rcBox, &mon.rect)) return;

    int dstX = rcVis.left - mon.rect.left;
    int dstY = rcVis.top - mon.rect.top;
    int visW = rcVis.right - rcVis.left;
    int visH = rcVis.bottom - rcVis.top;

    int srcX = rcVis.left - g_app.pristine.vx;
    int srcY = rcVis.top - g_app.pristine.vy;

    BitBlt(mon.hBackDC, dstX, dstY, visW, visH, g_app.pristine.hDC, srcX, srcY, SRCCOPY);

    HBRUSH hBrush = CreateSolidBrush(kAccentColor);
    if (rcBox.top >= mon.rect.top && rcBox.top < mon.rect.bottom) {
        RECT edge = { dstX, dstY, dstX + visW, std::min(dstY + borderPx, dstY + visH) };
        FillRect(mon.hBackDC, &edge, hBrush);
    }
    if (rcBox.bottom > mon.rect.top && rcBox.bottom <= mon.rect.bottom) {
        RECT edge = { dstX, std::max(dstY, dstY + visH - borderPx), dstX + visW, dstY + visH };
        FillRect(mon.hBackDC, &edge, hBrush);
    }
    if (rcBox.left >= mon.rect.left && rcBox.left < mon.rect.right) {
        RECT edge = { dstX, dstY, std::min(dstX + borderPx, dstX + visW), dstY + visH };
        FillRect(mon.hBackDC, &edge, hBrush);
    }
    if (rcBox.right > mon.rect.left && rcBox.right <= mon.rect.right) {
        RECT edge = { std::max(dstX, dstX + visW - borderPx), dstY, dstX + visW, dstY + visH };
        FillRect(mon.hBackDC, &edge, hBrush);
    }
    DeleteObject(hBrush);
}

static void PaintMonitorOverlay(MonitorOverlay& mon, HDC hPaintDC) {
    int monW = mon.rect.right - mon.rect.left;
    int monH = mon.rect.bottom - mon.rect.top;
    int srcMonX = mon.rect.left - g_app.dimmed.vx;
    int srcMonY = mon.rect.top - g_app.dimmed.vy;

    BitBlt(mon.hBackDC, 0, 0, monW, monH, g_app.dimmed.hDC, srcMonX, srcMonY, SRCCOPY);

    if (g_app.mode == CaptureMode::Region) {
        if (g_app.isDragging && !g_app.dragCancelArm) {
            DrawSelectionBoxOnMonitor(mon, GetNormalizedSelectionRect(), 1);
        }
    }
    else if (g_app.mode == CaptureMode::Window) {
        if (g_app.hasHoveredWin) {
            DrawSelectionBoxOnMonitor(mon, g_app.rcHoveredWin, 2);
        }
    }

    BitBlt(hPaintDC, 0, 0, monW, monH, mon.hBackDC, 0, 0, SRCCOPY);
}

static void CommitSelection(const RECT& rcFinal) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    bool ok = ExportCaptureToClipboardAndTemp(rcFinal);

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    wchar_t buf[160];
    swprintf_s(buf, L"[BlitSnap] Exported (%d, %d) -> %dx%d in %.2f ms (ok=%d)\n",
        rcFinal.left, rcFinal.top,
        rcFinal.right - rcFinal.left, rcFinal.bottom - rcFinal.top,
        ms, ok ? 1 : 0);
    OutputDebugStringW(buf);

    if (ok) Beep(1046, 35);
    EndCaptureSession();
}

void SetCaptureMode(CaptureMode newMode) {
    g_app.isDragging = false;
    g_app.dragCancelArm = false;
    g_app.mode = newMode;

    if (newMode == CaptureMode::Window) {
        POINT pt = {};
        GetCursorPos(&pt);
        UpdateHoveredWindowAt(pt);
    }
    else {
        g_app.hasHoveredWin = false;
    }

    InvalidateAllOverlays();
    RepaintToolbar();
}

void CaptureFullVirtualScreen() {
    RECT rcAll = {
        g_app.pristine.vx,
        g_app.pristine.vy,
        g_app.pristine.vx + g_app.pristine.width,
        g_app.pristine.vy + g_app.pristine.height
    };
    CommitSelection(rcAll);
}

static void ToggleCaptureMode(POINT) {
    SetCaptureMode(g_app.mode == CaptureMode::Region ? CaptureMode::Window : CaptureMode::Region);
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

    case WM_RBUTTONUP: {
        if (!pMon) return 0;
        POINT pt = { pMon->rect.left + GET_X_LPARAM(lParam), pMon->rect.top + GET_Y_LPARAM(lParam) };
        ToggleCaptureMode(pt);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!pMon) return 0;
        POINT pt = { pMon->rect.left + GET_X_LPARAM(lParam), pMon->rect.top + GET_Y_LPARAM(lParam) };

        if (g_app.mode == CaptureMode::Window) {
            UpdateHoveredWindowAt(pt);
            if (g_app.hasHoveredWin) {
                CommitSelection(g_app.rcHoveredWin);
            }
            return 0;
        }

        g_app.ptOrigin = pt;
        g_app.ptCurrent = pt;
        g_app.isDragging = true;
        g_app.dragCancelArm = true;

        SetCapture(hWnd);
        SetToolbarVisible(false);
        InvalidateAllOverlays();
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!pMon) return 0;
        POINT pt = { pMon->rect.left + GET_X_LPARAM(lParam), pMon->rect.top + GET_Y_LPARAM(lParam) };

        if (g_app.mode == CaptureMode::Window) {
            if (UpdateHoveredWindowAt(pt)) {
                InvalidateAllOverlays();
            }
            return 0;
        }

        if (g_app.isDragging) {
            g_app.ptCurrent = pt;
            int dx = std::abs(g_app.ptCurrent.x - g_app.ptOrigin.x);
            int dy = std::abs(g_app.ptCurrent.y - g_app.ptOrigin.y);
            g_app.dragCancelArm = (dx <= kCancelDeadzonePx && dy <= kCancelDeadzonePx);
            InvalidateAllOverlays();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_app.mode == CaptureMode::Region && g_app.isDragging) {
            ReleaseCapture();
            g_app.isDragging = false;

            int dx = std::abs(g_app.ptCurrent.x - g_app.ptOrigin.x);
            int dy = std::abs(g_app.ptCurrent.y - g_app.ptOrigin.y);
            bool cancel = g_app.dragCancelArm || (dx <= kCancelDeadzonePx && dy <= kCancelDeadzonePx);

            if (cancel) {
                g_app.dragCancelArm = false;
                SetToolbarVisible(true);
                InvalidateAllOverlays();
            }
            else {
                CommitSelection(GetNormalizedSelectionRect());
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED: {
        if (g_app.isDragging) {
            g_app.isDragging = false;
            g_app.dragCancelArm = false;
            InvalidateAllOverlays();
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            EndCaptureSession();
            return 0;
        }
        if (wParam == VK_TAB) {
            POINT pt = {};
            GetCursorPos(&pt);
            ToggleCaptureMode(pt);
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
    g_app.overlays.push_back(item);
    return TRUE;
}

static HCURSOR CreateInvertingCrosshairCursor() {
    constexpr int W = 32;
    constexpr int H = 32;

    uint8_t masks[H * 2 * 4] = {};
    memset(masks, 0xFF, H * 4);

    auto setPixel = [&](int x, int y, bool andBit, bool xorBit) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        int byteIdx = (y * 4) + (x >> 3);
        uint8_t bit = static_cast<uint8_t>(0x80u >> (x & 7));

        if (andBit) masks[byteIdx] |= bit;
        else        masks[byteIdx] &= ~bit;

        int xorIdx = ((H + y) * 4) + (x >> 3);
        if (xorBit) masks[xorIdx] |= bit;
        else        masks[xorIdx] &= ~bit;
        };

    for (int d = -11; d <= 11; ++d) {
        setPixel(16 + d, 15, true, true);
        setPixel(16 + d, 16, true, true);
        setPixel(15, 16 + d, true, true);
        setPixel(16, 16 + d, true, true);
    }

    setPixel(4, 15, false, false);  setPixel(4, 16, false, false);
    setPixel(28, 15, false, false); setPixel(28, 16, false, false);
    setPixel(15, 4, false, false);  setPixel(16, 4, false, false);
    setPixel(15, 28, false, false); setPixel(16, 28, false, false);

    HBITMAP hMask = CreateBitmap(W, H * 2, 1, 1, masks);
    if (!hMask) return LoadCursorW(nullptr, IDC_CROSS);

    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = 16;
    ii.yHotspot = 16;
    ii.hbmMask = hMask;
    ii.hbmColor = nullptr;

    HCURSOR hCur = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    return hCur ? hCur : LoadCursorW(nullptr, IDC_CROSS);
}

void RegisterOverlayClass(HINSTANCE hInst) {
    WNDCLASSEXW wcOverlay = {};
    wcOverlay.cbSize = sizeof(wcOverlay);
    wcOverlay.lpfnWndProc = OverlayWndProc;
    wcOverlay.hInstance = hInst;
    wcOverlay.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wcOverlay.lpszClassName = kOverlayWndClass;
    RegisterClassExW(&wcOverlay);
}

void BeginCaptureSession() {
    if (g_app.isCapturing) return;

    SnapshotVisibleWindows();
    if (!CaptureAndPrepareBuffers()) return;

    g_app.overlays.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, 0);
    if (g_app.overlays.empty()) {
        FreeDibSurface(g_app.pristine);
        FreeDibSurface(g_app.dimmed);
        return;
    }

    HDC hScreenDC = GetDC(nullptr);
    for (size_t i = 0; i < g_app.overlays.size(); ++i) {
        auto& mon = g_app.overlays[i];
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
            nullptr, nullptr, g_app.hInst, nullptr
        );
        SetWindowLongPtrW(mon.hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&g_app.overlays[i]));
        
    }
    CreateAndShowToolbar();

    ReleaseDC(nullptr, hScreenDC);

    g_app.isCapturing = true;
    g_app.isDragging = false;
    g_app.dragCancelArm = false;

    if (g_app.mode == CaptureMode::Window) {
        POINT pt = {};
        GetCursorPos(&pt);
        UpdateHoveredWindowAt(pt);
    }

    for (auto& mon : g_app.overlays) {
        ShowWindow(mon.hWnd, SW_SHOWNA);
        UpdateWindow(mon.hWnd);
    }
}

void EndCaptureSession() {
    if (GetCapture()) {
        ReleaseCapture();
    }

    DestroyToolbar();

    for (auto& mon : g_app.overlays) {
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
    g_app.overlays.clear();
    g_app.windowRects.clear();

    FreeDibSurface(g_app.pristine);
    FreeDibSurface(g_app.dimmed);

    g_app.isCapturing = false;
    g_app.isDragging = false;
    g_app.dragCancelArm = false;
    g_app.hasHoveredWin = false;

    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}