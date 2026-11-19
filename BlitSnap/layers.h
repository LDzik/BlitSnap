#pragma once

#include "blitsnap.h"
#include <string>

bool CaptureLayerStack();
void RecompositeLayers();

bool PeelLayerGroupAtPoint(POINT pt);
bool UnpeelLastLayerGroup();

// Toggles visibility for layer[index] and any owned windows sharing its rootOwnerHwnd.
bool ToggleLayerGroupVisibility(size_t index);

// Restores visibility for all captured layers and clears peelHistory.
void ShowAllLayers();

// Updates autoHidden/visible across all captured layers matching exeName and recomposites.
void ApplyProcessPrivacyLock(const std::wstring& exeName, bool locked);

void ToggleCaptureLayerMode();
void FreeLayerStack();