#pragma once

#include "blitsnap.h"
#include <string>

bool CaptureLayerStack();
void RecompositeLayers();

bool PeelLayerGroupAtPoint(POINT pt);
bool UnpeelLastLayerGroup();

bool ToggleLayerGroupVisibility(size_t index);

void ShowAllLayers();

void ApplyProcessPrivacyLock(const std::wstring& exeName, bool locked);

void ToggleCaptureLayerMode();
void FreeLayerStack();