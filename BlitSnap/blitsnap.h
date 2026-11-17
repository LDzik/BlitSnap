#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdint.h>
#include <vector>

constexpr UINT WM_APP_TRIGGER_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_CANCEL_CAPTURE = WM_APP + 2;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 3;

constexpr wchar_t kMutexName[] = L"BlitSnap_SingleInstance_Mutex";
constexpr wchar_t kMsgWndClass[] = L"BlitSnap_MessageOnly_Class";
constexpr wchar_t kOverlayWndClass[] = L"BlitSnap_Overlay_Class";

constexpr int      kCancelDeadzonePx = 5;
constexpr COLORREF kAccentColor = RGB(0, 120, 215); // #0078D7

enum class CaptureMode {
    Region,
    Window
};

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

struct AppState {
    HINSTANCE                   hInst = nullptr;
    HWND                        hMsgWnd = nullptr;
    HHOOK                       hKeyHook = nullptr;

    CaptureMode                 mode = CaptureMode::Region;
    DibSurface                  pristine = {};
    DibSurface                  dimmed = {};
    std::vector<MonitorOverlay> overlays;
    std::vector<RECT>           windowRects;

    bool                        isCapturing = false;
    bool                        isDragging = false;
    bool                        dragCancelArm = false;
    POINT                       ptOrigin = {};
    POINT                       ptCurrent = {};

    bool                        hasHoveredWin = false;
    RECT                        rcHoveredWin = {};
};

extern AppState g_app;