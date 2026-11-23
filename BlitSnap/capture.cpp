#include "capture.h"
#include "config.h"
#include "layers.h"
#include <dwmapi.h>
#include <emmintrin.h>

#pragma comment(lib, "dwmapi.lib")

void FreeDibSurface(DibSurface& surf) {
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

bool CreateDibSurface(DibSurface& surf, int width, int height) {
    FreeDibSurface(surf);
    if (width <= 0 || height <= 0) return false;

    HDC hScreenDC = GetDC(nullptr);
    if (!hScreenDC) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hDib = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    ReleaseDC(nullptr, hScreenDC);

    if (!hMemDC || !hDib || !pBits) {
        if (hDib) DeleteObject(hDib);
        if (hMemDC) DeleteDC(hMemDC);
        return false;
    }

    surf.hOldBmp = SelectObject(hMemDC, hDib);
    surf.hDC = hMemDC;
    surf.hBitmap = hDib;
    surf.pixels = static_cast<uint32_t*>(pBits);
    surf.width = width;
    surf.height = height;
    return true;
}

void DimPixelsSSE2(const uint32_t* src, uint32_t* dst, int pixelCount) {
    if (!src || !dst || pixelCount <= 0) return;

    const __m128i zero = _mm_setzero_si128();
    const size_t total = static_cast<size_t>(pixelCount);
    size_t i = 0;

    for (; i + 4 <= total; i += 4) {
        __m128i px = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m128i half = _mm_avg_epu8(px, zero);
        __m128i quarter = _mm_avg_epu8(half, zero);
        __m128i eighth = _mm_avg_epu8(quarter, zero);
        __m128i dimmed = _mm_adds_epu8(half, eighth);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), dimmed);
    }
    for (; i < total; ++i) {
        uint32_t c = src[i];
        uint32_t r = (((c >> 16) & 0xFFu) * 160u) >> 8;
        uint32_t g = (((c >> 8) & 0xFFu) * 160u) >> 8;
        uint32_t b = ((c & 0xFFu) * 160u) >> 8;
        dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

bool CaptureAndPrepareBuffers() {
    g_app.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_app.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_app.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_app.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (g_app.vw <= 0 || g_app.vh <= 0) {
        return false;
    }

    FreeDibSurface(g_app.pristine);
    FreeDibSurface(g_app.dimmed);
    FreeDibSurface(g_app.classicSnapshot);

    if (!CreateDibSurface(g_app.pristine, g_app.vw, g_app.vh) ||
        !CreateDibSurface(g_app.dimmed, g_app.vw, g_app.vh) ||
        !CreateDibSurface(g_app.classicSnapshot, g_app.vw, g_app.vh)) {
        FreeDibSurface(g_app.pristine);
        FreeDibSurface(g_app.dimmed);
        FreeDibSurface(g_app.classicSnapshot);
        return false;
    }

    HDC hScreenDC = GetDC(nullptr);
    if (!hScreenDC) {
        FreeDibSurface(g_app.pristine);
        FreeDibSurface(g_app.dimmed);
        FreeDibSurface(g_app.classicSnapshot);
        return false;
    }

    bool ok = BitBlt(
        g_app.classicSnapshot.hDC,
        0, 0, g_app.vw, g_app.vh,
        hScreenDC,
        g_app.vx, g_app.vy,
        SRCCOPY | CAPTUREBLT) != FALSE;
    ReleaseDC(nullptr, hScreenDC);

    if (!ok) {
        FreeDibSurface(g_app.pristine);
        FreeDibSurface(g_app.dimmed);
        FreeDibSurface(g_app.classicSnapshot);
        return false;
    }

    bool hasLayers = CaptureLayerStack();

    if (g_config.layerMode && hasLayers) {
        RecompositeLayers();
    }
    else {
        BitBlt(
            g_app.pristine.hDC,
            0, 0, g_app.vw, g_app.vh,
            g_app.classicSnapshot.hDC,
            0, 0,
            SRCCOPY);
        DimPixelsSSE2(g_app.pristine.pixels, g_app.dimmed.pixels, g_app.vw * g_app.vh);
        g_app.windowRects = g_app.classicWindowRects;
    }

    return true;
}

static BOOL CALLBACK EnumWindowsSnapProc(HWND hWnd, LPARAM lParam) {
    auto* outRects = reinterpret_cast<std::vector<RECT>*>(lParam);
    if (!outRects) return FALSE;

    if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    if (hWnd == GetShellWindow()) return TRUE;

    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }

    RECT rcBounds = {};
    if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcBounds, sizeof(rcBounds)))) {
        GetWindowRect(hWnd, &rcBounds);
    }

    if (rcBounds.right <= rcBounds.left || rcBounds.bottom <= rcBounds.top) return TRUE;

    outRects->push_back(rcBounds);
    return TRUE;
}

void SnapshotVisibleWindows() {
    g_app.classicWindowRects.clear();
    EnumWindows(EnumWindowsSnapProc, reinterpret_cast<LPARAM>(&g_app.classicWindowRects));
    g_app.windowRects = g_app.classicWindowRects;
}