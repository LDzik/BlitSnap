#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace BlitTheme {
    constexpr COLORREF kBgWindow = RGB(30, 30, 30);   // #1E1E1E main canvas
    constexpr COLORREF kBgSurface = RGB(37, 37, 38);   // #252526 toolbar / panel
    constexpr COLORREF kBgInput = RGB(24, 24, 24);   // #181818 edit box
    constexpr COLORREF kBgHover = RGB(45, 45, 48);   // #2D2D30 control hover
    constexpr COLORREF kBgPressed = RGB(62, 62, 66);   // #3E3E42 control active

    constexpr COLORREF kBorder = RGB(56, 56, 56);   // #383838 1px hairline
    constexpr COLORREF kBorderFocus = RGB(0, 120, 215);  // #0078D7 focus / accent
    constexpr COLORREF kAccent = RGB(0, 120, 215);  // #0078D7 primary action
    constexpr COLORREF kAccentHover = RGB(28, 136, 224); // #1C88E0 primary hover
    constexpr COLORREF kDangerHover = RGB(196, 43, 28);  // #C42B1C shutdown / close

    constexpr COLORREF kTextPrimary = RGB(230, 230, 230);// #E6E6E6
    constexpr COLORREF kTextMuted = RGB(150, 150, 150);// #969696

    inline int Scale(int px, UINT dpi) {
        return MulDiv(px, static_cast<int>(dpi), 96);
    }

    inline void EnableDarkTitleBar(HWND hWnd) {
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hWnd, 20, &dark, sizeof(dark)))) {
            DwmSetWindowAttribute(hWnd, 19, &dark, sizeof(dark));
        }
    }

    inline HFONT CreateUiFont(UINT dpi, int ptSize = 9, bool bold = false) {
        int height = -MulDiv(ptSize, static_cast<int>(dpi), 72);
        return CreateFontW(
            height, 0, 0, 0,
            bold ? FW_SEMIBOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            L"Segoe UI"
        );
    }

    inline void DrawFlatButton(const DRAWITEMSTRUCT* dis, UINT dpi, bool isPrimary = false, bool isDanger = false) {
        if (!dis) return;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;

        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        bool hovered = (dis->itemData & 1) != 0;

        COLORREF bg = isPrimary ? kAccent : kBgSurface;
        COLORREF border = isPrimary ? kAccent : kBorder;
        COLORREF fg = disabled ? kTextMuted : kTextPrimary;

        if (!disabled) {
            if (pressed) {
                bg = isDanger ? RGB(160, 32, 20) : (isPrimary ? RGB(0, 96, 175) : kBgPressed);
                border = isDanger ? kDangerHover : kBorderFocus;
            }
            else if (hovered) {
                bg = isDanger ? kDangerHover : (isPrimary ? kAccentHover : kBgHover);
                border = isDanger ? kDangerHover : (isPrimary ? kAccentHover : RGB(80, 80, 85));
            }
        }

        HBRUSH hBg = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        HBRUSH hBr = CreateSolidBrush(border);
        FrameRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        wchar_t text[128] = {};
        GetWindowTextW(dis->hwndItem, text, 128);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    inline void DrawFlatCheckbox(const DRAWITEMSTRUCT* dis, UINT dpi, bool checked) {
        if (!dis) return;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;

        HBRUSH hClear = CreateSolidBrush(kBgWindow);
        FillRect(hdc, &rc, hClear);
        DeleteObject(hClear);

        int boxSize = Scale(14, dpi);
        int boxY = rc.top + ((rc.bottom - rc.top) - boxSize) / 2;
        RECT rcBox = { rc.left, boxY, rc.left + boxSize, boxY + boxSize };

        bool hovered = (dis->itemData & 1) != 0;
        COLORREF boxBg = checked ? kAccent : kBgInput;
        COLORREF boxBrd = checked ? kAccent : (hovered ? RGB(100, 100, 105) : kBorder);

        HBRUSH hBoxBg = CreateSolidBrush(boxBg);
        FillRect(hdc, &rcBox, hBoxBg);
        DeleteObject(hBoxBg);

        HBRUSH hBoxBrd = CreateSolidBrush(boxBrd);
        FrameRect(hdc, &rcBox, hBoxBrd);
        DeleteObject(hBoxBrd);

        if (checked) {
            int stroke = std::max(1, Scale(2, dpi));
            HPEN hPen = CreatePen(PS_SOLID, stroke, RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(hdc, hPen);
            int x1 = rcBox.left + Scale(3, dpi);
            int y1 = rcBox.top + Scale(7, dpi);
            int x2 = rcBox.left + Scale(6, dpi);
            int y2 = rcBox.top + Scale(10, dpi);
            int x3 = rcBox.left + Scale(11, dpi);
            int y3 = rcBox.top + Scale(4, dpi);
            MoveToEx(hdc, x1, y1, nullptr);
            LineTo(hdc, x2, y2);
            LineTo(hdc, x3, y3);
            SelectObject(hdc, oldPen);
            DeleteObject(hPen);
        }

        wchar_t text[256] = {};
        GetWindowTextW(dis->hwndItem, text, 256);

        RECT rcText = rc;
        rcText.left += boxSize + Scale(8, dpi);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextPrimary);
        DrawTextW(hdc, text, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}