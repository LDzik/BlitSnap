#include "toolbar.h"
#include "overlay.h"
#include "timer.h"
#include <windowsx.h>
#include <cmath>

constexpr wchar_t kToolbarWndClass[] = L"BlitSnap_Toolbar_Class";

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
    BTN_DELAY_3 = 4,
    BTN_DELAY_5 = 5,
    BTN_CLOSE = 6,
    BTN_COUNT = 7
};

static HWND       g_hToolbar = nullptr;
static PillAnchor g_anchor = PillAnchor::TopCenter;
static int        g_hoverBtn = BTN_NONE;
static int        g_pressedBtn = BTN_NONE;
static bool       g_trackingLeave = false;
static bool       g_draggingPill = false;
static POINT      g_pillDragStart = {};

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

    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

static void GetButtonRects(int w, int h, UINT dpi, RECT outRects[BTN_COUNT]) {
    int pad = ScalePx(4, dpi);
    int gripW = ScalePx(16, dpi);
    int btnW = ScalePx(36, dpi);
    int gap = ScalePx(2, dpi);
    int sepGap = ScalePx(6, dpi);
    int btnH = h - pad * 2;

    int x = pad;
    outRects[BTN_GRIP] = { x, pad, x + gripW, pad + btnH }; x += gripW + gap;
    outRects[BTN_REGION] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_WINDOW] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_FULLSCREEN] = { x, pad, x + btnW,  pad + btnH }; x += btnW + sepGap;
    outRects[BTN_DELAY_3] = { x, pad, x + btnW,  pad + btnH }; x += btnW + gap;
    outRects[BTN_DELAY_5] = { x, pad, x + btnW,  pad + btnH }; x += btnW + sepGap;
    outRects[BTN_CLOSE] = { x, pad, x + btnW,  pad + btnH };
}

bool GetPillAnchorRect(int anchorIndex, RECT& outRect) {
    RECT rcMon = {};
    UINT dpi = GetPrimaryMonitorDpi(rcMon);

    int pillW = ScalePx(256, dpi);
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
    case BTN_DELAY_3:
    case BTN_DELAY_5: {
        const wchar_t* label = (btn == BTN_DELAY_3) ? L"3s" : L"5s";
        int fontH = ScalePx(13, dpi);
        HFONT hFont = CreateFontW(-fontH, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, hFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);
        RECT rcTxt = rc;
        DrawTextW(hdc, label, -1, &rcTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
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
            (i == BTN_WINDOW && g_app.mode == CaptureMode::Window);

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

void RegisterToolbarClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToolbarWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToolbarWndClass;
    RegisterClassExW(&wc);
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
        0, 0, 180, 40,
        hOwnerOverlay, nullptr, g_app.hInst, nullptr
    );

    PositionToolbarAtAnchor();
    ShowWindow(g_hToolbar, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hToolbar);
}

void DestroyToolbar() {
    if (g_hToolbar) {
        DestroyWindow(g_hToolbar);
        g_hToolbar = nullptr;
    }
}

void SetToolbarVisible(bool visible) {
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