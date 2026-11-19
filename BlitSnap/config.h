#pragma once
#include "blitsnap.h"
#include <string>

struct AppConfig {
    bool         autoSave = false;
    bool         showToast = true;
    bool         layerMode = true;
    bool         showTaskbar = true;
    std::wstring saveFolder;
    std::wstring filePattern = L"BlitSnap_{date}_{time}_{app}";
    std::wstring editorPath;

    std::vector<std::wstring> hiddenProcesses;
};

extern AppConfig g_config;

void LoadConfig();
void SaveConfig();

bool IsProcessHiddenInConfig(const std::wstring& exeName);
void SetProcessHiddenInConfig(const std::wstring& exeName, bool hidden);

std::wstring SanitizeFilenameComponent(const std::wstring& raw);
std::wstring FormatCaptureFilename(const std::wstring& pattern, const std::wstring& activeAppTitle);