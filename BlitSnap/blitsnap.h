#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdint.h>
#include <vector>
#include <string>

constexpr UINT WM_APP_TRIGGER_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_CANCEL_CAPTURE = WM_APP + 2;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 3;
constexpr UINT WM_APP_LAUNCH_EDITOR = WM_APP + 4;
constexpr UINT WM_APP_TIMER_TICK = WM_APP + 5;
constexpr UINT WM_APP_TOGGLE_LOUPE = WM_APP + 6;
constexpr UINT WM_APP_TOGGLE_MODE = WM_APP + 7;
constexpr UINT WM_APP_TOGGLE_LAYER_MODE = WM_APP + 8;
constexpr UINT WM_APP_PEEL_HOVER_LAYER = WM_APP + 9;
constexpr UINT WM_APP_UNPEEL_LAYER = WM_APP + 10;

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
    int       width = 0;
    int       height = 0;
};

struct WindowLayer {
    HWND         hWnd = nullptr;
    HWND         hOwner = nullptr;
    HWND         rootOwnerHwnd = nullptr;
    std::wstring title;
    std::wstring processName;
    RECT         rcWindow{};
    RECT         rcExtended{};
    DibSurface   surface{};
    bool         visible = true;
    bool         autoHidden = false;
    bool         isTaskbar = false;
};

struct MonitorOverlay {
    HWND    hWnd = nullptr;
    RECT    rect = {};
    UINT    dpi = 96;
    HBITMAP hBackBmp = nullptr;
    HDC     hBackDC = nullptr;
    HGDIOBJ hOldBackBmp = nullptr;
    HFONT   hLoupeFont = nullptr;
    HFONT   hBadgeFont = nullptr;
};

struct AppState {
    HINSTANCE                   hInst = nullptr;
    HWND                        hMsgWnd = nullptr;
    HHOOK                       hKeyHook = nullptr;


    bool                        isCapturing = false;
    CaptureMode                 mode = CaptureMode::Region;
    bool                        showLoupe = true;
    std::wstring                activeWindowTitle;

    int                         vx = 0;
    int                         vy = 0;
    int                         vw = 0;
    int                         vh = 0;
    DibSurface                  pristine = {};
    DibSurface                  dimmed = {};

    DibSurface                       desktopLayer{};
    std::vector<WindowLayer>         layers;
    std::vector<std::vector<size_t>> peelHistory;
    int                              hoveredLayerIndex = -1;

    std::vector<MonitorOverlay> overlays;
    std::vector<RECT>           windowRects;


    bool                        isDragging = false;
    bool                        dragCancelArm = false;
    bool                        hasLeftDeadzone = false;
    POINT                       ptOrigin = {};
    POINT                       ptCurrent = {};
    POINT                       ptCursor = {};
    HWND                        hLastLoupeWnd = nullptr;


    RECT                        rcHoveredWin = {};

    bool                        isDraggingPill = false;
    int                         pillSnapPreview = -1;
};

extern AppState g_app;