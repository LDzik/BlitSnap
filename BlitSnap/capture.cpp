#include "capture.h"
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

static bool CreateDibSurface(HDC hRefDC, int vx, int vy, int w, int h, DibSurface& outSurf) {
    FreeDibSurface(outSurf);
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hMemDC = CreateCompatibleDC(hRefDC);
    HBITMAP hDib = CreateDIBSection(hRefDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hMemDC || !hDib || !pBits) {
        if (hDib) DeleteObject(hDib);
        if (hMemDC) DeleteDC(hMemDC);
        return false;
    }

    outSurf.hOldBmp = SelectObject(hMemDC, hDib);
    outSurf.hDC = hMemDC;
    outSurf.hBitmap = hDib;
    outSurf.pixels = static_cast<uint32_t*>(pBits);
    outSurf.vx = vx;
    outSurf.vy = vy;
    outSurf.width = w;
    outSurf.height = h;
    return true;
}

static void DimPixelsSSE2(const uint32_t* src, uint32_t* dst, size_t pixelCount) {
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;

    for (; i + 4 <= pixelCount; i += 4) {
        __m128i px = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m128i half = _mm_avg_epu8(px, zero);
        __m128i quarter = _mm_avg_epu8(half, zero);
        __m128i eighth = _mm_avg_epu8(quarter, zero);
        __m128i dimmed = _mm_adds_epu8(half, eighth);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), dimmed);
    }
    for (; i < pixelCount; ++i) {
        uint32_t c = src[i];
        uint32_t r = (((c >> 16) & 0xFFu) * 160u) >> 8;
        uint32_t g = (((c >> 8) & 0xFFu) * 160u) >> 8;
        uint32_t b = ((c & 0xFFu) * 160u) >> 8;
        dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

bool CaptureAndPrepareBuffers() {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hScreenDC = GetDC(nullptr);
    if (!hScreenDC) return false;

    bool ok = CreateDibSurface(hScreenDC, vx, vy, vw, vh, g_app.pristine) &&
        CreateDibSurface(hScreenDC, vx, vy, vw, vh, g_app.dimmed);

    if (ok) {
        ok = BitBlt(g_app.pristine.hDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY | CAPTUREBLT) != FALSE;
    }
    ReleaseDC(nullptr, hScreenDC);

    if (!ok) {
        FreeDibSurface(g_app.pristine);
        FreeDibSurface(g_app.dimmed);
        return false;
    }

    DimPixelsSSE2(g_app.pristine.pixels, g_app.dimmed.pixels, static_cast<size_t>(vw) * static_cast<size_t>(vh));
    return true;
}

static BOOL CALLBACK EnumWindowsSnapProc(HWND hWnd, LPARAM) {
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

    g_app.windowRects.push_back(rcBounds);
    return TRUE;
}

void SnapshotVisibleWindows() {
    g_app.windowRects.clear();
    g_app.hasHoveredWin = false;
    EnumWindows(EnumWindowsSnapProc, 0);
}