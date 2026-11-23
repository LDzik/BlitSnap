#include "toolbar.h"
#include "overlay.h"
#include "timer.h"
#include "config.h"
#include "layers.h"
#include "theme.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")

static HFONT g_hDelayFont = nullptr;

enum class PillAnchor {
    TopCenter = 0,
    TopRight,
    BottomCenter,
    BottomRight
};

enum PillButton {
    BTN_NONE = -1,
    BTN_GRIP = 0,
    BTN_REGION = 1,
    BTN_WINDOW = 2,
    BTN_FULLSCREEN = 3,
    BTN_LAYER_MODE = 4,
    BTN_LAYERS = 5,
    BTN_DELAY_3 = 6,
    BTN_DELAY_5 = 7,
    BTN_CLOSE = 8,
    BTN_COUNT = 9
};

static HWND       g_hToolbar = nullptr;
static PillAnchor g_anchor = PillAnchor::TopCenter;
static int        g_hoverBtn = BTN_NONE;
static int        g_pressedBtn = BTN_NONE;
static bool       g_trackingLeave = false;
static bool       g_draggingPill = false;
static POINT      g_pillDragStart = {};

static HWND  g_hLayersPopup = nullptr;
static HFONT g_hLayerTitleFont = nullptr;
static HFONT g_hLayerSubFont = nullptr;
static UINT  g_popupDpi = 96;
static int   g_popupScrollIdx = 0;
static int   g_popupHoverRow = -1;
static bool  g_popupHoverLock = false;
static bool  g_popupHoverReset = false;

constexpr int kPopupMaxVisibleRows = 9;

struct PopupLayout {
    int width = 0;
    int height = 0;
    int headerH = 0;
    int rowH = 0;
    int padX = 0;
    int visibleRows = 0;
    RECT rcShowAll{};
};

static int ScalePx(int px, UINT dpi) {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

static UINT GetPrimaryMonitorDpi(RECT& outWorkArea) {
    POINT origin = { 0, 0 };
    HMONITOR hMon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    outWorkArea = mi.rcMonitor;

    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        return 96;
    }
    return dpiX;
}

static std::vector<size_t> GetPopupDisplayLayerIndices() {
    std::vector<size_t> indices;
    indices.reserve(g_app.layers.size());
    for (size_t i = 0; i < g_app.layers.size(); ++i) {
        const WindowLayer& layer = g_app.layers[i];
        if (layer.isShellFlyout && layer.rootOwnerHwnd != nullptr) {
            bool seenGroup = false;
            for (size_t prevIdx : indices) {
                if (g_app.layers[prevIdx].isShellFlyout &&
                    g_app.layers[prevIdx].rootOwnerHwnd == layer.rootOwnerHwnd) {
                    seenGroup = true;
                    break;
                }
            }
            if (seenGroup) {
                continue;
            }
        }
        indices.push_back(i);
    }
    return indices;
}

static int CountHiddenLayers() {
    std::vector<size_t> displayIndices = GetPopupDisplayLayerIndices();
    int hidden = 0;
    for (size_t idx : displayIndices) {
        if (!g_app.layers[idx].visible) ++hidden;
    }
    return hidden;
}

static PopupLayout ComputePopupLayout(UINT dpi) {
    PopupLayout L{};
    L.width = BlitTheme::Scale(332, dpi);
    L.headerH = BlitTheme::Scale(30, dpi);
    L.rowH = BlitTheme::Scale(32, dpi);
    L.padX = BlitTheme::Scale(10, dpi);

    int totalRows = static_cast<int>(GetPopupDisplayLayerIndices().size());
    L.visibleRows = (totalRows == 0)
        ? 1
        : (std::min)(totalRows, kPopupMaxVisibleRows);

    L.height = L.headerH + L.visibleRows * L.rowH + BlitTheme::Scale(6, dpi);

    int resetW = BlitTheme::Scale(62, dpi);
    int resetH = BlitTheme::Scale(20, dpi);
    L.rcShowAll = {
        L.width - L.padX - resetW,
        (L.headerH - resetH) / 2,
        L.width - L.padX,
        (L.headerH + resetH) / 2
    };
    return L;
}

static RECT GetRowCheckboxRect(const PopupLayout& L, int visRow, UINT dpi) {
    int top = L.headerH + visRow * L.rowH;
    int box = BlitTheme::Scale(15, dpi);
    int x = L.padX;
    int y = top + (L.rowH - box) / 2;
    return { x, y, x + box, y + box };
}

static RECT GetRowPrivacyLockRect(const PopupLayout& L, int visRow, UINT dpi) {
    int top = L.headerH + visRow * L.rowH;
    int lockW = BlitTheme::Scale(26, dpi);
    int lockH = BlitTheme::Scale(20, dpi);
    int rightPad = (static_cast<int>(GetPopupDisplayLayerIndices().size()) > kPopupMaxVisibleRows)
        ? L.padX + BlitTheme::Scale(6, dpi)
        : L.padX;
    int x = L.width - rightPad - lockW;
    int y = top + (L.rowH - lockH) / 2;
    return { x, y, x + lockW, y + lockH };
}

static void PositionLayersPopupNearToolbar() {
    if (!g_hLayersPopup || !g_hToolbar) return;

    RECT rcPill{};
    GetWindowRect(g_hToolbar, &rcPill);

    HMONITOR hMon = MonitorFromWindow(g_hToolbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    UINT dpiX = 96, dpiY = 96;
    if (hMon && SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX != 0) {
        g_popupDpi = dpiX;
    }

    PopupLayout L = ComputePopupLayout(g_popupDpi);
    int gap = BlitTheme::Scale(6, g_popupDpi);

    int x = rcPill.right - L.width;
    if (x < mi.rcMonitor.left + gap) {
        x = rcPill.left;
    }
    if (x + L.width > mi.rcMonitor.right - gap) {
        x = mi.rcMonitor.right - L.width - gap;
    }

    bool expandUpward = (static_cast<int>(g_anchor) >= 2) || (rcPill.bottom + gap + L.height > mi.rcMonitor.bottom);
    int y = expandUpward
        ? (rcPill.top - gap - L.height)
        : (rcPill.bottom + gap);

    y = (std::max)(static_cast<int>(mi.rcMonitor.top + gap),
        (std::min)(y, static_cast<int>(mi.rcMonitor.bottom - L.height - gap)));

    SetWindowPos(
        g_hLayersPopup,
        HWND_TOPMOST,
        x,
        y,
        L.width,
        L.height,
        SWP_NOACTIVATE | SWP_NOZORDER);
}

static void GetButtonRects(int w, int h, UINT dpi, RECT outRects[BTN_COUNT]) {
    int pad = ScalePx(4, dpi);
    int gripW = ScalePx(14, dpi);
    int btnW = ScalePx(36, dpi);
    int gap = ScalePx(2, dpi);
    int sepGap = ScalePx(4, dpi);
    int btnH = h - pad * 2;

    int x = pad;
    outRects[BTN_GRIP] = { x, pad, x + gripW, pad + btnH }; x += gripW + gap;
    outRects[BTN_REGION] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_WINDOW] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_FULLSCREEN] = { x, pad, x + btnW,  pad + btnH }; x += btnW + sepGap;
    outRects[BTN_LAYER_MODE] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_LAYERS] = { x, pad, x + btnW,  pad + btnH }; x += btnW + sepGap;
    outRects[BTN_DELAY_3] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_DELAY_5] = { x, pad, x + btnW,  pad + btnH }; x += btnW + sepGap;
    outRects[BTN_CLOSE] = { x, pad, x + btnW,  pad + btnH };
}

bool GetPillAnchorRect(int anchorIndex, RECT& outRect) {
    RECT rcMon = {};
    UINT dpi = GetPrimaryMonitorDpi(rcMon);

    int pillW = ScalePx(332, dpi);
    int pillH = ScalePx(40, dpi);
    int margin = ScalePx(14, dpi);

    int x = rcMon.left + (rcMon.right - rcMon.left - pillW) / 2;
    int y = rcMon.top + margin;

    switch (static_cast<PillAnchor>(anchorIndex)) {
    case PillAnchor::TopCenter:
        x = rcMon.left + (rcMon.right - rcMon.left - pillW) / 2;
        y = rcMon.top + margin;
        break;
    case PillAnchor::TopRight:
        x = rcMon.right - pillW - margin;
        y = rcMon.top + margin;
        break;
    case PillAnchor::BottomCenter:
        x = rcMon.left + (rcMon.right - rcMon.left - pillW) / 2;
        y = rcMon.bottom - pillH - margin;
        break;
    case PillAnchor::BottomRight:
        x = rcMon.right - pillW - margin;
        y = rcMon.bottom - pillH - margin;
        break;
    default:
        return false;
    }

    outRect = { x, y, x + pillW, y + pillH };
    return true;
}

static void PositionToolbarAtAnchor() {
    if (!g_hToolbar) return;
    RECT rc = {};
    if (GetPillAnchorRect(static_cast<int>(g_anchor), rc)) {
        SetWindowPos(g_hToolbar, HWND_TOPMOST, rc.left, rc.top,
            rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);
    }
    if (IsLayersPopupOpen()) {
        PositionLayersPopupNearToolbar();
    }
}

static PillAnchor NearestAnchorToPoint(POINT ptScreen) {
    RECT rcMon = {};
    GetPrimaryMonitorDpi(rcMon);

    int midX = (rcMon.left + rcMon.right) / 2;
    int midY = (rcMon.top + rcMon.bottom) / 2;
    int rightThreshold = rcMon.left + ((rcMon.right - rcMon.left) * 3) / 4;

    bool isBottom = (ptScreen.y >= midY);
    bool isRight = (ptScreen.x >= rightThreshold);

    if (!isBottom && !isRight) return PillAnchor::TopCenter;
    if (!isBottom && isRight) return PillAnchor::TopRight;
    if (isBottom && !isRight) return PillAnchor::BottomCenter;
    return PillAnchor::BottomRight;
}

static int HitTestPill(POINT ptClient, int w, int h, UINT dpi) {
    RECT rects[BTN_COUNT];
    GetButtonRects(w, h, dpi, rects);
    for (int i = 0; i < BTN_COUNT; ++i) {
        if (PtInRect(&rects[i], ptClient)) return i;
    }
    return BTN_GRIP;
}

static void DrawGlyph(HDC hdc, int btn, const RECT& rc, UINT dpi) {
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    int r = ScalePx(6, dpi);
    int stroke = std::max(1, ScalePx(1, dpi));

    COLORREF fg = (btn == BTN_CLOSE && g_hoverBtn == BTN_CLOSE) ? RGB(255, 255, 255) : RGB(230, 230, 230);
    HPEN hPen = CreatePen(PS_SOLID, stroke, fg);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    switch (btn) {
    case BTN_GRIP: {
        int dot = std::max(1, ScalePx(2, dpi));
        int dx = ScalePx(3, dpi);
        int dy = ScalePx(4, dpi);
        HBRUSH hDot = CreateSolidBrush(RGB(115, 115, 115));
        for (int row = -1; row <= 1; ++row) {
            for (int col = 0; col <= 1; ++col) {
                int px = cx - dx / 2 + col * dx;
                int py = cy + row * dy;
                RECT d = { px, py, px + dot, py + dot };
                FillRect(hdc, &d, hDot);
            }
        }
        DeleteObject(hDot);
        break;
    }
    case BTN_REGION: {
        int L = cx - r, T = cy - r, R = cx + r - ScalePx(2, dpi), B = cy + r - ScalePx(2, dpi);
        MoveToEx(hdc, L, T, nullptr); LineTo(hdc, R, T);
        MoveToEx(hdc, L, T, nullptr); LineTo(hdc, L, B);
        MoveToEx(hdc, L, B, nullptr); LineTo(hdc, cx + 1, B);
        MoveToEx(hdc, R, T, nullptr); LineTo(hdc, R, cy + 1);

        int p = ScalePx(3, dpi);
        MoveToEx(hdc, cx + r - p, cy + r, nullptr); LineTo(hdc, cx + r + p, cy + r);
        MoveToEx(hdc, cx + r, cy + r - p, nullptr); LineTo(hdc, cx + r, cy + r + p);
        break;
    }
    case BTN_WINDOW: {
        int L = cx - r, T = cy - r + 1, R = cx + r, B = cy + r - 1;
        Rectangle(hdc, L, T, R + 1, B + 1);
        int barY = T + ScalePx(3, dpi);
        MoveToEx(hdc, L, barY, nullptr);
        LineTo(hdc, R, barY);
        break;
    }
    case BTN_FULLSCREEN: {
        int L = cx - r, T = cy - r, R = cx + r, B = cy + r;
        int arm = ScalePx(4, dpi);
        MoveToEx(hdc, L, T + arm, nullptr); LineTo(hdc, L, T); LineTo(hdc, L + arm + 1, T);
        MoveToEx(hdc, R - arm, T, nullptr); LineTo(hdc, R, T); LineTo(hdc, R, T + arm + 1);
        MoveToEx(hdc, L, B - arm, nullptr); LineTo(hdc, L, B); LineTo(hdc, L + arm + 1, B);
        MoveToEx(hdc, R - arm, B, nullptr); LineTo(hdc, R, B); LineTo(hdc, R, B - arm - 1);
        break;
    }
    case BTN_LAYER_MODE: {
        int off = ScalePx(3, dpi);
        int halfW = ScalePx(6, dpi);
        int halfH = ScalePx(4, dpi);

        Rectangle(hdc, cx - halfW - off / 2, cy - halfH - off, cx + halfW - off / 2, cy + halfH - off);
        Rectangle(hdc, cx - halfW + off / 2, cy - halfH + off, cx + halfW + off / 2, cy + halfH + off);
        break;
    }
    case BTN_LAYERS: {
        int halfW = BlitTheme::Scale(6, dpi);
        int stepY = BlitTheme::Scale(4, dpi);

        HPEN hListPen = CreatePen(PS_SOLID, (std::max)(1, BlitTheme::Scale(2, dpi)), fg);
        HGDIOBJ hPrevPen = SelectObject(hdc, hListPen);
        for (int row = -1; row <= 1; ++row) {
            int y = cy + row * stepY;
            MoveToEx(hdc, cx - halfW, y, nullptr);
            LineTo(hdc, cx + halfW, y);
        }
        SelectObject(hdc, hPrevPen);
        DeleteObject(hListPen);

        if (g_config.layerMode && CountHiddenLayers() > 0) {
            int dotR = BlitTheme::Scale(3, dpi);
            HBRUSH hDot = CreateSolidBrush(kAccentColor);
            HPEN hDotPen = CreatePen(PS_SOLID, 1, RGB(24, 24, 24));
            HGDIOBJ hOldB = SelectObject(hdc, hDot);
            HGDIOBJ hOldP = SelectObject(hdc, hDotPen);
            Ellipse(hdc, rc.right - dotR * 3, rc.top + dotR, rc.right - dotR, rc.top + dotR * 3);
            SelectObject(hdc, hOldB);
            SelectObject(hdc, hOldP);
            DeleteObject(hDot);
            DeleteObject(hDotPen);
        }
        break;
    }
    case BTN_DELAY_3:
    case BTN_DELAY_5: {
        const wchar_t* label = (btn == BTN_DELAY_3) ? L"3s" : L"5s";
        HGDIOBJ oldFont = g_hDelayFont ? SelectObject(hdc, g_hDelayFont) : nullptr;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);
        RECT rcTxt = rc;
        DrawTextW(hdc, label, -1, &rcTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (oldFont) SelectObject(hdc, oldFont);
        break;
    }
    case BTN_CLOSE: {
        int cr = ScalePx(5, dpi);
        MoveToEx(hdc, cx - cr, cy - cr, nullptr); LineTo(hdc, cx + cr + 1, cy + cr + 1);
        MoveToEx(hdc, cx + cr, cy - cr, nullptr); LineTo(hdc, cx - cr - 1, cy + cr + 1);
        break;
    }
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

static void PaintToolbar(HWND hWnd, HDC hPaintDC) {
    RECT rcClient = {};
    GetClientRect(hWnd, &rcClient);
    int w = rcClient.right;
    int h = rcClient.bottom;

    RECT dummy = {};
    UINT dpi = GetPrimaryMonitorDpi(dummy);

    HDC hdc = CreateCompatibleDC(hPaintDC);
    HBITMAP hBmp = CreateCompatibleBitmap(hPaintDC, w, h);
    HGDIOBJ oldBmp = SelectObject(hdc, hBmp);

    HBRUSH hBg = CreateSolidBrush(RGB(31, 31, 31));
    FillRect(hdc, &rcClient, hBg);
    DeleteObject(hBg);

    HBRUSH hBorder = CreateSolidBrush(RGB(51, 51, 51));
    FrameRect(hdc, &rcClient, hBorder);
    DeleteObject(hBorder);

    RECT rects[BTN_COUNT];
    GetButtonRects(w, h, dpi, rects);

    for (int i = 0; i < BTN_COUNT; ++i) {
        bool isActiveMode = (i == BTN_REGION && g_app.mode == CaptureMode::Region) ||
            (i == BTN_WINDOW && g_app.mode == CaptureMode::Window) ||
            (i == BTN_LAYER_MODE && g_config.layerMode) ||
            (i == BTN_LAYERS && IsLayersPopupOpen());

        if (i != BTN_GRIP) {
            COLORREF fill = 0;
            bool drawFill = false;

            if (g_pressedBtn == i) {
                fill = (i == BTN_CLOSE) ? RGB(196, 43, 28) : RGB(61, 61, 61);
                drawFill = true;
            }
            else if (g_hoverBtn == i) {
                fill = (i == BTN_CLOSE) ? RGB(232, 17, 35) : RGB(45, 45, 45);
                drawFill = true;
            }
            else if (isActiveMode) {
                fill = RGB(40, 40, 40);
                drawFill = true;
            }

            if (drawFill) {
                HBRUSH hBtnBg = CreateSolidBrush(fill);
                FillRect(hdc, &rects[i], hBtnBg);
                DeleteObject(hBtnBg);
            }

            if (isActiveMode) {
                int barH = std::max(2, ScalePx(2, dpi));
                RECT bar = {
                    rects[i].left + ScalePx(6, dpi),
                    rects[i].bottom - barH,
                    rects[i].right - ScalePx(6, dpi),
                    rects[i].bottom
                };
                HBRUSH hAccent = CreateSolidBrush(kAccentColor);
                FillRect(hdc, &bar, hAccent);
                DeleteObject(hAccent);
            }
        }

        DrawGlyph(hdc, i, rects[i], dpi);
    }

    BitBlt(hPaintDC, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdc);
}

static LRESULT CALLBACK ToolbarWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintToolbar(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MBUTTONUP: {
        g_anchor = static_cast<PillAnchor>((static_cast<int>(g_anchor) + 1) % 4);
        PositionToolbarAtAnchor();
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_trackingLeave) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingLeave = true;
        }

        if (g_draggingPill) {
            POINT ptScreen = {};
            GetCursorPos(&ptScreen);
            SetWindowPos(hWnd, HWND_TOPMOST,
                ptScreen.x - g_pillDragStart.x,
                ptScreen.y - g_pillDragStart.y,
                0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            if (IsLayersPopupOpen()) {
                PositionLayersPopupNearToolbar();
            }

            int preview = static_cast<int>(NearestAnchorToPoint(ptScreen));
            if (preview != g_app.pillSnapPreview) {
                g_app.pillSnapPreview = preview;
                RefreshOverlayVisuals();
            }
            return 0;
        }

        RECT rc = {}, dummy = {};
        GetClientRect(hWnd, &rc);
        UINT dpi = GetPrimaryMonitorDpi(dummy);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        int hit = HitTestPill(pt, rc.right, rc.bottom, dpi);
        if (hit != g_hoverBtn) {
            g_hoverBtn = hit;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        g_trackingLeave = false;
        if (g_hoverBtn != BTN_NONE) {
            g_hoverBtn = BTN_NONE;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc = {}, dummy = {};
        GetClientRect(hWnd, &rc);
        UINT dpi = GetPrimaryMonitorDpi(dummy);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        int hit = HitTestPill(pt, rc.right, rc.bottom, dpi);
        SetCapture(hWnd);

        if (hit == BTN_GRIP) {
            g_draggingPill = true;
            g_pillDragStart = pt;
            g_app.isDraggingPill = true;
            g_app.pillSnapPreview = static_cast<int>(g_anchor);
            RefreshOverlayVisuals();
        }
        else {
            g_pressedBtn = hit;
            InvalidateRect(hWnd, nullptr, FALSE);
        }

       

        return 0;
    }

    case WM_LBUTTONUP: {
        if (GetCapture() == hWnd) {
            ReleaseCapture();
        }

        if (g_draggingPill) {
            g_draggingPill = false;
            g_app.isDraggingPill = false;
            g_app.pillSnapPreview = -1;

            POINT ptScreen = {};
            GetCursorPos(&ptScreen);
            g_anchor = NearestAnchorToPoint(ptScreen);
            PositionToolbarAtAnchor();
            RefreshOverlayVisuals();
            return 0;
        }

        RECT rc = {}, dummy = {};
        GetClientRect(hWnd, &rc);
        UINT dpi = GetPrimaryMonitorDpi(dummy);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTestPill(pt, rc.right, rc.bottom, dpi);

        int clicked = (hit == g_pressedBtn) ? hit : BTN_NONE;
        g_pressedBtn = BTN_NONE;
        InvalidateRect(hWnd, nullptr, FALSE);

        switch (clicked) {
        case BTN_REGION:
            SetCaptureMode(CaptureMode::Region);
            break;
        case BTN_WINDOW:
            SetCaptureMode(CaptureMode::Window);
            break;
        case BTN_FULLSCREEN:
            CaptureFullVirtualScreen();
            break;
        case BTN_LAYER_MODE:
            if (g_app.hMsgWnd) {
                PostMessageW(g_app.hMsgWnd, WM_APP_TOGGLE_LAYER_MODE, 0, 0);
            }
            break;
        case BTN_LAYERS:
            ToggleLayersPopup();
            break;
        case BTN_DELAY_3:
            EndCaptureSession();
            StartDelayedCapture(3);
            break;
        case BTN_DELAY_5:
            EndCaptureSession();
            StartDelayedCapture(5);
            break;
        case BTN_CLOSE:
            EndCaptureSession();
            break;
        }
        return 0;
    }
    
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void PaintLayersPopup(HWND hWnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hWnd, &ps);

    RECT rcClient{};
    GetClientRect(hWnd, &rcClient);
    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom - rcClient.top;
    if (w <= 0 || h <= 0) {
        EndPaint(hWnd, &ps);
        return;
    }

    HDC hMemDC = CreateCompatibleDC(hdc);
    HBITMAP hBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ hOldBmp = SelectObject(hMemDC, hBmp);

    constexpr COLORREF kPopupBg = RGB(31, 31, 31);      // #1F1F1F
    constexpr COLORREF kHeaderBg = RGB(26, 26, 26);     // #1A1A1A
    constexpr COLORREF kRowHoverBg = RGB(45, 45, 45);   // #2D2D2D
    constexpr COLORREF kBorderCol = RGB(54, 54, 54);    // #363636
    constexpr COLORREF kTextPrimary = RGB(240, 240, 240);
    constexpr COLORREF kTextMuted = RGB(136, 136, 136);
    constexpr COLORREF kLockActiveBg = RGB(196, 43, 28);
    constexpr COLORREF kLockIdleBg = RGB(44, 44, 44);

    HBRUSH hBgBrush = CreateSolidBrush(kPopupBg);
    FillRect(hMemDC, &rcClient, hBgBrush);
    DeleteObject(hBgBrush);

    PopupLayout L = ComputePopupLayout(g_popupDpi);

    RECT rcHeader{ 0, 0, w, L.headerH };
    HBRUSH hHdrBrush = CreateSolidBrush(kHeaderBg);
    FillRect(hMemDC, &rcHeader, hHdrBrush);
    DeleteObject(hHdrBrush);

    HPEN hDivPen = CreatePen(PS_SOLID, 1, kBorderCol);
    HGDIOBJ hOldPen = SelectObject(hMemDC, hDivPen);
    MoveToEx(hMemDC, 0, L.headerH - 1, nullptr);
    LineTo(hMemDC, w, L.headerH - 1);

    SetBkMode(hMemDC, TRANSPARENT);
    HGDIOBJ hOldFont = SelectObject(hMemDC, g_hLayerTitleFont);

    std::vector<size_t> displayIndices = GetPopupDisplayLayerIndices();
    int totalCount = static_cast<int>(displayIndices.size());
    int hiddenCount = CountHiddenLayers();
    int visibleCount = totalCount - hiddenCount;

    wchar_t headerText[96]{};
    swprintf_s(headerText, L"Layers (%d/%d visible)", visibleCount, totalCount);
    SetTextColor(hMemDC, kTextPrimary);
    RECT rcHdrText{ L.padX, 0, L.rcShowAll.left - BlitTheme::Scale(6, g_popupDpi), L.headerH };
    DrawTextW(hMemDC, headerText, -1, &rcHdrText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (hiddenCount > 0) {
        HBRUSH hResetBrush = CreateSolidBrush(g_popupHoverReset ? kAccentColor : RGB(48, 48, 48));
        HGDIOBJ hOldBrush = SelectObject(hMemDC, hResetBrush);
        RoundRect(hMemDC, L.rcShowAll.left, L.rcShowAll.top, L.rcShowAll.right, L.rcShowAll.bottom,
            BlitTheme::Scale(4, g_popupDpi), BlitTheme::Scale(4, g_popupDpi));
        SelectObject(hMemDC, hOldBrush);
        DeleteObject(hResetBrush);

        SelectObject(hMemDC, g_hLayerSubFont);
        SetTextColor(hMemDC, kTextPrimary);
        DrawTextW(hMemDC, L"Show All", -1, &L.rcShowAll, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (totalCount == 0) {
        SelectObject(hMemDC, g_hLayerTitleFont);
        SetTextColor(hMemDC, kTextMuted);
        RECT rcEmpty{ L.padX, L.headerH, w - L.padX, L.headerH + L.rowH };
        DrawTextW(hMemDC, L"No capturable windows found", -1, &rcEmpty,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    else {
        int maxScroll = (std::max)(0, totalCount - kPopupMaxVisibleRows);
        g_popupScrollIdx = (std::max)(0, (std::min)(g_popupScrollIdx, maxScroll));

        for (int visRow = 0; visRow < L.visibleRows; ++visRow) {
            int idx = g_popupScrollIdx + visRow;
            if (idx >= totalCount) break;

            size_t layerIdx = displayIndices[idx];
            const WindowLayer& layer = g_app.layers[layerIdx];
            int rowTop = L.headerH + visRow * L.rowH;
            RECT rcRow{ 1, rowTop, w - 1, rowTop + L.rowH };

            if (g_popupHoverRow == idx) {
                HBRUSH hHover = CreateSolidBrush(kRowHoverBg);
                FillRect(hMemDC, &rcRow, hHover);
                DeleteObject(hHover);
            }

            RECT rcChk = GetRowCheckboxRect(L, visRow, g_popupDpi);
            if (layer.rootOwnerHwnd != nullptr && !layer.isShellFlyout) {
                int indent = BlitTheme::Scale(8, g_popupDpi);
                rcChk.left += indent;
                rcChk.right += indent;
            }

            if (layer.visible) {
                HBRUSH hChkFill = CreateSolidBrush(kAccentColor);
                HGDIOBJ hOldBrush = SelectObject(hMemDC, hChkFill);
                HPEN hChkPen = CreatePen(PS_SOLID, 1, kAccentColor);
                SelectObject(hMemDC, hChkPen);
                RoundRect(hMemDC, rcChk.left, rcChk.top, rcChk.right, rcChk.bottom,
                    BlitTheme::Scale(3, g_popupDpi), BlitTheme::Scale(3, g_popupDpi));
                SelectObject(hMemDC, hOldBrush);
                DeleteObject(hChkFill);

                HPEN hTickPen = CreatePen(PS_SOLID, (std::max)(1, BlitTheme::Scale(2, g_popupDpi)), RGB(255, 255, 255));
                SelectObject(hMemDC, hTickPen);
                int cw = rcChk.right - rcChk.left;
                int ch = rcChk.bottom - rcChk.top;
                MoveToEx(hMemDC, rcChk.left + cw * 22 / 100, rcChk.top + ch * 52 / 100, nullptr);
                LineTo(hMemDC, rcChk.left + cw * 44 / 100, rcChk.top + ch * 74 / 100);
                LineTo(hMemDC, rcChk.left + cw * 80 / 100, rcChk.top + ch * 28 / 100);
                SelectObject(hMemDC, hDivPen);
                DeleteObject(hChkPen);
                DeleteObject(hTickPen);
            }
            else {
                HBRUSH hEmptyBox = CreateSolidBrush(RGB(24, 24, 24));
                HGDIOBJ hOldBrush = SelectObject(hMemDC, hEmptyBox);
                HPEN hBoxPen = CreatePen(PS_SOLID, 1, RGB(96, 96, 96));
                SelectObject(hMemDC, hBoxPen);
                RoundRect(hMemDC, rcChk.left, rcChk.top, rcChk.right, rcChk.bottom,
                    BlitTheme::Scale(3, g_popupDpi), BlitTheme::Scale(3, g_popupDpi));
                SelectObject(hMemDC, hOldBrush);
                SelectObject(hMemDC, hDivPen);
                DeleteObject(hEmptyBox);
                DeleteObject(hBoxPen);
            }

            RECT rcLock = GetRowPrivacyLockRect(L, visRow, g_popupDpi);
            bool noPrivacyLock = layer.isTaskbar || layer.isDesktopIcons;
            bool isLocked = !noPrivacyLock && IsProcessHiddenInConfig(layer.processName);

            if (!noPrivacyLock) {
                bool isLockHovered = (g_popupHoverRow == idx && g_popupHoverLock);

                COLORREF lockFill = isLocked
                    ? kLockActiveBg
                    : (isLockHovered ? RGB(66, 66, 66) : kLockIdleBg);
                COLORREF lockBorder = isLocked
                    ? kLockActiveBg
                    : (isLockHovered ? RGB(130, 130, 130) : RGB(72, 72, 72));

                HBRUSH hLockBrush = CreateSolidBrush(lockFill);
                HPEN hLockPen = CreatePen(PS_SOLID, 1, lockBorder);
                HGDIOBJ hOldBrush = SelectObject(hMemDC, hLockBrush);
                SelectObject(hMemDC, hLockPen);
                RoundRect(hMemDC, rcLock.left, rcLock.top, rcLock.right, rcLock.bottom,
                    BlitTheme::Scale(4, g_popupDpi), BlitTheme::Scale(4, g_popupDpi));
                SelectObject(hMemDC, hOldBrush);
                SelectObject(hMemDC, hDivPen);
                DeleteObject(hLockBrush);
                DeleteObject(hLockPen);

                SelectObject(hMemDC, g_hLayerSubFont);
                SetTextColor(hMemDC, isLocked ? RGB(255, 255, 255) : kTextMuted);
                DrawTextW(hMemDC, L"[P]", -1, &rcLock, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            int textLeft = rcChk.right + BlitTheme::Scale(8, g_popupDpi);
            int textRight = (noPrivacyLock ? rcLock.right : rcLock.left) - BlitTheme::Scale(8, g_popupDpi);

            if (!layer.processName.empty() && textRight - textLeft > BlitTheme::Scale(110, g_popupDpi)) {
                int procW = BlitTheme::Scale(84, g_popupDpi);
                RECT rcProc{ textRight - procW, rowTop, textRight, rowTop + L.rowH };
                SelectObject(hMemDC, g_hLayerSubFont);
                SetTextColor(hMemDC, isLocked ? RGB(242, 139, 130) : kTextMuted);
                DrawTextW(hMemDC, layer.processName.c_str(), -1, &rcProc,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                textRight = rcProc.left - BlitTheme::Scale(6, g_popupDpi);
            }

            if (textRight > textLeft) {
                RECT rcTitle{ textLeft, rowTop, textRight, rowTop + L.rowH };
                SelectObject(hMemDC, g_hLayerTitleFont);
                SetTextColor(hMemDC, layer.visible ? kTextPrimary : kTextMuted);
                DrawTextW(hMemDC, layer.title.c_str(), -1, &rcTitle,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }

        if (totalCount > kPopupMaxVisibleRows) {
            int trackTop = L.headerH + BlitTheme::Scale(4, g_popupDpi);
            int trackBot = h - BlitTheme::Scale(6, g_popupDpi);
            int trackH = trackBot - trackTop;
            int thumbH = (std::max)(BlitTheme::Scale(18, g_popupDpi), trackH * kPopupMaxVisibleRows / totalCount);
            int thumbY = trackTop + (trackH - thumbH) * g_popupScrollIdx / maxScroll;

            RECT rcThumb{
                w - BlitTheme::Scale(5, g_popupDpi),
                thumbY,
                w - BlitTheme::Scale(2, g_popupDpi),
                thumbY + thumbH
            };
            HBRUSH hThumb = CreateSolidBrush(RGB(96, 96, 96));
            FillRect(hMemDC, &rcThumb, hThumb);
            DeleteObject(hThumb);
        }
    }

    HGDIOBJ hNullBrush = SelectObject(hMemDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hMemDC, 0, 0, w, h);
    SelectObject(hMemDC, hNullBrush);

    SelectObject(hMemDC, hOldFont);
    SelectObject(hMemDC, hOldPen);
    DeleteObject(hDivPen);

    BitBlt(hdc, 0, 0, w, h, hMemDC, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);

    EndPaint(hWnd, &ps);
}

static LRESULT CALLBACK LayersPopupWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        PopupLayout L = ComputePopupLayout(g_popupDpi);

        std::vector<size_t> displayIndices = GetPopupDisplayLayerIndices();
        int newHoverRow = -1;
        bool newHoverLock = false;
        bool newHoverReset = (CountHiddenLayers() > 0) && PtInRect(&L.rcShowAll, pt);

        if (!newHoverReset && pt.y >= L.headerH && !displayIndices.empty()) {
            int visRow = (pt.y - L.headerH) / L.rowH;
            int idx = g_popupScrollIdx + visRow;
            if (visRow >= 0 && visRow < L.visibleRows && idx < static_cast<int>(displayIndices.size())) {
                size_t layerIdx = displayIndices[idx];
                newHoverRow = idx;
                if (!g_app.layers[layerIdx].isTaskbar && !g_app.layers[layerIdx].isDesktopIcons) {
                    RECT rcLock = GetRowPrivacyLockRect(L, visRow, g_popupDpi);
                    newHoverLock = PtInRect(&rcLock, pt) != FALSE;
                }
            }
        }

        if (newHoverRow != g_popupHoverRow ||
            newHoverLock != g_popupHoverLock ||
            newHoverReset != g_popupHoverReset) {
            g_popupHoverRow = newHoverRow;
            g_popupHoverLock = newHoverLock;
            g_popupHoverReset = newHoverReset;
            RepaintLayersPopup();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_popupHoverRow != -1 || g_popupHoverLock || g_popupHoverReset) {
            g_popupHoverRow = -1;
            g_popupHoverLock = false;
            g_popupHoverReset = false;
            RepaintLayersPopup();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int totalCount = static_cast<int>(GetPopupDisplayLayerIndices().size());
        if (totalCount > kPopupMaxVisibleRows) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_popupScrollIdx += (delta > 0) ? -1 : 1;
            int maxScroll = totalCount - kPopupMaxVisibleRows;
            g_popupScrollIdx = (std::max)(0, (std::min)(g_popupScrollIdx, maxScroll));
            RepaintLayersPopup();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        PopupLayout L = ComputePopupLayout(g_popupDpi);

        if (CountHiddenLayers() > 0 && PtInRect(&L.rcShowAll, pt)) {
            ShowAllLayers();
            RepaintToolbar();
            RepaintLayersPopup();
            return 0;
        }

        std::vector<size_t> displayIndices = GetPopupDisplayLayerIndices();
        if (pt.y >= L.headerH && !displayIndices.empty()) {
            int visRow = (pt.y - L.headerH) / L.rowH;
            int idx = g_popupScrollIdx + visRow;
            if (visRow >= 0 && visRow < L.visibleRows && idx < static_cast<int>(displayIndices.size())) {
                size_t layerIdx = displayIndices[idx];
                RECT rcLock = GetRowPrivacyLockRect(L, visRow, g_popupDpi);
                if (!g_app.layers[layerIdx].isTaskbar &&
                    !g_app.layers[layerIdx].isDesktopIcons &&
                    PtInRect(&rcLock, pt)) {
                    const std::wstring exe = g_app.layers[layerIdx].processName;
                    if (!exe.empty()) {
                        bool nextLocked = !IsProcessHiddenInConfig(exe);
                        ApplyProcessPrivacyLock(exe, nextLocked);
                    }
                }
                else {
                    ToggleLayerGroupVisibility(layerIdx);
                }
                RepaintToolbar();
                RepaintLayersPopup();
            }
        }
        return 0;
    }

    case WM_PAINT:
        PaintLayersPopup(hWnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RegisterToolbarClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToolbarWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToolbarWndClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW wcPopup = {};
    wcPopup.cbSize = sizeof(wcPopup);
    wcPopup.style = CS_HREDRAW | CS_VREDRAW;
    wcPopup.lpfnWndProc = LayersPopupWndProc;
    wcPopup.hInstance = hInst;
    wcPopup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcPopup.lpszClassName = kLayersPopupWndClass;
    RegisterClassExW(&wcPopup);
}

void CreateAndShowToolbar() {
    DestroyToolbar();
    g_hoverBtn = BTN_NONE;
    g_pressedBtn = BTN_NONE;
    g_trackingLeave = false;
    g_draggingPill = false;

    HWND hOwnerOverlay = nullptr;
    POINT origin = { 0, 0 };
    HMONITOR hPrimary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hPrimary, &mi)) {
        for (const auto& mon : g_app.overlays) {
            if (EqualRect(&mon.rect, &mi.rcMonitor)) {
                hOwnerOverlay = mon.hWnd;
                break;
            }
        }
    }
    if (!hOwnerOverlay && !g_app.overlays.empty()) {
        hOwnerOverlay = g_app.overlays[0].hWnd;
    }

    g_hToolbar = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kToolbarWndClass,
        L"",
        WS_POPUP,
        0, 0, 332, 40,
        hOwnerOverlay, nullptr, g_app.hInst, nullptr
    );

    RECT dummy = {};
    UINT dpi = GetPrimaryMonitorDpi(dummy);
    if (!g_hDelayFont) {
        g_hDelayFont = CreateFontW(-ScalePx(13, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    PositionToolbarAtAnchor();
    ShowWindow(g_hToolbar, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hToolbar);
}

void DestroyToolbar() {
    CloseLayersPopup();
    if (g_hDelayFont) {
        DeleteObject(g_hDelayFont);
        g_hDelayFont = nullptr;
    }
    if (g_hToolbar) {
        DestroyWindow(g_hToolbar);
        g_hToolbar = nullptr;
    }
}

void SetToolbarVisible(bool visible) {
    if (!visible && IsLayersPopupOpen()) {
        CloseLayersPopup();
    }
    if (!g_hToolbar) return;
    ShowWindow(g_hToolbar, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) {
        SetWindowPos(g_hToolbar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(g_hToolbar, nullptr, FALSE);
        UpdateWindow(g_hToolbar);
    }
}

void RepaintToolbar() {
    if (g_hToolbar) {
        InvalidateRect(g_hToolbar, nullptr, FALSE);
        UpdateWindow(g_hToolbar);
    }
}

bool IsLayersPopupOpen() {
    return g_hLayersPopup != nullptr && IsWindowVisible(g_hLayersPopup);
}

void RepaintLayersPopup() {
    if (g_hLayersPopup && IsWindowVisible(g_hLayersPopup)) {
        InvalidateRect(g_hLayersPopup, nullptr, FALSE);
        UpdateWindow(g_hLayersPopup);
    }
}

void CloseLayersPopup() {
    if (g_hLayersPopup) {
        DestroyWindow(g_hLayersPopup);
        g_hLayersPopup = nullptr;
    }
    if (g_hLayerTitleFont) {
        DeleteObject(g_hLayerTitleFont);
        g_hLayerTitleFont = nullptr;
    }
    if (g_hLayerSubFont) {
        DeleteObject(g_hLayerSubFont);
        g_hLayerSubFont = nullptr;
    }
    g_popupScrollIdx = 0;
    g_popupHoverRow = -1;
    g_popupHoverLock = false;
    g_popupHoverReset = false;
    RepaintToolbar();
}

void ToggleLayersPopup() {
    if (IsLayersPopupOpen()) {
        CloseLayersPopup();
        return;
    }

    if (!g_app.isCapturing || !g_hToolbar) {
        return;
    }

    if (!g_config.layerMode) {
        ToggleCaptureLayerMode();
    }

    RECT dummy = {};
    g_popupDpi = GetPrimaryMonitorDpi(dummy);
    g_hLayerTitleFont = BlitTheme::CreateUiFont(g_popupDpi, 9, false);
    g_hLayerSubFont = BlitTheme::CreateUiFont(g_popupDpi, 8, true);

    PopupLayout L = ComputePopupLayout(g_popupDpi);
    g_hLayersPopup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kLayersPopupWndClass,
        L"",
        WS_POPUP,
        0, 0, L.width, L.height,
        g_hToolbar,
        nullptr,
        g_app.hInst,
        nullptr);

    if (g_hLayersPopup) {
        PositionLayersPopupNearToolbar();
        ShowWindow(g_hLayersPopup, SW_SHOWNA);
        UpdateWindow(g_hLayersPopup);
        RepaintToolbar();
    }
}