#pragma once
#include "blitsnap.h"

bool ExportCaptureToClipboardAndTemp(const RECT& rcVirtualScreen);
bool GetLatestCapturePath(wchar_t* outPath, size_t cch);