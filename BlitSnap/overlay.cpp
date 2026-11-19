#include "overlay.h"
#include "capture.h"
#include "toolbar.h"
#include "export.h"
#include "toast.h"
#include <windowsx.h>
#include <wchar.h>
#include <algorithm>
#include <cmath>
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")

static void InvalidateAllOverlays() {
    for (const auto& mon : g_app.overlays) {
        if (mon.hWnd) {
            InvalidateRect(mon.hWnd, nullptr, FALSE);
            UpdateWindow(mon.hWnd);
        }
    }
    RepaintToolbar();
}

void RefreshOverlayVisuals() {
    InvalidateAllOverlays();
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

static void DrawPixelLoupeOnMonitor(MonitorOverlay& mon) {
    if (!g_app.showLoupe || g_app.mode != CaptureMode::Region) return;
    if (!PtInRect(&mon.rect, g_app.ptCursor)) return;

    constexpr int kGridCount = 9;
    constexpr int kHalfGrid = kGridCount / 2;

    int dpi = static_cast<int>(mon.dpi);

    int cellPx = std::max(8, MulDiv(10, dpi, 96));
    int gridPx = kGridCount * cellPx;
    int footerH = MulDiv(34, dpi, 96);
    int boxW = gridPx + 2;
    int boxH = gridPx + footerH + 2;
    int offset = MulDiv(16, dpi, 96);

    int boxX = g_app.ptCursor.x + offset;
    int boxY = g_app.ptCursor.y + offset;
    if (boxX + boxW > mon.rect.right) {
        boxX = g_app.ptCursor.x - boxW - offset;
    }
    if (boxY + boxH > mon.rect.bottom) {
        boxY = g_app.ptCursor.y - boxH - offset;
    }

    int localX = boxX - mon.rect.left;
    int localY = boxY - mon.rect.top;

    RECT rcOuter = { localX, localY, localX + boxW, localY + boxH };
    HBRUSH hFrameBg = CreateSolidBrush(RGB(26, 26, 26));
    FillRect(mon.hBackDC, &rcOuter, hFrameBg);
    DeleteObject(hFrameBg);

    COLORREF centerRgb = RGB(0, 0, 0);

    for (int gy = 0; gy < kGridCount; ++gy) {
        int srcY = (g_app.ptCursor.y - kHalfGrid + gy) - g_app.pristine.vy;
        for (int gx = 0; gx < kGridCount; ++gx) {
            int srcX = (g_app.ptCursor.x - kHalfGrid + gx) - g_app.pristine.vx;

            COLORREF c = RGB(20, 20, 20);
            if (srcX >= 0 && srcX < g_app.pristine.width &&
                srcY >= 0 && srcY < g_app.pristine.height) {
                uint32_t bgra = g_app.pristine.pixels[static_cast<size_t>(srcY) * g_app.pristine.width + srcX];
                uint8_t r = (bgra >> 16) & 0xFFu;
                uint8_t g = (bgra >> 8) & 0xFFu;
                uint8_t b = bgra & 0xFFu;
                c = RGB(r, g, b);
            }

            if (gx == kHalfGrid && gy == kHalfGrid) {
                centerRgb = c;
            }

            RECT rcCell = {
                localX + 1 + gx * cellPx,
                localY + 1 + gy * cellPx,
                localX + 1 + (gx + 1) * cellPx,
                localY + 1 + (gy + 1) * cellPx
            };
            HBRUSH hCell = CreateSolidBrush(c);
            FillRect(mon.hBackDC, &rcCell, hCell);
            DeleteObject(hCell);
        }
    }

    HPEN hGridPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
    HGDIOBJ oldPen = SelectObject(mon.hBackDC, hGridPen);
    for (int i = 1; i < kGridCount; ++i) {
        int px = localX + 1 + i * cellPx;
        int py = localY + 1 + i * cellPx;
        MoveToEx(mon.hBackDC, px, localY + 1, nullptr);
        LineTo(mon.hBackDC, px, localY + 1 + gridPx);
        MoveToEx(mon.hBackDC, localX + 1, py, nullptr);
        LineTo(mon.hBackDC, localX + 1 + gridPx, py);
    }
    SelectObject(mon.hBackDC, oldPen);
    DeleteObject(hGridPen);

    RECT rcCenter = {
        localX + 1 + kHalfGrid * cellPx,
        localY + 1 + kHalfGrid * cellPx,
        localX + 1 + (kHalfGrid + 1) * cellPx + 1,
        localY + 1 + (kHalfGrid + 1) * cellPx + 1
    };
    HBRUSH hRetOuter = CreateSolidBrush(RGB(255, 255, 255));
    FrameRect(mon.hBackDC, &rcCenter, hRetOuter);
    DeleteObject(hRetOuter);

    InflateRect(&rcCenter, -1, -1);
    HBRUSH hRetInner = CreateSolidBrush(kAccentColor);
    FrameRect(mon.hBackDC, &rcCenter, hRetInner);
    DeleteObject(hRetInner);

    wchar_t line1[32] = {};
    wchar_t line2[32] = {};
    swprintf_s(line1, L"#%02X%02X%02X",
        GetRValue(centerRgb), GetGValue(centerRgb), GetBValue(centerRgb));

    if (g_app.isDragging && !g_app.dragCancelArm) {
        RECT rcSel = GetNormalizedSelectionRect();
        swprintf_s(line2, L"%d x %d", rcSel.right - rcSel.left, rcSel.bottom - rcSel.top);
    }
    else {
        swprintf_s(line2, L"%d, %d", g_app.ptCursor.x, g_app.ptCursor.y);
    }

    HGDIOBJ oldFont = mon.hLoupeFont ? SelectObject(mon.hBackDC, mon.hLoupeFont) : nullptr;
    SetBkMode(mon.hBackDC, TRANSPARENT);

    RECT rcText1 = { localX + 4, localY + gridPx + 3, localX + boxW - 4, localY + gridPx + footerH / 2 + 2 };
    SetTextColor(mon.hBackDC, RGB(240, 240, 240));
    DrawTextW(mon.hBackDC, line1, -1, &rcText1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT rcText2 = { localX + 4, localY + gridPx + footerH / 2 + 1, localX + boxW - 4, localY + boxH - 2 };
    SetTextColor(mon.hBackDC, RGB(155, 155, 155));
    DrawTextW(mon.hBackDC, line2, -1, &rcText2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (oldFont) SelectObject(mon.hBackDC, oldFont);

    HBRUSH hOuterBorder = CreateSolidBrush(RGB(75, 75, 75));
    FrameRect(mon.hBackDC, &rcOuter, hOuterBorder);
    DeleteObject(hOuterBorder);
}

static void DrawCancelIndicatorOnMonitor(MonitorOverlay& mon) {
    if (!g_app.isDragging) return;
    if (!PtInRect(&mon.rect, g_app.ptOrigin)) return;

    int ox = g_app.ptOrigin.x - mon.rect.left;
    int oy = g_app.ptOrigin.y - mon.rect.top;

    RECT rcDot = { ox - 3, oy - 3, ox + 4, oy + 4 };
    HBRUSH hDot = CreateSolidBrush(g_app.dragCancelArm ? RGB(232, 17, 35) : kAccentColor);
    FillRect(mon.hBackDC, &rcDot, hDot);
    DeleteObject(hDot);

    if (!g_app.hasLeftDeadzone || !g_app.dragCancelArm) return;

    int badgeW = MulDiv(128, mon.dpi, 96);
    int badgeH = MulDiv(26, mon.dpi, 96);
    int bx = ox + 12;
    int by = oy - badgeH - 10;

    int monW = mon.rect.right - mon.rect.left;
    int monH = mon.rect.bottom - mon.rect.top;
    if (bx + badgeW > monW - 8) bx = ox - badgeW - 12;
    if (by < 8)                 by = oy + 12;
    if (by + badgeH > monH - 8) by = monH - badgeH - 8;

    RECT rcBadge = { bx, by, bx + badgeW, by + badgeH };
    HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(mon.hBackDC, &rcBadge, hBg);
    DeleteObject(hBg);

    HBRUSH hBorder = CreateSolidBrush(RGB(232, 17, 35));
    FrameRect(mon.hBackDC, &rcBadge, hBorder);
    DeleteObject(hBorder);

    HGDIOBJ oldFont = mon.hBadgeFont ? SelectObject(mon.hBackDC, mon.hBadgeFont) : nullptr;
    SetBkMode(mon.hBackDC, TRANSPARENT);
    SetTextColor(mon.hBackDC, RGB(240, 240, 240));
    DrawTextW(mon.hBackDC, L"Release to cancel", -1, &rcBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont) SelectObject(mon.hBackDC, oldFont);
}

static void DrawPillSnapIndicatorsOnMonitor(MonitorOverlay& mon) {
    if (!g_app.isDraggingPill) return;

    HGDIOBJ oldFont = mon.hBadgeFont ? SelectObject(mon.hBackDC, mon.hBadgeFont) : nullptr;
    SetBkMode(mon.hBackDC, TRANSPARENT);

    for (int i = 0; i < 4; ++i) {
        RECT rcAnchor = {};
        if (!GetPillAnchorRect(i, rcAnchor)) continue;

        RECT rcVis = {};
        if (!IntersectRect(&rcVis, &rcAnchor, &mon.rect)) continue;

        RECT rcLocal = {
            rcAnchor.left - mon.rect.left,
            rcAnchor.top - mon.rect.top,
            rcAnchor.right - mon.rect.left,
            rcAnchor.bottom - mon.rect.top
        };

        bool isTarget = (i == g_app.pillSnapPreview);
        HBRUSH hFill = CreateSolidBrush(isTarget ? RGB(24, 48, 72) : RGB(28, 28, 28));
        FillRect(mon.hBackDC, &rcLocal, hFill);
        DeleteObject(hFill);

        HBRUSH hBrd = CreateSolidBrush(isTarget ? kAccentColor : RGB(95, 95, 95));
        FrameRect(mon.hBackDC, &rcLocal, hBrd);
        DeleteObject(hBrd);

        SetTextColor(mon.hBackDC, isTarget ? RGB(255, 255, 255) : RGB(160, 160, 160));
        DrawTextW(mon.hBackDC, isTarget ? L"Drop to snap here" : L"Snap zone",
            -1, &rcLocal, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (oldFont) SelectObject(mon.hBackDC, oldFont);
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

    DrawCancelIndicatorOnMonitor(mon);
    DrawPillSnapIndicatorsOnMonitor(mon);
    DrawPixelLoupeOnMonitor(mon);

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

    if (ok) {
        wchar_t pngPath[MAX_PATH] = {};
        if (GetLatestCapturePath(pngPath, MAX_PATH)) {
            ShowCaptureToast(pngPath);
        }
    }
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
        g_app.hasLeftDeadzone = false;

        SetCapture(hWnd);
        SetToolbarVisible(false);
        InvalidateAllOverlays();
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!pMon) return 0;
        POINT pt = { pMon->rect.left + GET_X_LPARAM(lParam), pMon->rect.top + GET_Y_LPARAM(lParam) };
        g_app.ptCursor = pt;

        if (g_app.hLastLoupeWnd && g_app.hLastLoupeWnd != hWnd && IsWindow(g_app.hLastLoupeWnd)) {
            HWND hPrev = g_app.hLastLoupeWnd;
            g_app.hLastLoupeWnd = hWnd;
            InvalidateRect(hPrev, nullptr, FALSE);
            UpdateWindow(hPrev);
        }
        else {
            g_app.hLastLoupeWnd = hWnd;
        }

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
            if (dx > kCancelDeadzonePx || dy > kCancelDeadzonePx) {
                g_app.hasLeftDeadzone = true;
            }
            g_app.dragCancelArm = (dx <= kCancelDeadzonePx && dy <= kCancelDeadzonePx);
            InvalidateAllOverlays();
        }
        else if (g_app.showLoupe) {
            InvalidateRect(hWnd, nullptr, FALSE);
            UpdateWindow(hWnd);
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
        break;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        dpiX = 96;
    }

    MonitorOverlay item = {};
    item.rect = mi.rcMonitor;
    item.dpi = dpiX;
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
    wcOverlay.hCursor = CreateInvertingCrosshairCursor();
    wcOverlay.lpszClassName = kOverlayWndClass;
    RegisterClassExW(&wcOverlay);
}

void BeginCaptureSession() {
    if (g_app.isCapturing) return;

    wchar_t titleBuf[256] = {};
    HWND hFg = GetForegroundWindow();
    if (hFg) {
        GetWindowTextW(hFg, titleBuf, 256);
    }
    g_app.activeWindowTitle = titleBuf;

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


        int loupeFontH = -MulDiv(8, static_cast<int>(mon.dpi), 72);
        mon.hLoupeFont = CreateFontW(loupeFontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        int badgeFontH = -MulDiv(9, static_cast<int>(mon.dpi), 72);
        mon.hBadgeFont = CreateFontW(badgeFontH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

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
    

    GetCursorPos(&g_app.ptCursor);
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

    CreateAndShowToolbar();
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

        if (mon.hLoupeFont) {
            DeleteObject(mon.hLoupeFont);
            mon.hLoupeFont = nullptr;
        }
        if (mon.hBadgeFont) {
            DeleteObject(mon.hBadgeFont);
            mon.hBadgeFont = nullptr;
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


    g_app.hLastLoupeWnd = nullptr;
    g_app.isCapturing = false;
    g_app.isDragging = false;
    g_app.dragCancelArm = false;
    g_app.hasHoveredWin = false;
    g_app.isDraggingPill = false;
    g_app.hasLeftDeadzone = false;

    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}