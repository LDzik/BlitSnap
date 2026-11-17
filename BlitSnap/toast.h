#pragma once
#include "blitsnap.h"

void InitToastSubsystem();
void ShutdownToastSubsystem();
void ShowCaptureToast(const wchar_t* pngPath);
void LaunchEditorForLatestCapture();