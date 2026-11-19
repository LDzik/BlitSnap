#include "export.h"
#include "config.h"
#include <wincodec.h>
#include <objbase.h>
#include <shlobj.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

static std::wstring g_lastPngPath;

bool GetLatestCapturePath(wchar_t* outPath, size_t cch) {
    if (g_lastPngPath.empty()) return false;
    return wcscpy_s(outPath, cch, g_lastPngPath.c_str()) == 0;
}

static void CleanupStaleTempPngs(const std::wstring& tempDir, const std::wstring& keepPath) {
    FILETIME ftNow = {};
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER now64 = { ftNow.dwLowDateTime, ftNow.dwHighDateTime };
    constexpr ULONGLONG kTenMinutes100ns = 10ULL * 60ULL * 10000000ULL;

    std::wstring searchPattern = tempDir + L"\\*.png";
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fullPath = tempDir + L"\\" + fd.cFileName;
        if (_wcsicmp(fullPath.c_str(), keepPath.c_str()) == 0) continue;

        ULARGE_INTEGER write64 = { fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime };
        if (now64.QuadPart > write64.QuadPart && (now64.QuadPart - write64.QuadPart) > kTenMinutes100ns) {
            DeleteFileW(fullPath.c_str());
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static std::wstring WriteNamedPngToDisk(const void* data, DWORD byteCount) {
    std::wstring fileName = FormatCaptureFilename(g_config.filePattern, g_app.activeWindowTitle);
    std::wstring targetDir;

    if (g_config.autoSave && !g_config.saveFolder.empty()) {
        targetDir = g_config.saveFolder;
        SHCreateDirectoryExW(nullptr, targetDir.c_str(), nullptr);
    }
    else {
        wchar_t tempRoot[MAX_PATH] = {};
        DWORD len = GetTempPathW(MAX_PATH, tempRoot);
        if (len == 0 || len >= MAX_PATH - 32) return L"";
        targetDir = std::wstring(tempRoot) + L"BlitSnap";
        CreateDirectoryW(targetDir.c_str(), nullptr);
    }

    std::wstring fullPath = targetDir + L"\\" + fileName + L".png";

    HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data, byteCount, &written, nullptr);
    CloseHandle(hFile);

    if (!ok || written != byteCount) return L"";

    if (!g_config.autoSave) {
        CleanupStaleTempPngs(targetDir, fullPath);
    }

    g_lastPngPath = fullPath;
    return fullPath;
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

static HGLOBAL BuildDropFilesGlobal(const std::wstring& filePath) {
    if (filePath.empty()) return nullptr;

    SIZE_T pathBytes = (filePath.size() + 2) * sizeof(wchar_t);
    SIZE_T totalBytes = sizeof(DROPFILES) + pathBytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!hMem) return nullptr;

    void* pBase = GlobalLock(hMem);
    if (!pBase) {
        GlobalFree(hMem);
        return nullptr;
    }

    auto* df = static_cast<DROPFILES*>(pBase);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;

    auto* dstPath = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(pBase) + sizeof(DROPFILES));
    memcpy(dstPath, filePath.c_str(), filePath.size() * sizeof(wchar_t));

    GlobalUnlock(hMem);
    return hMem;
}

static HGLOBAL BuildDropEffectGlobal(DWORD effect) {
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!hMem) return nullptr;

    void* pBase = GlobalLock(hMem);
    if (!pBase) {
        GlobalFree(hMem);
        return nullptr;
    }

    *static_cast<DWORD*>(pBase) = effect;
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
            WriteNamedPngToDisk(pPngBytes, pngSize);

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

namespace {

bool EncodeAndInjectClipboard(
    const uint32_t* topDownPixels,
    int cropW,
    int cropH,
    std::wstring* outPngPath)
{
    if (!topDownPixels || cropW <= 0 || cropH <= 0 || !g_app.hMsgWnd) {
        return false;
    }

    g_lastPngPath.clear();
    HGLOBAL hPngGlobal = EncodePngAndSaveTemp(topDownPixels, cropW, cropH);
    HGLOBAL hDibGlobal = BuildBottomUpCfDib(topDownPixels, cropW, cropH);
    HGLOBAL hDropGlobal = BuildDropFilesGlobal(g_lastPngPath);
    HGLOBAL hEffectGlobal = hDropGlobal ? BuildDropEffectGlobal(DROPEFFECT_COPY) : nullptr;

    if (!hPngGlobal && !hDibGlobal && !hDropGlobal) return false;

    if (!OpenClipboard(g_app.hMsgWnd)) {
        if (hPngGlobal)    GlobalFree(hPngGlobal);
        if (hDibGlobal)    GlobalFree(hDibGlobal);
        if (hDropGlobal)   GlobalFree(hDropGlobal);
        if (hEffectGlobal) GlobalFree(hEffectGlobal);
        return false;
    }

    EmptyClipboard();

    static const UINT cfPng = RegisterClipboardFormatW(L"PNG");
    static const UINT cfDropEffect = RegisterClipboardFormatW(L"Preferred DropEffect");

    if (hDropGlobal && !SetClipboardData(CF_HDROP, hDropGlobal)) {
        GlobalFree(hDropGlobal);
    }
    if (hEffectGlobal && !SetClipboardData(cfDropEffect, hEffectGlobal)) {
        GlobalFree(hEffectGlobal);
    }
    if (hPngGlobal && !SetClipboardData(cfPng, hPngGlobal)) {
        GlobalFree(hPngGlobal);
    }
    if (hDibGlobal && !SetClipboardData(CF_DIB, hDibGlobal)) {
        GlobalFree(hDibGlobal);
    }

    CloseClipboard();

    if (outPngPath) {
        *outPngPath = g_lastPngPath;
    }
    return true;
}

bool ExportSurfaceSubRect(
    const DibSurface& surface,
    RECT rcLocalCrop,
    std::wstring* outPngPath)
{
    if (!surface.pixels || surface.width <= 0 || surface.height <= 0 || !g_app.hMsgWnd) {
        return false;
    }

    RECT rcBounds{ 0, 0, surface.width, surface.height };
    RECT rcClamped{};
    if (!IntersectRect(&rcClamped, &rcLocalCrop, &rcBounds)) {
        return false;
    }

    int cropW = rcClamped.right - rcClamped.left;
    int cropH = rcClamped.bottom - rcClamped.top;
    if (cropW <= 0 || cropH <= 0) {
        return false;
    }

    std::vector<uint32_t> cropPixels(static_cast<size_t>(cropW) * static_cast<size_t>(cropH));
    for (int y = 0; y < cropH; ++y) {
        const uint32_t* srcRow = surface.pixels + static_cast<size_t>(rcClamped.top + y) * surface.width + rcClamped.left;
        uint32_t* dstRow = cropPixels.data() + static_cast<size_t>(y) * cropW;
        for (int x = 0; x < cropW; ++x) {
            dstRow[x] = srcRow[x] | 0xFF000000u; // Enforce opaque alpha for PNG/DIB
        }
    }

    return EncodeAndInjectClipboard(cropPixels.data(), cropW, cropH, outPngPath);
}

} // namespace

bool ExportCaptureToClipboardAndTemp(RECT rcVirtualScreen, std::wstring* outPngPath) {
    RECT rcLocal{
        rcVirtualScreen.left - g_app.vx,
        rcVirtualScreen.top - g_app.vy,
        rcVirtualScreen.right - g_app.vx,
        rcVirtualScreen.bottom - g_app.vy
    };
    return ExportSurfaceSubRect(g_app.pristine, rcLocal, outPngPath);
}

bool ExportLayerToClipboardAndTemp(const WindowLayer& layer, std::wstring* outPngPath) {
    RECT rcLocal{
        layer.rcExtended.left - layer.rcWindow.left,
        layer.rcExtended.top - layer.rcWindow.top,
        layer.rcExtended.right - layer.rcWindow.left,
        layer.rcExtended.bottom - layer.rcWindow.top
    };
    return ExportSurfaceSubRect(layer.surface, rcLocal, outPngPath);
}