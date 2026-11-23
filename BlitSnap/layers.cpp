#include "layers.h"
#include "capture.h"
#include "config.h"
#include "overlay.h"
#include "toolbar.h"
#include <dwmapi.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "msimg32.lib")

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

    bool g_hadOpenShellFlyout = false;

    struct EnumLayerContext {
        HWND hProgman = nullptr;
        HWND hWallpaperWorker = nullptr;
        RECT rcVirtual{};
        const std::vector<ShellFlyoutInfo>* flyouts = nullptr;
    };

#ifdef _DEBUG
    void LogShellFlyoutCandidate(
        HWND hWnd,
        const wchar_t* cls,
        const std::wstring& proc,
        const RECT& rcWin,
        const RECT& rcExt,
        const wchar_t* status)
    {
        wchar_t buf[512]{};
        swprintf_s(
            buf,
            L"[BlitSnap][Flyout] hwnd=0x%p cls=%ls exe=%ls rcWin=(%ld,%ld,%ld,%ld) rcExt=(%ld,%ld,%ld,%ld) -> %ls\n",
            static_cast<void*>(hWnd),
            (cls && cls[0]) ? cls : L"<none>",
            !proc.empty() ? proc.c_str() : L"<unknown>",
            rcWin.left, rcWin.top, rcWin.right, rcWin.bottom,
            rcExt.left, rcExt.top, rcExt.right, rcExt.bottom,
            status ? status : L"unknown");
        OutputDebugStringW(buf);
    }
#endif

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

    HWND FindShellDefView(HWND hProgman) {
        if (hProgman) {
            HWND hDefView = FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
            if (hDefView) {
                return hDefView;
            }
        }
        HWND hWorker = nullptr;
        while ((hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr) {
            HWND hDefView = FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr);
            if (hDefView) {
                return hDefView;
            }
        }
        return nullptr;
    }

    bool IsShellHostProcessName(const std::wstring& exeName) {
        return _wcsicmp(exeName.c_str(), L"ShellExperienceHost.exe") == 0 ||
               _wcsicmp(exeName.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
               _wcsicmp(exeName.c_str(), L"SearchUI.exe") == 0 ||
               _wcsicmp(exeName.c_str(), L"SearchApp.exe") == 0 ||
               _wcsicmp(exeName.c_str(), L"TextInputHost.exe") == 0;
    }

    bool IsSurfaceAllBlack(const DibSurface& surf) {
        if (!surf.pixels || surf.width <= 0 || surf.height <= 0) {
            return true;
        }
        GdiFlush();
        const size_t pixelCount = static_cast<size_t>(surf.width) * static_cast<size_t>(surf.height);
        for (size_t i = 0; i < pixelCount; ++i) {
            if ((surf.pixels[i] & 0x00FFFFFFu) != 0) {
                return false;
            }
        }
        return true;
    }

    bool CopyRectFromClassicSnapshot(DibSurface& dstSurface, const RECT& rcWindow, const RECT& rcExtended, const RECT& rcVirtual) {
        if (!g_app.classicSnapshot.hDC || !dstSurface.hDC || !dstSurface.pixels ||
            dstSurface.width <= 0 || dstSurface.height <= 0) {
            return false;
        }

        RECT rcClip{};
        if (!IntersectRect(&rcClip, &rcExtended, &rcVirtual)) {
            return false;
        }

        const size_t pixelCount = static_cast<size_t>(dstSurface.width) * static_cast<size_t>(dstSurface.height);
        std::memset(dstSurface.pixels, 0, pixelCount * sizeof(uint32_t));

        int dstX = rcClip.left - rcWindow.left;
        int dstY = rcClip.top - rcWindow.top;
        int srcX = rcClip.left - g_app.vx;
        int srcY = rcClip.top - g_app.vy;
        int copyW = rcClip.right - rcClip.left;
        int copyH = rcClip.bottom - rcClip.top;

        if (dstX < 0 || dstY < 0 || copyW <= 0 || copyH <= 0 ||
            dstX + copyW > dstSurface.width || dstY + copyH > dstSurface.height) {
            return false;
        }

        if (!BitBlt(
                dstSurface.hDC,
                dstX,
                dstY,
                copyW,
                copyH,
                g_app.classicSnapshot.hDC,
                srcX,
                srcY,
                SRCCOPY)) {
            return false;
        }

        GdiFlush();
        for (int y = 0; y < copyH; ++y) {
            uint32_t* row = dstSurface.pixels + static_cast<size_t>(dstY + y) * dstSurface.width + dstX;
            for (int x = 0; x < copyW; ++x) {
                row[x] |= 0xFF000000u;
            }
        }
        return true;
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

    bool CaptureDesktopBaseLayer(HWND hProgman, HWND hWallpaperWorker) {
        FreeDibSurface(g_app.desktopLayer);
        if (!CreateDibSurface(g_app.desktopLayer, g_app.vw, g_app.vh)) {
            return false;
        }

        POINT oldOrg{};
        SetViewportOrgEx(g_app.desktopLayer.hDC, -g_app.vx, -g_app.vy, &oldOrg);
        PaintDesktop(g_app.desktopLayer.hDC);
        SetViewportOrgEx(g_app.desktopLayer.hDC, oldOrg.x, oldOrg.y, nullptr);

        HWND hTarget = (hWallpaperWorker && IsWindow(hWallpaperWorker)) ? hWallpaperWorker : hProgman;
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

    void CaptureDesktopIconsLayer(HWND hProgman) {
        HWND hDefView = FindShellDefView(hProgman);
        if (!hDefView || !IsWindow(hDefView) || !IsWindowVisible(hDefView)) {
            return;
        }

        HWND hListView = FindWindowExW(hDefView, nullptr, L"SysListView32", L"FolderView");
        if (!hListView) {
            hListView = FindWindowExW(hDefView, nullptr, L"SysListView32", nullptr);
        }
        if (!hListView || !IsWindow(hListView) || !IsWindowVisible(hListView)) {
            return;
        }

        RECT rcListView{};
        if (!GetWindowRect(hListView, &rcListView)) {
            return;
        }
        int w = rcListView.right - rcListView.left;
        int h = rcListView.bottom - rcListView.top;
        if (w <= 0 || h <= 0) {
            return;
        }

        DibSurface tmpIcons{};
        if (!CreateDibSurface(tmpIcons, w, h)) {
            return;
        }

        size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::memset(tmpIcons.pixels, 0, pixelCount * sizeof(uint32_t));

        BOOL ok = PrintWindow(hListView, tmpIcons.hDC, PW_RENDERFULLCONTENT);
        if (!ok) {
            ok = PrintWindow(hListView, tmpIcons.hDC, 0);
        }
        GdiFlush();

        if (!ok) {
            FreeDibSurface(tmpIcons);
            return;
        }

        WindowLayer layer{};
        if (!CreateDibSurface(layer.surface, w, h)) {
            FreeDibSurface(tmpIcons);
            return;
        }

        bool hasAlpha = false;
        for (size_t i = 0; i < pixelCount; ++i) {
            if ((tmpIcons.pixels[i] & 0xFF000000u) != 0) {
                hasAlpha = true;
                break;
            }
        }

        if (hasAlpha) {
            for (size_t i = 0; i < pixelCount; ++i) {
                uint32_t px = tmpIcons.pixels[i];
                uint32_t a = (px >> 24) & 0xFFu;
                if (a == 0) {
                    layer.surface.pixels[i] = 0;
                }
                else if (a == 255) {
                    layer.surface.pixels[i] = px;
                }
                else {
                    uint32_t r = (((px >> 16) & 0xFFu) * a + 127u) / 255u;
                    uint32_t g = (((px >> 8) & 0xFFu) * a + 127u) / 255u;
                    uint32_t b = ((px & 0xFFu) * a + 127u) / 255u;
                    layer.surface.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
        else {
            for (size_t i = 0; i < pixelCount; ++i) {
                uint32_t px = tmpIcons.pixels[i];
                uint32_t r = (px >> 16) & 0xFFu;
                uint32_t g = (px >> 8) & 0xFFu;
                uint32_t b = px & 0xFFu;
                uint32_t maxC = (std::max)(r, (std::max)(g, b));

                if (maxC == 0) {
                    layer.surface.pixels[i] = 0x00000000u;
                }
                else if (maxC >= 48) {
                    layer.surface.pixels[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
                else {
                    uint32_t alpha = (maxC * 255u) / 48u;
                    uint32_t pr = (r * alpha + 127u) / 255u;
                    uint32_t pg = (g * alpha + 127u) / 255u;
                    uint32_t pb = (b * alpha + 127u) / 255u;
                    layer.surface.pixels[i] = (alpha << 24) | (pr << 16) | (pg << 8) | pb;
                }
            }
        }

        FreeDibSurface(tmpIcons);

        layer.hWnd = hListView;
        layer.hOwner = nullptr;
        layer.rootOwnerHwnd = nullptr;
        layer.title = L"Desktop Icons";
        layer.processName = L"explorer.exe";
        layer.rcWindow = rcListView;
        layer.rcExtended = rcListView;
        layer.visible = true;
        layer.autoHidden = false;
        layer.isTaskbar = false;
        layer.isDesktopIcons = true;

        g_app.layers.push_back(std::move(layer));
    }

    bool CaptureSingleTaskbarWindow(HWND hWnd, const RECT& rcVirtual, WindowLayer& outLayer) {
        if (!hWnd || !IsWindowVisible(hWnd) || IsIconic(hWnd)) {
            return false;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) {
            return false;
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return false;
        }

        RECT rcWindow{};
        if (!GetWindowRect(hWnd, &rcWindow)) {
            return false;
        }
        int winW = rcWindow.right - rcWindow.left;
        int winH = rcWindow.bottom - rcWindow.top;
        if (winW <= 0 || winH <= 0) {
            return false;
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
            return false;
        }

        RECT rcClip{};
        if (!IntersectRect(&rcClip, &rcExtended, &rcVirtual)) {
            return false;
        }

        DWORD_PTR unused = 0;
        bool canPrintWindow = !IsHungAppWindow(hWnd) &&
                              SendMessageTimeoutW(hWnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &unused) != 0;

        WindowLayer layer{};
        layer.hWnd = hWnd;
        layer.hOwner = GetWindow(hWnd, GW_OWNER);
        layer.rcWindow = rcWindow;
        layer.rcExtended = rcExtended;
        layer.processName = QueryWindowProcessName(pid);
        layer.title = L"Taskbar";

        if (!CreateDibSurface(layer.surface, winW, winH)) {
            return false;
        }
        std::memset(layer.surface.pixels, 0, static_cast<size_t>(winW) * static_cast<size_t>(winH) * sizeof(uint32_t));

        BOOL captured = FALSE;
        if (canPrintWindow) {
            captured = PrintWindow(hWnd, layer.surface.hDC, PW_RENDERFULLCONTENT);
            if (!captured) {
                captured = PrintWindow(hWnd, layer.surface.hDC, 0);
            }
        }
        if (!captured || IsSurfaceAllBlack(layer.surface)) {
            captured = CopyRectFromClassicSnapshot(layer.surface, rcWindow, rcExtended, rcVirtual) ? TRUE : FALSE;
        }
        if (!captured) {
            FreeDibSurface(layer.surface);
            return false;
        }

        layer.isTaskbar = true;
        layer.isShellFlyout = false;
        layer.isDesktopIcons = false;
        layer.autoHidden = !g_config.showTaskbar;
        layer.visible = !layer.autoHidden;

        outLayer = std::move(layer);
        return true;
    }

    BOOL CALLBACK EnumWindowsLayersProc(HWND hWnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<EnumLayerContext*>(lParam);

        if (ctx->flyouts) {
            for (const auto& flyout : *ctx->flyouts) {
                if (flyout.hWnd == hWnd) {
                    return TRUE;
                }
            }
        }

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
        if (isTaskbar) {
            WindowLayer layer{};
            if (CaptureSingleTaskbarWindow(hWnd, ctx->rcVirtual, layer)) {
                g_app.layers.push_back(std::move(layer));
            }
            return TRUE;
        }

        bool isTrayOverflow = (wcscmp(cls, L"NotifyIconOverflowWindow") == 0);
        std::wstring procName = QueryWindowProcessName(pid);

        if (!isTrayOverflow &&
            (IsShellHostProcessName(procName) ||
             wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0)) {
            return TRUE;
        }

        LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (!isTrayOverflow && (exStyle & WS_EX_TOOLWINDOW)) {
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
        layer.processName = std::move(procName);
        layer.title = isTrayOverflow
            ? L"System Tray"
            : QueryWindowDisplayTitle(hWnd, layer.processName);

        if (!CreateDibSurface(layer.surface, winW, winH)) {
            return TRUE;
        }
        std::memset(layer.surface.pixels, 0, static_cast<size_t>(winW) * static_cast<size_t>(winH) * sizeof(uint32_t));

        BOOL captured = PrintWindow(hWnd, layer.surface.hDC, PW_RENDERFULLCONTENT);
        if (!captured) {
            captured = PrintWindow(hWnd, layer.surface.hDC, 0);
        }
        if (isTrayOverflow && (!captured || IsSurfaceAllBlack(layer.surface))) {
            captured = CopyRectFromClassicSnapshot(layer.surface, rcWindow, rcExtended, ctx->rcVirtual) ? TRUE : FALSE;
        }
        if (!captured) {
            FreeDibSurface(layer.surface);
            return TRUE;
        }

        layer.isTaskbar = false;
        layer.isShellFlyout = false;
        layer.autoHidden = IsProcessHiddenInConfig(layer.processName);
        layer.visible = !layer.autoHidden;

        g_app.layers.push_back(std::move(layer));
        return TRUE;
    }

    bool ComputeClampedShellWindowRect(HWND hWnd, RECT& outRawWindow, RECT& outExtended, MONITORINFO& outMonitorInfo, HMONITOR& outMonitor, bool& outHasMonitor) {
        if (!GetWindowRect(hWnd, &outRawWindow)) {
            return false;
        }

        RECT rcDwm{};
        if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcDwm, sizeof(rcDwm)))) {
            rcDwm = outRawWindow;
        }

        outMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        outMonitorInfo = {};
        outMonitorInfo.cbSize = sizeof(outMonitorInfo);
        outHasMonitor = (outMonitor && GetMonitorInfoW(outMonitor, &outMonitorInfo));

        if (!UnionRect(&outExtended, &outRawWindow, &rcDwm)) {
            outExtended = outRawWindow;
        }
        if (outHasMonitor) {
            outExtended.left = (std::max)(outExtended.left, outMonitorInfo.rcMonitor.left);
            outExtended.top = (std::max)(outExtended.top, outMonitorInfo.rcMonitor.top);
            outExtended.right = (std::min)(outExtended.right, outMonitorInfo.rcMonitor.right);
            outExtended.bottom = (std::min)(outExtended.bottom, outMonitorInfo.rcMonitor.bottom);
        }
        return true;
    }

    bool TryInspectShellFlyoutWindow(
        HWND hWnd,
        ShellFlyoutInfo& outInfo,
        bool logDebug = false,
        const std::wstring* knownProcName = nullptr)
    {
        if (!hWnd) {
            return false;
        }

        wchar_t cls[128]{};
        if (GetClassNameW(hWnd, cls, _countof(cls)) <= 0) {
            cls[0] = L'\0';
        }
        bool isInputSwitch = (wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0);

        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) {
            return false;
        }

        std::wstring proc = knownProcName ? *knownProcName : QueryWindowProcessName(pid);
        bool isShellHost = IsShellHostProcessName(proc);
        if (!isShellHost && !isInputSwitch) {
            return false;
        }

        RECT rcRawWindow{};
        RECT rcExtended{};

        auto reject = [&](const wchar_t* reason) -> bool {
#ifdef _DEBUG
            if (logDebug) {
                if (IsRectEmpty(&rcRawWindow)) {
                    GetWindowRect(hWnd, &rcRawWindow);
                }
                LogShellFlyoutCandidate(hWnd, cls, proc, rcRawWindow, rcExtended, reason);
            }
#else
            (void)logDebug;
            (void)reason;
#endif
            return false;
        };

        if (!IsWindowVisible(hWnd)) {
            return reject(L"SKIP: not_visible");
        }
        if (IsIconic(hWnd)) {
            return reject(L"SKIP: iconic");
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return reject(L"SKIP: cloaked");
        }
        HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (hRoot && hRoot != hWnd) {
            if (!IsWindowVisible(hRoot)) {
                return reject(L"SKIP: root_not_visible");
            }
            if (IsIconic(hRoot)) {
                return reject(L"SKIP: root_iconic");
            }
            DWORD rootCloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hRoot, DWMWA_CLOAKED, &rootCloaked, sizeof(rootCloaked))) &&
                rootCloaked != 0) {
                return reject(L"SKIP: root_cloaked");
            }
        }

        HMONITOR hMon = nullptr;
        MONITORINFO mi{};
        bool hasMon = false;
        if (!ComputeClampedShellWindowRect(hWnd, rcRawWindow, rcExtended, mi, hMon, hasMon)) {
            return reject(L"SKIP: GetWindowRect_failed");
        }

        int extW = rcExtended.right - rcExtended.left;
        int extH = rcExtended.bottom - rcExtended.top;
        if (extW < 1 || extH < 1) {
            return reject(L"SKIP: empty_rect");
        }

        int monW = 0;
        int monH = 0;
        if (hasMon) {
            monW = mi.rcMonitor.right - mi.rcMonitor.left;
            monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (monW > 0 && monH > 0 &&
                !(extW < monW * 95 / 100 || extH < monH * 95 / 100)) {
                return reject(L"SKIP: fullscreen");
            }
        }

        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT rcVirtual = { vx, vy, vx + vw, vy + vh };
        RECT rcClip{};
        if (!IntersectRect(&rcClip, &rcExtended, &rcVirtual)) {
            return reject(L"SKIP: offscreen");
        }

        bool isStartMenu = (_wcsicmp(proc.c_str(), L"StartMenuExperienceHost.exe") == 0);
        std::wstring title;
        if (isStartMenu) {
            title = L"Start Menu";
        }
        else if (isInputSwitch) {
            title = L"Language Switcher";
        }
        else if (_wcsicmp(proc.c_str(), L"SearchApp.exe") == 0 ||
                 _wcsicmp(proc.c_str(), L"SearchUI.exe") == 0) {
            title = L"Search";
        }
        else if (_wcsicmp(proc.c_str(), L"TextInputHost.exe") == 0) {
            title = L"Language Switcher";
        }
        else if (_wcsicmp(proc.c_str(), L"ShellExperienceHost.exe") == 0) {
            if (extW > 100) {
                HWND hFg = GetForegroundWindow();
                bool isForeground = (hWnd == hFg || (hRoot && hRoot == hFg));
                bool isCompactFlyout = (monW > 0 && monH > 0 &&
                                        extW < monW * 70 / 100 &&
                                        extH < monH * 85 / 100);
                if (!isForeground && !isCompactFlyout) {
                    return reject(L"SKIP: shell_exp_bg_container");
                }
            }
            title = L"System Flyout";
        }
        else {
            return reject(L"SKIP: unknown_process");
        }

#ifdef _DEBUG
        if (logDebug) {
            LogShellFlyoutCandidate(hWnd, cls, proc, rcRawWindow, rcExtended, L"ACCEPTED");
        }
#endif

        outInfo.hWnd = hWnd;
        outInfo.processName = std::move(proc);
        outInfo.title = std::move(title);
        outInfo.rcWindow = rcExtended;
        outInfo.rcExtended = rcExtended;
        return true;
    }

    void CaptureShellFlyoutLayers(const RECT& rcVirtual, const std::vector<ShellFlyoutInfo>& flyouts) {
        if (flyouts.empty()) {
            return;
        }

        g_hadOpenShellFlyout = true;

        if (!g_app.classicSnapshot.hDC) {
            return;
        }

        std::vector<WindowLayer> flyoutLayers;
        flyoutLayers.reserve(flyouts.size());

        for (const auto& flyout : flyouts) {
            int winW = flyout.rcWindow.right - flyout.rcWindow.left;
            int winH = flyout.rcWindow.bottom - flyout.rcWindow.top;
            if (winW <= 0 || winH <= 0) {
                continue;
            }

            WindowLayer layer{};
            layer.hWnd = flyout.hWnd;
            layer.hOwner = GetWindow(flyout.hWnd, GW_OWNER);
            layer.rcWindow = flyout.rcWindow;
            layer.rcExtended = flyout.rcExtended;
            layer.processName = flyout.processName;
            layer.title = flyout.title;

            if (!CreateDibSurface(layer.surface, winW, winH)) {
                continue;
            }

            if (!CopyRectFromClassicSnapshot(layer.surface, flyout.rcWindow, flyout.rcExtended, rcVirtual)) {
                FreeDibSurface(layer.surface);
                continue;
            }

            layer.isTaskbar = false;
            layer.isShellFlyout = true;
            layer.isDesktopIcons = false;
            layer.autoHidden = IsProcessHiddenInConfig(layer.processName);
            layer.visible = !layer.autoHidden;

            flyoutLayers.push_back(std::move(layer));
        }

        if (!flyoutLayers.empty()) {
            HWND startRootHwnd = nullptr;
            bool startRootAutoHidden = false;
            long long largestStartArea = -1;
            for (const auto& layer : flyoutLayers) {
                if (_wcsicmp(layer.processName.c_str(), L"StartMenuExperienceHost.exe") == 0) {
                    long long area = static_cast<long long>(layer.rcExtended.right - layer.rcExtended.left) *
                                     static_cast<long long>(layer.rcExtended.bottom - layer.rcExtended.top);
                    if (area > largestStartArea) {
                        largestStartArea = area;
                        startRootHwnd = layer.hWnd;
                        startRootAutoHidden = layer.autoHidden;
                    }
                }
            }
            if (startRootHwnd != nullptr) {
                for (auto& layer : flyoutLayers) {
                    if (_wcsicmp(layer.processName.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
                        layer.title == L"Start Menu") {
                        layer.rootOwnerHwnd = startRootHwnd;
                        layer.autoHidden = startRootAutoHidden;
                        layer.visible = !startRootAutoHidden;
                    }
                }
            }

            g_app.layers.insert(
                g_app.layers.begin(),
                std::make_move_iterator(flyoutLayers.begin()),
                std::make_move_iterator(flyoutLayers.end()));
        }
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

    void CaptureMissingTaskbarLayers(const RECT& rcVirtual) {
        const wchar_t* const kTaskbarClasses[] = {
            L"Shell_TrayWnd",
            L"Shell_SecondaryTrayWnd"
        };

        std::vector<WindowLayer> missingTaskbars;
        for (const wchar_t* clsName : kTaskbarClasses) {
            HWND hPrev = nullptr;
            int guard = 0;
            while ((hPrev = FindWindowExW(nullptr, hPrev, clsName, nullptr)) != nullptr &&
                   guard++ < 64) {
                if (FindLayerIndexByHwnd(hPrev) >= 0) {
                    continue;
                }
                bool alreadyQueued = false;
                for (const auto& queued : missingTaskbars) {
                    if (queued.hWnd == hPrev) {
                        alreadyQueued = true;
                        break;
                    }
                }
                if (alreadyQueued) {
                    continue;
                }

                WindowLayer layer{};
                if (CaptureSingleTaskbarWindow(hPrev, rcVirtual, layer)) {
                    missingTaskbars.push_back(std::move(layer));
                }
            }
        }

        if (!missingTaskbars.empty()) {
            size_t insertPos = 0;
            while (insertPos < g_app.layers.size() && g_app.layers[insertPos].isShellFlyout) {
                ++insertPos;
            }
            g_app.layers.insert(
                g_app.layers.begin() + static_cast<ptrdiff_t>(insertPos),
                std::make_move_iterator(missingTaskbars.begin()),
                std::make_move_iterator(missingTaskbars.end()));
        }
    }

    void ResolveOwnerHierarchy() {
        for (auto& layer : g_app.layers) {
            if (layer.rootOwnerHwnd != nullptr) {
                continue;
            }
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

    struct FlyoutCollectContext {
        DWORD selfPid = 0;
        std::unordered_map<DWORD, std::wstring> pidToProc;
        std::unordered_set<HWND> seenHwnds;
        std::vector<ShellFlyoutInfo>* flyouts = nullptr;
    };

    const std::wstring& QueryProcessNameCached(FlyoutCollectContext& ctx, DWORD pid) {
        static const std::wstring kEmpty;
        if (pid == 0 || pid == ctx.selfPid) {
            return kEmpty;
        }
        auto it = ctx.pidToProc.find(pid);
        if (it != ctx.pidToProc.end()) {
            return it->second;
        }
        auto [inserted, _] = ctx.pidToProc.emplace(pid, QueryWindowProcessName(pid));
        return inserted->second;
    }

    void TryAddShellFlyoutCandidate(
        FlyoutCollectContext& ctx,
        HWND hWnd,
        const std::wstring* knownProcName = nullptr,
        bool logDebug = true)
    {
        if (!hWnd || !ctx.seenHwnds.insert(hWnd).second) {
            return;
        }
        ShellFlyoutInfo info{};
        if (TryInspectShellFlyoutWindow(hWnd, info, logDebug, knownProcName)) {
            ctx.flyouts->push_back(std::move(info));
        }
    }

    BOOL CALLBACK EnumShellFlyoutChildProc(HWND hChild, LPARAM lParam) {
        auto* ctx = reinterpret_cast<FlyoutCollectContext*>(lParam);
        if (!ctx || !ctx->flyouts) {
            return FALSE;
        }
        DWORD childPid = 0;
        GetWindowThreadProcessId(hChild, &childPid);
        const std::wstring& proc = QueryProcessNameCached(*ctx, childPid);

        bool isShellHost = IsShellHostProcessName(proc);
        if (!isShellHost) {
            wchar_t cls[64]{};
            if (GetClassNameW(hChild, cls, _countof(cls)) > 0 &&
                wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0) {
                TryAddShellFlyoutCandidate(*ctx, hChild, &proc, true);
            }
            return TRUE;
        }

        TryAddShellFlyoutCandidate(*ctx, hChild, &proc, true);
        return TRUE;
    }

    bool IsExcludedExplorerRailClass(const wchar_t* cls) {
        return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
               wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
               wcscmp(cls, L"Progman") == 0 ||
               wcscmp(cls, L"WorkerW") == 0 ||
               wcscmp(cls, L"NotifyIconOverflowWindow") == 0 ||
               wcscmp(cls, L"CabinetWClass") == 0 ||
               wcscmp(cls, L"ExploreWClass") == 0;
    }

}

std::vector<ShellFlyoutInfo> CollectActiveShellFlyouts() {
    std::vector<ShellFlyoutInfo> flyouts;

    FlyoutCollectContext collectCtx{};
    collectCtx.selfPid = GetCurrentProcessId();
    collectCtx.flyouts = &flyouts;

    std::vector<HWND> explorerTopLevel;

    HWND hPrev = nullptr;
    int guard = 0;
    while ((hPrev = FindWindowExW(nullptr, hPrev, nullptr, nullptr)) != nullptr &&
           guard++ < 4096) {
        DWORD topPid = 0;
        GetWindowThreadProcessId(hPrev, &topPid);
        const std::wstring& proc = QueryProcessNameCached(collectCtx, topPid);

        if (_wcsicmp(proc.c_str(), L"explorer.exe") == 0) {
            explorerTopLevel.push_back(hPrev);
        }

        bool isShellHost = IsShellHostProcessName(proc);
        bool isInputSwitch = false;
        if (!isShellHost) {
            wchar_t cls[64]{};
            if (GetClassNameW(hPrev, cls, _countof(cls)) > 0 &&
                wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0) {
                isInputSwitch = true;
            }
        }

        if (isShellHost || isInputSwitch) {
            TryAddShellFlyoutCandidate(collectCtx, hPrev, &proc, true);
        }

        if (!IsWindowVisible(hPrev) || IsIconic(hPrev)) {
            continue;
        }
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hPrev, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
            cloaked != 0) {
            continue;
        }

        EnumChildWindows(hPrev, EnumShellFlyoutChildProc, reinterpret_cast<LPARAM>(&collectCtx));
    }

    struct StartPaneByMonitor {
        HMONITOR hMon = nullptr;
        RECT rcMain{};
        long long area = -1;
    };
    std::vector<StartPaneByMonitor> startPanes;
    for (const auto& flyout : flyouts) {
        if (_wcsicmp(flyout.processName.c_str(), L"StartMenuExperienceHost.exe") != 0) {
            continue;
        }
        HMONITOR hMon = MonitorFromRect(&flyout.rcExtended, MONITOR_DEFAULTTONEAREST);
        if (!hMon) {
            continue;
        }
        long long area = static_cast<long long>(flyout.rcExtended.right - flyout.rcExtended.left) *
                         static_cast<long long>(flyout.rcExtended.bottom - flyout.rcExtended.top);
        bool found = false;
        for (auto& pane : startPanes) {
            if (pane.hMon == hMon) {
                if (area > pane.area) {
                    pane.rcMain = flyout.rcExtended;
                    pane.area = area;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            startPanes.push_back({ hMon, flyout.rcExtended, area });
        }
    }

    if (!startPanes.empty()) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT rcVirtual = { vx, vy, vx + vw, vy + vh };

        for (HWND hWnd : explorerTopLevel) {
            if (!collectCtx.seenHwnds.insert(hWnd).second) {
                continue;
            }

            wchar_t cls[128]{};
            if (GetClassNameW(hWnd, cls, _countof(cls)) <= 0) {
                cls[0] = L'\0';
            }
            const std::wstring procName = L"explorer.exe";

            RECT rcRawWindow{};
            RECT rcExtended{};
            auto rejectExplorer = [&](const wchar_t* reason) {
#ifdef _DEBUG
                if (IsRectEmpty(&rcRawWindow)) {
                    GetWindowRect(hWnd, &rcRawWindow);
                }
                LogShellFlyoutCandidate(hWnd, cls, procName, rcRawWindow, rcExtended, reason);
#else
                (void)reason;
#endif
            };

            if (IsExcludedExplorerRailClass(cls)) {
                continue;
            }

            if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) {
                rejectExplorer(L"SKIP: explorer_not_visible");
                continue;
            }

            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
                rejectExplorer(L"SKIP: explorer_cloaked");
                continue;
            }
            HWND hRoot = GetAncestor(hWnd, GA_ROOT);
            if (hRoot && hRoot != hWnd) {
                if (!IsWindowVisible(hRoot) || IsIconic(hRoot)) {
                    rejectExplorer(L"SKIP: explorer_root_not_visible");
                    continue;
                }
                DWORD rootCloaked = 0;
                if (SUCCEEDED(DwmGetWindowAttribute(hRoot, DWMWA_CLOAKED, &rootCloaked, sizeof(rootCloaked))) &&
                    rootCloaked != 0) {
                    rejectExplorer(L"SKIP: explorer_root_cloaked");
                    continue;
                }
            }

            LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
            LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
            bool isPopupLike = ((style & WS_POPUP) != 0) ||
                               ((exStyle & WS_EX_TOOLWINDOW) != 0) ||
                               ((style & WS_CAPTION) == 0);
            if (!isPopupLike) {
                rejectExplorer(L"SKIP: explorer_not_popup");
                continue;
            }

            HMONITOR hMon = nullptr;
            MONITORINFO mi{};
            bool hasMon = false;
            if (!ComputeClampedShellWindowRect(hWnd, rcRawWindow, rcExtended, mi, hMon, hasMon) || !hasMon) {
                rejectExplorer(L"SKIP: explorer_rect_or_monitor_failed");
                continue;
            }

            int extW = rcExtended.right - rcExtended.left;
            int extH = rcExtended.bottom - rcExtended.top;
            if (extW < 1 || extH < 1) {
                rejectExplorer(L"SKIP: explorer_empty_rect");
                continue;
            }

            int monW = mi.rcMonitor.right - mi.rcMonitor.left;
            int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (monW > 0 && monH > 0 &&
                !(extW < monW * 95 / 100 || extH < monH * 95 / 100)) {
                rejectExplorer(L"SKIP: explorer_fullscreen");
                continue;
            }

            RECT rcClip{};
            if (!IntersectRect(&rcClip, &rcExtended, &rcVirtual)) {
                rejectExplorer(L"SKIP: explorer_offscreen");
                continue;
            }

            const RECT* pStartMain = nullptr;
            for (const auto& pane : startPanes) {
                if (pane.hMon == hMon) {
                    pStartMain = &pane.rcMain;
                    break;
                }
            }
            if (!pStartMain) {
                rejectExplorer(L"SKIP: explorer_different_monitor");
                continue;
            }

            RECT rcExpanded = {
                pStartMain->left - 4,
                pStartMain->top - 4,
                pStartMain->right + 4,
                pStartMain->bottom + 4
            };
            RECT rcTouch{};
            if (!IntersectRect(&rcTouch, &rcExpanded, &rcExtended)) {
                rejectExplorer(L"SKIP: explorer_not_adjacent");
                continue;
            }

#ifdef _DEBUG
            LogShellFlyoutCandidate(hWnd, cls, procName, rcRawWindow, rcExtended, L"ACCEPTED: explorer_start_rail");
#endif

            ShellFlyoutInfo info{};
            info.hWnd = hWnd;
            info.processName = procName;
            info.title = L"Start Menu";
            info.rcWindow = rcExtended;
            info.rcExtended = rcExtended;
            flyouts.push_back(std::move(info));
        }
    }

    return flyouts;
}

bool IsShellFlyoutWindow(HWND hWnd, std::wstring* outProcessName) {
    ShellFlyoutInfo info{};
    if (!TryInspectShellFlyoutWindow(hWnd, info)) {
        return false;
    }
    if (outProcessName) {
        *outProcessName = std::move(info.processName);
    }
    return true;
}

bool HasOpenShellFlyout() {
    if (g_hadOpenShellFlyout) {
        return true;
    }
    return !CollectActiveShellFlyouts().empty();
}

void FreeLayerStack() {
    for (auto& layer : g_app.layers) {
        FreeDibSurface(layer.surface);
    }
    g_app.layers.clear();
    g_app.peelHistory.clear();
    g_app.hoveredLayerIndex = -1;
    g_hadOpenShellFlyout = false;
    FreeDibSurface(g_app.desktopLayer);
}

bool CaptureLayerStack(const std::vector<ShellFlyoutInfo>* precollectedFlyouts) {
    FreeLayerStack();

    std::vector<ShellFlyoutInfo> ownedFlyouts;
    const std::vector<ShellFlyoutInfo>* flyouts = precollectedFlyouts;
    if (!flyouts) {
        ownedFlyouts = CollectActiveShellFlyouts();
        flyouts = &ownedFlyouts;
    }

    HWND hProgman = FindWindowW(L"Progman", nullptr);
    HWND hWallpaperWorker = FindWallpaperWorkerW(hProgman);

    if (!CaptureDesktopBaseLayer(hProgman, hWallpaperWorker)) {
        return false;
    }

    EnumLayerContext ctx{};
    ctx.hProgman = hProgman;
    ctx.hWallpaperWorker = hWallpaperWorker;
    ctx.rcVirtual = { g_app.vx, g_app.vy, g_app.vx + g_app.vw, g_app.vy + g_app.vh };
    ctx.flyouts = flyouts;

    EnumWindows(EnumWindowsLayersProc, reinterpret_cast<LPARAM>(&ctx));
    CaptureShellFlyoutLayers(ctx.rcVirtual, *flyouts);
    CaptureMissingTaskbarLayers(ctx.rcVirtual);
    ResolveOwnerHierarchy();
    CaptureDesktopIconsLayer(hProgman);
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

        if (layer.isDesktopIcons) {
            RECT rcVirtual = { 0, 0, g_app.vw, g_app.vh };
            RECT rcDst = { dstX, dstY, dstX + copyW, dstY + copyH };
            RECT rcDstClip{};
            if (IntersectRect(&rcDstClip, &rcDst, &rcVirtual)) {
                int clipSrcX = srcX + (rcDstClip.left - dstX);
                int clipSrcY = srcY + (rcDstClip.top - dstY);
                int clipW = rcDstClip.right - rcDstClip.left;
                int clipH = rcDstClip.bottom - rcDstClip.top;
                BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(
                    g_app.pristine.hDC,
                    rcDstClip.left,
                    rcDstClip.top,
                    clipW,
                    clipH,
                    layer.surface.hDC,
                    clipSrcX,
                    clipSrcY,
                    clipW,
                    clipH,
                    bf);
            }
        }
        else {
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
        if (layer.isTaskbar || layer.isDesktopIcons) continue;
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