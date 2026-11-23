#include "layers.h"
#include "capture.h"
#include "config.h"
#include "overlay.h"
#include "toolbar.h"
#include <dwmapi.h>
#include <algorithm>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

    struct EnumLayerContext {
        HWND hProgman = nullptr;
        HWND hWallpaperWorker = nullptr;
        RECT rcVirtual{};
    };

    HWND FindWallpaperWorkerW(HWND hProgman) {
        HWND hWorker = nullptr;
        while ((hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr) {
            if (FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr) {
                HWND hNext = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr);
                if (hNext != nullptr && IsWindowVisible(hNext)) {
                    return hNext;
                }
            }
        }
        return hProgman;
    }

    std::wstring QueryWindowProcessName(DWORD pid) {
        if (pid == 0) return L"";

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return L"";

        wchar_t pathBuf[MAX_PATH * 2]{};
        DWORD len = _countof(pathBuf);
        std::wstring exeName;
        if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &len) && len > 0) {
            const wchar_t* lastSlash = wcsrchr(pathBuf, L'\\');
            const wchar_t* lastFwd = wcsrchr(pathBuf, L'/');
            const wchar_t* base = lastSlash ? lastSlash + 1 : pathBuf;
            if (lastFwd && lastFwd + 1 > base) {
                base = lastFwd + 1;
            }
            exeName = base;
        }

        CloseHandle(hProc);
        return exeName;
    }

    std::wstring QueryWindowDisplayTitle(HWND hWnd, const std::wstring& processName) {
        int len = GetWindowTextLengthW(hWnd);
        if (len > 0) {
            std::wstring title(static_cast<size_t>(len) + 1, L'\0');
            int copied = GetWindowTextW(hWnd, title.data(), len + 1);
            if (copied > 0) {
                title.resize(static_cast<size_t>(copied));
                return title;
            }
        }

        wchar_t cls[64]{};
        if (GetClassNameW(hWnd, cls, _countof(cls)) > 0) {
            if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
                return L"Taskbar";
            }
        }

        if (!processName.empty()) {
            size_t dot = processName.rfind(L'.');
            return (dot != std::wstring::npos) ? processName.substr(0, dot) : processName;
        }

        return L"Window";
    }

    bool CaptureDesktopBaseLayer(HWND hProgman, HWND hWallpaperWnd) {
        FreeDibSurface(g_app.desktopLayer);
        if (!CreateDibSurface(g_app.desktopLayer, g_app.vw, g_app.vh)) {
            return false;
        }

        POINT oldOrg{};
        SetViewportOrgEx(g_app.desktopLayer.hDC, -g_app.vx, -g_app.vy, &oldOrg);
        PaintDesktop(g_app.desktopLayer.hDC);
        SetViewportOrgEx(g_app.desktopLayer.hDC, oldOrg.x, oldOrg.y, nullptr);

        HWND hTarget = (hWallpaperWnd && IsWindow(hWallpaperWnd)) ? hWallpaperWnd : hProgman;
        if (hTarget && IsWindow(hTarget)) {
            RECT rcDesk{};
            if (GetWindowRect(hTarget, &rcDesk)) {
                int dw = rcDesk.right - rcDesk.left;
                int dh = rcDesk.bottom - rcDesk.top;
                if (dw > 0 && dh > 0) {
                    DibSurface tmpDesk{};
                    if (CreateDibSurface(tmpDesk, dw, dh)) {
                        BOOL ok = PrintWindow(hTarget, tmpDesk.hDC, PW_RENDERFULLCONTENT);
                        if (!ok) {
                            ok = PrintWindow(hTarget, tmpDesk.hDC, 0);
                        }
                        if (ok) {
                            BitBlt(
                                g_app.desktopLayer.hDC,
                                rcDesk.left - g_app.vx,
                                rcDesk.top - g_app.vy,
                                dw,
                                dh,
                                tmpDesk.hDC,
                                0,
                                0,
                                SRCCOPY);
                        }
                        FreeDibSurface(tmpDesk);
                    }
                }
            }
        }

        return true;
    }

    BOOL CALLBACK EnumWindowsLayersProc(HWND hWnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<EnumLayerContext*>(lParam);

        if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) {
            return TRUE;
        }
        if (hWnd == GetShellWindow() || hWnd == ctx->hProgman || hWnd == ctx->hWallpaperWorker) {
            return TRUE;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == GetCurrentProcessId()) {
            return TRUE;
        }

        wchar_t cls[64]{};
        GetClassNameW(hWnd, cls, _countof(cls));

        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
            return TRUE;
        }

        bool isTaskbar = (wcscmp(cls, L"Shell_TrayWnd") == 0 ||
                          wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0);

        LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (!isTaskbar && (exStyle & WS_EX_TOOLWINDOW)) {
            return TRUE;
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        RECT rcWindow{};
        if (!GetWindowRect(hWnd, &rcWindow)) {
            return TRUE;
        }
        int winW = rcWindow.right - rcWindow.left;
        int winH = rcWindow.bottom - rcWindow.top;
        if (winW <= 0 || winH <= 0) {
            return TRUE;
        }

        RECT rcExtended{};
        if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExtended, sizeof(rcExtended)))) {
            rcExtended = rcWindow;
        }

        rcExtended.left = (std::max)(rcExtended.left, rcWindow.left);
        rcExtended.top = (std::max)(rcExtended.top, rcWindow.top);
        rcExtended.right = (std::min)(rcExtended.right, rcWindow.right);
        rcExtended.bottom = (std::min)(rcExtended.bottom, rcWindow.bottom);
        if (rcExtended.right <= rcExtended.left || rcExtended.bottom <= rcExtended.top) {
            return TRUE;
        }

        RECT rcClip{};
        if (!IntersectRect(&rcClip, &rcExtended, &ctx->rcVirtual)) {
            return TRUE;
        }

        DWORD_PTR unused = 0;
        if (!SendMessageTimeoutW(hWnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &unused)) {
            return TRUE;
        }

        WindowLayer layer{};
        layer.hWnd = hWnd;
        layer.hOwner = GetWindow(hWnd, GW_OWNER);
        layer.rcWindow = rcWindow;
        layer.rcExtended = rcExtended;
        layer.processName = QueryWindowProcessName(pid);
        layer.title = isTaskbar ? L"Taskbar" : QueryWindowDisplayTitle(hWnd, layer.processName);

        if (!CreateDibSurface(layer.surface, winW, winH)) {
            return TRUE;
        }

        BOOL captured = PrintWindow(hWnd, layer.surface.hDC, PW_RENDERFULLCONTENT);
        if (!captured) {
            captured = PrintWindow(hWnd, layer.surface.hDC, 0);
        }
        if (!captured) {
            FreeDibSurface(layer.surface);
            return TRUE;
        }

        layer.isTaskbar = isTaskbar;
        layer.autoHidden = isTaskbar
            ? !g_config.showTaskbar
            : IsProcessHiddenInConfig(layer.processName);
        layer.visible = !layer.autoHidden;

        g_app.layers.push_back(std::move(layer));
        return TRUE;
    }

    int FindLayerIndexByHwnd(HWND hWnd) {
        if (!hWnd) return -1;
        for (size_t i = 0; i < g_app.layers.size(); ++i) {
            if (g_app.layers[i].hWnd == hWnd) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void ResolveOwnerHierarchy() {
        for (auto& layer : g_app.layers) {
            HWND cur = layer.hOwner;
            int depth = 0;
            while (cur && depth < 16) {
                int ownerIdx = FindLayerIndexByHwnd(cur);
                if (ownerIdx >= 0) {
                    layer.rootOwnerHwnd = g_app.layers[ownerIdx].hWnd;
                    if (g_app.layers[ownerIdx].autoHidden) {
                        layer.visible = false;
                        layer.autoHidden = true;
                    }
                }
                cur = GetWindow(cur, GW_OWNER);
                ++depth;
            }
        }
    }

}

void FreeLayerStack() {
    for (auto& layer : g_app.layers) {
        FreeDibSurface(layer.surface);
    }
    g_app.layers.clear();
    g_app.peelHistory.clear();
    g_app.hoveredLayerIndex = -1;
    FreeDibSurface(g_app.desktopLayer);
}

bool CaptureLayerStack() {
    FreeLayerStack();

    HWND hProgman = FindWindowW(L"Progman", nullptr);
    HWND hWallpaperWorker = FindWallpaperWorkerW(hProgman);

    if (!CaptureDesktopBaseLayer(hProgman, hWallpaperWorker)) {
        return false;
    }

    EnumLayerContext ctx{};
    ctx.hProgman = hProgman;
    ctx.hWallpaperWorker = hWallpaperWorker;
    ctx.rcVirtual = { g_app.vx, g_app.vy, g_app.vx + g_app.vw, g_app.vy + g_app.vh };

    EnumWindows(EnumWindowsLayersProc, reinterpret_cast<LPARAM>(&ctx));
    ResolveOwnerHierarchy();
    return true;
}

void RecompositeLayers() {
    if (!g_app.pristine.hDC || !g_app.dimmed.pixels || !g_app.desktopLayer.hDC) {
        return;
    }

    BitBlt(g_app.pristine.hDC, 0, 0, g_app.vw, g_app.vh, g_app.desktopLayer.hDC, 0, 0, SRCCOPY);

    for (size_t i = g_app.layers.size(); i-- > 0;) {
        const WindowLayer& layer = g_app.layers[i];
        if (!layer.visible || !layer.surface.hDC) {
            continue;
        }

        int srcX = layer.rcExtended.left - layer.rcWindow.left;
        int srcY = layer.rcExtended.top - layer.rcWindow.top;
        int copyW = layer.rcExtended.right - layer.rcExtended.left;
        int copyH = layer.rcExtended.bottom - layer.rcExtended.top;

        if (srcX < 0 || srcY < 0 || copyW <= 0 || copyH <= 0) {
            continue;
        }
        if (srcX + copyW > layer.surface.width) {
            copyW = layer.surface.width - srcX;
        }
        if (srcY + copyH > layer.surface.height) {
            copyH = layer.surface.height - srcY;
        }
        if (copyW <= 0 || copyH <= 0) {
            continue;
        }

        int dstX = layer.rcExtended.left - g_app.vx;
        int dstY = layer.rcExtended.top - g_app.vy;

        BitBlt(
            g_app.pristine.hDC,
            dstX,
            dstY,
            copyW,
            copyH,
            layer.surface.hDC,
            srcX,
            srcY,
            SRCCOPY);
    }

    DimPixelsSSE2(g_app.pristine.pixels, g_app.dimmed.pixels, g_app.vw * g_app.vh);

    g_app.windowRects.clear();
    g_app.windowRects.reserve(g_app.layers.size());
    for (const auto& layer : g_app.layers) {
        if (layer.visible) {
            g_app.windowRects.push_back(layer.rcExtended);
        }
    }

    if (g_app.mode == CaptureMode::Window) {
        g_app.hoveredLayerIndex = -1;
        SetRectEmpty(&g_app.rcHoveredWin);
        for (size_t i = 0; i < g_app.layers.size(); ++i) {
            if (g_app.layers[i].visible && PtInRect(&g_app.layers[i].rcExtended, g_app.ptCursor)) {
                g_app.hoveredLayerIndex = static_cast<int>(i);
                g_app.rcHoveredWin = g_app.layers[i].rcExtended;
                break;
            }
        }
    }

    for (const auto& mon : g_app.overlays) {
        if (mon.hWnd) {
            InvalidateRect(mon.hWnd, nullptr, FALSE);
            UpdateWindow(mon.hWnd);
        }
    }
}

bool PeelLayerGroupAtPoint(POINT pt) {
    if (!g_app.isCapturing || !g_config.layerMode || g_app.isDragging) {
        return false;
    }

    int hitIdx = -1;
    for (size_t i = 0; i < g_app.layers.size(); ++i) {
        if (g_app.layers[i].visible && PtInRect(&g_app.layers[i].rcExtended, pt)) {
            hitIdx = static_cast<int>(i);
            break;
        }
    }
    if (hitIdx < 0) {
        return false;
    }

    bool hitTaskbar = g_app.layers[hitIdx].isTaskbar;
    HWND groupRoot = g_app.layers[hitIdx].rootOwnerHwnd
        ? g_app.layers[hitIdx].rootOwnerHwnd
        : g_app.layers[hitIdx].hWnd;

    std::vector<size_t> hiddenGroup;
    for (size_t i = 0; i < g_app.layers.size(); ++i) {
        if (!g_app.layers[i].visible) continue;
        bool sameGroup = hitTaskbar
            ? g_app.layers[i].isTaskbar
            : (!g_app.layers[i].isTaskbar &&
               (g_app.layers[i].rootOwnerHwnd ? g_app.layers[i].rootOwnerHwnd : g_app.layers[i].hWnd) == groupRoot);
        if (sameGroup) {
            g_app.layers[i].visible = false;
            hiddenGroup.push_back(i);
        }
    }

    if (hiddenGroup.empty()) {
        return false;
    }

    g_app.peelHistory.push_back(std::move(hiddenGroup));
    RecompositeLayers();
    return true;
}

bool UnpeelLastLayerGroup() {
    if (!g_app.isCapturing || !g_config.layerMode || g_app.isDragging || g_app.peelHistory.empty()) {
        return false;
    }

    std::vector<size_t> group = std::move(g_app.peelHistory.back());
    g_app.peelHistory.pop_back();

    bool restoredAny = false;
    for (size_t idx : group) {
        if (idx < g_app.layers.size() && !g_app.layers[idx].visible) {
            g_app.layers[idx].visible = true;
            restoredAny = true;
        }
    }

    if (restoredAny) {
        RecompositeLayers();
    }
    return restoredAny;
}

bool ToggleLayerGroupVisibility(size_t index) {
    if (!g_app.isCapturing || !g_config.layerMode || index >= g_app.layers.size()) {
        return false;
    }

    bool targetVisible = !g_app.layers[index].visible;
    bool clickedTaskbar = g_app.layers[index].isTaskbar;

    if (clickedTaskbar) {
        g_config.showTaskbar = targetVisible;
        SaveConfig();
    }

    HWND groupRoot = g_app.layers[index].rootOwnerHwnd
        ? g_app.layers[index].rootOwnerHwnd
        : g_app.layers[index].hWnd;

    std::vector<size_t> changedIndices;
    for (size_t i = 0; i < g_app.layers.size(); ++i) {
        bool sameGroup = clickedTaskbar
            ? g_app.layers[i].isTaskbar
            : (!g_app.layers[i].isTaskbar &&
               (g_app.layers[i].rootOwnerHwnd ? g_app.layers[i].rootOwnerHwnd : g_app.layers[i].hWnd) == groupRoot);

        if (sameGroup && g_app.layers[i].visible != targetVisible) {
            g_app.layers[i].visible = targetVisible;
            changedIndices.push_back(i);
        }
    }

    if (changedIndices.empty()) {
        return false;
    }

    if (!targetVisible) {
        g_app.peelHistory.push_back(std::move(changedIndices));
    }

    RecompositeLayers();
    return true;
}

void ShowAllLayers() {
    if (!g_app.isCapturing || !g_config.layerMode) {
        return;
    }

    bool anyChanged = false;
    for (auto& layer : g_app.layers) {
        if (!layer.visible) {
            layer.visible = true;
            anyChanged = true;
        }
    }

    g_app.peelHistory.clear();
    if (anyChanged) {
        RecompositeLayers();
    }
}

void ApplyProcessPrivacyLock(const std::wstring& exeName, bool locked) {
    if (exeName.empty()) {
        return;
    }

    SetProcessHiddenInConfig(exeName, locked);

    if (!g_app.isCapturing || !g_config.layerMode) {
        return;
    }

    bool anyChanged = false;
    for (auto& layer : g_app.layers) {
        if (layer.isTaskbar) continue;
        if (!layer.processName.empty() && _wcsicmp(layer.processName.c_str(), exeName.c_str()) == 0) {
            layer.autoHidden = locked;
            bool desiredVisible = !locked;
            if (layer.visible != desiredVisible) {
                layer.visible = desiredVisible;
                anyChanged = true;
            }
        }
    }

    if (anyChanged) {
        RecompositeLayers();
    }
}

void ToggleCaptureLayerMode() {
    if (g_app.isDragging || g_app.isDraggingPill) {
        return;
    }

    g_config.layerMode = !g_config.layerMode;
    SaveConfig();
    RepaintToolbar();

    if (!g_app.isCapturing) {
        return;
    }

    if (g_config.layerMode) {
        if (!g_app.desktopLayer.hDC) {
            CaptureLayerStack();
        }
        RecompositeLayers();
    }
    else {
        if (IsLayersPopupOpen()) {
            CloseLayersPopup();
        }

        if (g_app.classicSnapshot.hDC && g_app.pristine.hDC && g_app.dimmed.pixels) {
            BitBlt(
                g_app.pristine.hDC,
                0, 0, g_app.vw, g_app.vh,
                g_app.classicSnapshot.hDC,
                0, 0,
                SRCCOPY);
            DimPixelsSSE2(g_app.pristine.pixels, g_app.dimmed.pixels, g_app.vw * g_app.vh);
        }

        g_app.windowRects = g_app.classicWindowRects;
        g_app.hoveredLayerIndex = -1;
        SetRectEmpty(&g_app.rcHoveredWin);

        if (g_app.mode == CaptureMode::Window) {
            UpdateHoveredWindowAt(g_app.ptCursor);
        }

        for (const auto& mon : g_app.overlays) {
            if (mon.hWnd) {
                InvalidateRect(mon.hWnd, nullptr, FALSE);
                UpdateWindow(mon.hWnd);
            }
        }
    }
}