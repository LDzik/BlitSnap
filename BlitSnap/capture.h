#pragma once
#include "blitsnap.h"

bool CreateDibSurface(DibSurface& surf, int width, int height);
void FreeDibSurface(DibSurface& surf);
void DimPixelsSSE2(const uint32_t* src, uint32_t* dst, int pixelCount);
bool CaptureAndPrepareBuffers();
void SnapshotVisibleWindows();