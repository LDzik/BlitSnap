#pragma once
#include "blitsnap.h"
#include <string>

struct AppConfig {
    bool         autoSave = false;
    std::wstring saveFolder;
    std::wstring filePattern = L"BlitSnap_{date}_{time}_{app}";
    std::wstring editorPath;
    bool         showToast = true;
};

extern AppConfig g_config;

void LoadConfig();
void SaveConfig();
std::wstring SanitizeFilenameComponent(const std::wstring& raw);
std::wstring FormatCaptureFilename(const std::wstring& pattern, const std::wstring& activeAppTitle);