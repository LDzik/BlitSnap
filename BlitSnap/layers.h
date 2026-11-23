#pragma once

#include "blitsnap.h"
#include <string>
#include <vector>

struct ShellFlyoutInfo {
    HWND         hWnd = nullptr;
    std::wstring processName;
    std::wstring title;
    RECT         rcWindow{};
    RECT         rcExtended{};
};

std::vector<ShellFlyoutInfo> CollectActiveShellFlyouts();
bool IsShellFlyoutWindow(HWND hWnd, std::wstring* outProcessName = nullptr);
bool HasOpenShellFlyout();

bool CaptureLayerStack(const std::vector<ShellFlyoutInfo>* precollectedFlyouts = nullptr);
void RecompositeLayers();

bool PeelLayerGroupAtPoint(POINT pt);
bool UnpeelLastLayerGroup();

bool ToggleLayerGroupVisibility(size_t index);

void ShowAllLayers();

void ApplyProcessPrivacyLock(const std::wstring& exeName, bool locked);

void ToggleCaptureLayerMode();
void FreeLayerStack();