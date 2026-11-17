#pragma once
#include "blitsnap.h"

void FreeDibSurface(DibSurface& surf);
bool CaptureAndPrepareBuffers();
void SnapshotVisibleWindows();