#pragma once
#include "blitsnap.h"
#include <string>

bool ExportCaptureToClipboardAndTemp(RECT rcVirtualScreen, std::wstring* outPngPath = nullptr);

bool ExportLayerToClipboardAndTemp(const WindowLayer& layer, std::wstring* outPngPath = nullptr);

bool GetLatestCapturePath(wchar_t* outPath, size_t cch);