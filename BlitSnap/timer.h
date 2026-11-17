#pragma once
#include "blitsnap.h"

void StartDelayedCapture(int seconds);
void CancelDelayedCapture();
bool IsDelayedCaptureActive();
void HandleDelayedCaptureTick();