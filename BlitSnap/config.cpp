#include "config.h"
#include <shlobj.h>
#include <wchar.h>
#include <random>
#include <algorithm>
#include <cwctype>

AppConfig g_config = {};

static std::wstring GetConfigFilePath() {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || !appData) {
        return L"";
    }
    std::wstring dir = std::wstring(appData) + L"\\BlitSnap";
    CoTaskMemFree(appData);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
}

static std::wstring GetDefaultScreenshotsFolder() {
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) && pics) {
        std::wstring folder = std::wstring(pics) + L"\\BlitSnap";
        CoTaskMemFree(pics);
        return folder;
    }
    return L"C:\\BlitSnap";
}

static std::vector<std::wstring> ParseProcessList(const std::wstring& raw) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t sep = raw.find_first_of(L",;", pos);
        std::wstring token = (sep == std::wstring::npos)
            ? raw.substr(pos)
            : raw.substr(pos, sep - pos);

        size_t first = token.find_first_not_of(L" \t\r\n");
        if (first != std::wstring::npos) {
            size_t last = token.find_last_not_of(L" \t\r\n");
            token = token.substr(first, last - first + 1);

            bool duplicate = false;
            for (const auto& existing : out) {
                if (_wcsicmp(existing.c_str(), token.c_str()) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!token.empty() && !duplicate) {
                out.push_back(std::move(token));
            }
        }

        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }
    return out;
}

static std::wstring JoinProcessList(const std::vector<std::wstring>& list) {
    std::wstring joined;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) joined += L",";
        joined += list[i];
    }
    return joined;
}

static std::wstring ReadIniStringDynamic(const wchar_t* section, const wchar_t* key, const wchar_t* defVal, const std::wstring& iniPath) {
    DWORD cap = 512;
    for (;;) {
        std::wstring buf(cap, L'\0');
        DWORD copied = GetPrivateProfileStringW(section, key, defVal, buf.data(), cap, iniPath.c_str());
        if (copied < cap - 2) {
            buf.resize(copied);
            return buf;
        }
        cap *= 2;
        if (cap > 65536) {
            buf.resize(copied);
            return buf;
        }
    }
}

void LoadConfig() {
    g_config.saveFolder = GetDefaultScreenshotsFolder();
    g_config.filePattern = L"BlitSnap_{date}_{time}_{app}";
    g_config.editorPath.clear();
    g_config.autoSave = false;
    g_config.showToast = true;
    g_config.layerMode = true;
    g_config.showTaskbar = true;
    g_config.hiddenProcesses.clear();

    std::wstring ini = GetConfigFilePath();
    if (ini.empty()) return;

    g_config.autoSave = GetPrivateProfileIntW(L"General", L"AutoSave", 0, ini.c_str()) != 0;
    g_config.showToast = GetPrivateProfileIntW(L"General", L"ShowToast", 1, ini.c_str()) != 0;

    g_config.layerMode = GetPrivateProfileIntW(L"General", L"LayerMode", 1, ini.c_str()) != 0;
    g_config.showTaskbar = GetPrivateProfileIntW(L"General", L"ShowTaskbar", 1, ini.c_str()) != 0;
    std::wstring rawHidden = ReadIniStringDynamic(L"General", L"HiddenProcesses", L"", ini);
    g_config.hiddenProcesses = ParseProcessList(rawHidden);

    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(L"General", L"SaveFolder", g_config.saveFolder.c_str(), buf, 1024, ini.c_str());
    g_config.saveFolder = buf;

    GetPrivateProfileStringW(L"General", L"FilePattern", g_config.filePattern.c_str(), buf, 1024, ini.c_str());
    if (buf[0]) g_config.filePattern = buf;

    GetPrivateProfileStringW(L"General", L"EditorPath", L"", buf, 1024, ini.c_str());
    g_config.editorPath = buf;
}

void SaveConfig() {
    std::wstring ini = GetConfigFilePath();
    if (ini.empty()) return;

    WritePrivateProfileStringW(L"General", L"AutoSave", g_config.autoSave ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"ShowToast", g_config.showToast ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"SaveFolder", g_config.saveFolder.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"FilePattern", g_config.filePattern.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"EditorPath", g_config.editorPath.c_str(), ini.c_str());

    WritePrivateProfileStringW(L"General", L"LayerMode", g_config.layerMode ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"ShowTaskbar", g_config.showTaskbar ? L"1" : L"0", ini.c_str());
    std::wstring joinedHidden = JoinProcessList(g_config.hiddenProcesses);
    WritePrivateProfileStringW(L"General", L"HiddenProcesses", joinedHidden.c_str(), ini.c_str());
}


bool IsProcessHiddenInConfig(const std::wstring& exeName) {
    if (exeName.empty()) return false;
    for (const auto& entry : g_config.hiddenProcesses) {
        if (_wcsicmp(entry.c_str(), exeName.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

void SetProcessHiddenInConfig(const std::wstring& exeName, bool hidden) {
    if (exeName.empty()) return;

    auto it = std::remove_if(
        g_config.hiddenProcesses.begin(),
        g_config.hiddenProcesses.end(),
        [&](const std::wstring& item) {
            return _wcsicmp(item.c_str(), exeName.c_str()) == 0;
        });
    g_config.hiddenProcesses.erase(it, g_config.hiddenProcesses.end());

    if (hidden) {
        g_config.hiddenProcesses.push_back(exeName);
    }
    SaveConfig();
}

std::wstring SanitizeFilenameComponent(const std::wstring& raw) {
    std::wstring out;
    out.reserve(raw.size());

    bool prevUnderscore = false;
    for (wchar_t ch : raw) {
        bool illegal = (ch < 0x20) ||
            ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|';
        if (illegal) {
            if (!prevUnderscore && !out.empty()) {
                out.push_back(L'_');
                prevUnderscore = true;
            }
        }
        else {
            out.push_back(ch);
            prevUnderscore = (ch == L'_');
        }
    }

    while (!out.empty() && (out.back() == L' ' || out.back() == L'.' || out.back() == L'_')) {
        out.pop_back();
    }
    if (out.size() > 64) {
        out.resize(64);
    }
    return out.empty() ? L"Desktop" : out;
}

static std::wstring GenerateRandomToken(int count, bool hexMode) {
    count = std::clamp(count, 1, 32);
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    std::mt19937 rng(static_cast<uint32_t>(qpc.LowPart ^ GetTickCount()));

    const wchar_t* alphabet = hexMode ? L"0123456789abcdef" : L"0123456789";
    int maxIdx = hexMode ? 15 : 9;
    std::uniform_int_distribution<int> dist(0, maxIdx);

    std::wstring res;
    res.reserve(count);
    for (int i = 0; i < count; ++i) {
        res.push_back(alphabet[dist(rng)]);
    }
    return res;
}

std::wstring FormatCaptureFilename(const std::wstring& pattern, const std::wstring& activeAppTitle) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t dateBuf[32], timeBuf[32], msBuf[16];
    swprintf_s(dateBuf, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    swprintf_s(timeBuf, L"%02d-%02d-%02d", st.wHour, st.wMinute, st.wSecond);
    swprintf_s(msBuf, L"%03d", st.wMilliseconds);

    std::wstring safeApp = SanitizeFilenameComponent(activeAppTitle);
    std::wstring src = pattern.empty() ? L"BlitSnap_{date}_{time}" : pattern;
    std::wstring result;
    result.reserve(src.size() + 48);

    for (size_t i = 0; i < src.size(); ) {
        if (src[i] == L'{') {
            size_t end = src.find(L'}', i + 1);
            if (end != std::wstring::npos) {
                std::wstring tok = src.substr(i + 1, end - (i + 1));
                if (tok == L"date") {
                    result += dateBuf;
                    i = end + 1;
                    continue;
                }
                if (tok == L"time") {
                    result += timeBuf;
                    i = end + 1;
                    continue;
                }
                if (tok == L"ms") {
                    result += msBuf;
                    i = end + 1;
                    continue;
                }
                if (tok == L"app" || tok == L"title") {
                    result += safeApp;
                    i = end + 1;
                    continue;
                }
                if (tok.rfind(L"rand_hex:", 0) == 0) {
                    int n = _wtoi(tok.c_str() + 9);
                    result += GenerateRandomToken(n > 0 ? n : 6, true);
                    i = end + 1;
                    continue;
                }
                if (tok.rfind(L"rand_num:", 0) == 0) {
                    int n = _wtoi(tok.c_str() + 9);
                    result += GenerateRandomToken(n > 0 ? n : 4, false);
                    i = end + 1;
                    continue;
                }
            }
        }
        result.push_back(src[i]);
        ++i;
    }

    return SanitizeFilenameComponent(result);
}