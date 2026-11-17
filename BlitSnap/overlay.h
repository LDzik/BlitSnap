#pragma once
#include "blitsnap.h"

void RegisterOverlayClass(HINSTANCE hInst);
void BeginCaptureSession();
void EndCaptureSession();
void SetCaptureMode(CaptureMode newMode);
void CaptureFullVirtualScreen();