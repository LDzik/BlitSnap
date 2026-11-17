#include "export.h"
#include <wincodec.h>
#include <objbase.h>
#include <wchar.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static bool SaveBytesToTempPng(const void* data, DWORD byteCount) {
    wchar_t tempRoot[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempRoot);
    if (len == 0 || len >= MAX_PATH - 32) return false;

    wchar_t folderPath[MAX_PATH] = {};
    swprintf_s(folderPath, L"%sBlitSnap", tempRoot);
    CreateDirectoryW(folderPath, nullptr);

    wchar_t filePath[MAX_PATH] = {};
    swprintf_s(filePath, L"%s\\latest.png", folderPath);

    HANDLE hFile = CreateFileW(filePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data, byteCount, &written, nullptr);
    CloseHandle(hFile);
    return ok && (written == byteCount);
}

static HGLOBAL BuildBottomUpCfDib(const uint32_t* topDownPixels, int w, int h) {
    SIZE_T rowBytes = static_cast<SIZE_T>(w) * 4;
    SIZE_T pixelBytes = rowBytes * static_cast<SIZE_T>(h);
    SIZE_T totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
    if (!hMem) return nullptr;

    void* pBase = GlobalLock(hMem);
    if (!pBase) {
        GlobalFree(hMem);
        return nullptr;
    }

    auto* bih = static_cast<BITMAPINFOHEADER*>(pBase);
    ZeroMemory(bih, sizeof(BITMAPINFOHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = w;
    bih->biHeight = h;
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;

    auto* dstBits = reinterpret_cast<uint8_t*>(bih + 1);
    for (int y = 0; y < h; ++y) {
        const uint32_t* srcRow = topDownPixels + static_cast<SIZE_T>(h - 1 - y) * w;
        memcpy(dstBits + static_cast<SIZE_T>(y) * rowBytes, srcRow, rowBytes);
    }

    GlobalUnlock(hMem);
    return hMem;
}

static HGLOBAL EncodePngAndSaveTemp(const uint32_t* topDownPixels, int w, int h) {
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) return nullptr;

    IStream* pStream = nullptr;

    hr = CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
    if (FAILED(hr) || !pStream) {
        pFactory->Release();
        return nullptr;
    }

    IWICBitmapEncoder* pEncoder = nullptr;
    IWICBitmapFrameEncode* pFrame = nullptr;
    HGLOBAL hExactPngGlobal = nullptr;

    do {
        hr = pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder);
        if (FAILED(hr)) break;

        hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) break;

        hr = pEncoder->CreateNewFrame(&pFrame, nullptr);
        if (FAILED(hr)) break;

        hr = pFrame->Initialize(nullptr);
        if (FAILED(hr)) break;

        hr = pFrame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
        if (FAILED(hr)) break;

        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = pFrame->SetPixelFormat(&fmt);
        if (FAILED(hr)) break;

        UINT stride = static_cast<UINT>(w) * 4;
        UINT bufSize = stride * static_cast<UINT>(h);
        hr = pFrame->WritePixels(
            static_cast<UINT>(h),
            stride,
            bufSize,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(topDownPixels))
        );
        if (FAILED(hr)) break;

        hr = pFrame->Commit();
        if (FAILED(hr)) break;

        hr = pEncoder->Commit();
        if (FAILED(hr)) break;

        STATSTG stat = {};
        if (FAILED(pStream->Stat(&stat, STATFLAG_NONAME))) break;
        DWORD pngSize = stat.cbSize.LowPart;
        if (pngSize == 0) break;

        HGLOBAL hStreamMem = nullptr;
        if (FAILED(GetHGlobalFromStream(pStream, &hStreamMem)) || !hStreamMem) break;

        void* pPngBytes = GlobalLock(hStreamMem);
        if (pPngBytes) {
            SaveBytesToTempPng(pPngBytes, pngSize);

            hExactPngGlobal = GlobalAlloc(GMEM_MOVEABLE, pngSize);
            if (hExactPngGlobal) {
                void* pDst = GlobalLock(hExactPngGlobal);
                if (pDst) {
                    memcpy(pDst, pPngBytes, pngSize);
                    GlobalUnlock(hExactPngGlobal);
                }
                else {
                    GlobalFree(hExactPngGlobal);
                    hExactPngGlobal = nullptr;
                }
            }
            GlobalUnlock(hStreamMem);
        }
    } while (false);

    if (pFrame)   pFrame->Release();
    if (pEncoder) pEncoder->Release();
    pStream->Release();
    pFactory->Release();

    return hExactPngGlobal;
}

bool ExportCaptureToClipboardAndTemp(const RECT& rcVirtualScreen) {
    if (!g_app.pristine.pixels || !g_app.hMsgWnd) return false;

    RECT rcBounds = {
        g_app.pristine.vx,
        g_app.pristine.vy,
        g_app.pristine.vx + g_app.pristine.width,
        g_app.pristine.vy + g_app.pristine.height
    };
    RECT rcCrop = {};
    if (!IntersectRect(&rcCrop, &rcVirtualScreen, &rcBounds)) return false;

    int cropW = rcCrop.right - rcCrop.left;
    int cropH = rcCrop.bottom - rcCrop.top;
    if (cropW <= 0 || cropH <= 0) return false;

    int offsetX = rcCrop.left - g_app.pristine.vx;
    int offsetY = rcCrop.top - g_app.pristine.vy;
    int srcStride = g_app.pristine.width;

    std::vector<uint32_t> cropPixels(static_cast<SIZE_T>(cropW) * static_cast<SIZE_T>(cropH));
    for (int y = 0; y < cropH; ++y) {
        const uint32_t* srcRow = g_app.pristine.pixels + static_cast<SIZE_T>(offsetY + y) * srcStride + offsetX;
        uint32_t* dstRow = cropPixels.data() + static_cast<SIZE_T>(y) * cropW;
        for (int x = 0; x < cropW; ++x) {
            dstRow[x] = srcRow[x] | 0xFF000000u;
        }
    }

    HGLOBAL hPngGlobal = EncodePngAndSaveTemp(cropPixels.data(), cropW, cropH);
    HGLOBAL hDibGlobal = BuildBottomUpCfDib(cropPixels.data(), cropW, cropH);
    if (!hPngGlobal && !hDibGlobal) return false;

    if (!OpenClipboard(g_app.hMsgWnd)) {
        if (hPngGlobal) GlobalFree(hPngGlobal);
        if (hDibGlobal) GlobalFree(hDibGlobal);
        return false;
    }

    EmptyClipboard();

    static const UINT cfPng = RegisterClipboardFormatW(L"PNG");
    if (hPngGlobal) {
        if (!SetClipboardData(cfPng, hPngGlobal)) {
            GlobalFree(hPngGlobal);
        }
    }
    if (hDibGlobal) {
        if (!SetClipboardData(CF_DIB, hDibGlobal)) {
            GlobalFree(hDibGlobal);
        }
    }

    CloseClipboard();
    return true;
}