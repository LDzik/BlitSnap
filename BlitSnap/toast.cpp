#include "toast.h"
#include "export.h"
#include "config.h"
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <notificationactivationcallback.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <wchar.h>
#include <string>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "shell32.lib")

using namespace ABI::Windows::UI::Notifications;
using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::Foundation;
using namespace Microsoft::WRL;

constexpr wchar_t kAppUserModelId[] = L"BlitSnap.CaptureTool";

// Unique COM CLSID for Action Center toast activation
// {7B4E9A12-4F31-4C8D-9E2A-61D8F03B4C19}
static const CLSID CLSID_BlitSnapToastActivator =
{ 0x7b4e9a12, 0x4f31, 0x4c8d,{ 0x9e, 0x2a, 0x61, 0xd8, 0xf0, 0x3b, 0x4c, 0x19 } };
constexpr wchar_t kActivatorClsidStr[] = L"{7B4E9A12-4F31-4C8D-9E2A-61D8F03B4C19}";

static DWORD g_comRegCookie = 0;

class BlitSnapToastActivator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, INotificationActivationCallback> {
public:
    IFACEMETHODIMP Activate(
        LPCWSTR /*appUserModelId*/,
        LPCWSTR /*invokedArgs*/,
        const NOTIFICATION_USER_INPUT_DATA* /*data*/,
        ULONG /*count*/) override
    {
        if (g_app.hMsgWnd) {
            PostMessageW(g_app.hMsgWnd, WM_APP_LAUNCH_EDITOR, 0, 0);
        }
        else {
            LaunchEditorForLatestCapture();
        }
        return S_OK;
    }
};

class BlitSnapToastClassFactory
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        auto obj = Make<BlitSnapToastActivator>();
        if (!obj) return E_OUTOFMEMORY;
        return obj.CopyTo(riid, ppvObject);
    }

    IFACEMETHODIMP LockServer(BOOL /*fLock*/) override {
        return S_OK;
    }
};

static void RegisterComLocalServerInHkcu(const wchar_t* exePath) {
    wchar_t subkey[256] = {};
    swprintf_s(subkey, L"Software\\Classes\\CLSID\\%s\\LocalServer32", kActivatorClsidStr);

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        wchar_t cmd[MAX_PATH + 32] = {};
        swprintf_s(cmd, L"\"%s\" -ToastActivated", exePath);
        DWORD bytes = static_cast<DWORD>((wcslen(cmd) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd), bytes);
        RegCloseKey(hKey);
    }
}

static void EnsureStartMenuShortcut(const wchar_t* exePath) {
    PWSTR programsDir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programsDir)) || !programsDir) {
        return;
    }

    wchar_t lnkPath[MAX_PATH] = {};
    swprintf_s(lnkPath, L"%s\\BlitSnap.lnk", programsDir);
    CoTaskMemFree(programsDir);

    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink)))) {
        return;
    }

    shellLink->SetPath(exePath);
    shellLink->SetArguments(L"");
    shellLink->SetDescription(L"BlitSnap Lightweight Screen Capture");

    ComPtr<IPropertyStore> propStore;
    if (SUCCEEDED(shellLink.As(&propStore))) {
        PROPVARIANT pvAumid;
        PropVariantInit(&pvAumid);
        pvAumid.vt = VT_LPWSTR;
        pvAumid.pwszVal = const_cast<LPWSTR>(kAppUserModelId);
        propStore->SetValue(PKEY_AppUserModel_ID, pvAumid);

        PROPVARIANT pvClsid;
        PropVariantInit(&pvClsid);
        pvClsid.vt = VT_CLSID;
        pvClsid.puuid = const_cast<CLSID*>(&CLSID_BlitSnapToastActivator);
        propStore->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, pvClsid);

        propStore->Commit();
    }

    ComPtr<IPersistFile> persistFile;
    if (SUCCEEDED(shellLink.As(&persistFile))) {
        persistFile->Save(lnkPath, TRUE);
    }
}

void InitToastSubsystem() {
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    RoInitialize(RO_INIT_SINGLETHREADED);

    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        RegisterComLocalServerInHkcu(exePath);
        EnsureStartMenuShortcut(exePath);
    }

    auto factory = Make<BlitSnapToastClassFactory>();
    if (factory) {
        CoRegisterClassObject(
            CLSID_BlitSnapToastActivator,
            factory.Get(),
            CLSCTX_LOCAL_SERVER,
            REGCLS_MULTIPLEUSE,
            &g_comRegCookie
        );
    }
}

void ShutdownToastSubsystem() {
    if (g_comRegCookie != 0) {
        CoRevokeClassObject(g_comRegCookie);
        g_comRegCookie = 0;
    }
    RoUninitialize();
}

static std::wstring BuildXmlSafeFileUri(const wchar_t* filePath) {
    std::wstring uri = L"file:///";
    for (const wchar_t* p = filePath; *p; ++p) {
        wchar_t ch = *p;
        if (ch == L'\\') {
            uri += L'/';
        }
        else if (ch == L'&') {
            uri += L"&amp;";
        }
        else if (ch == L'<') {
            uri += L"&lt;";
        }
        else if (ch == L'>') {
            uri += L"&gt;";
        }
        else if (ch == L'"') {
            uri += L"&quot;";
        }
        else if (ch == L'\'') {
            uri += L"&apos;";
        }
        else {
            uri += ch;
        }
    }
    return uri;
}

void ShowCaptureToast(const wchar_t* pngPath) {
    if (!g_config.showToast || !pngPath || !pngPath[0]) return;

    std::wstring imageUri = BuildXmlSafeFileUri(pngPath);
    std::wstring xmlPayload =
        L"<toast duration=\"short\">"
        L"  <visual>"
        L"    <binding template=\"ToastGeneric\">"
        L"      <text>BlitSnap</text>"
        L"      <text>Screenshot copied to clipboard. Click to edit.</text>"
        L"      <image placement=\"inline\" src=\"" + imageUri + L"\"/>"
        L"    </binding>"
        L"  </visual>"
        L"</toast>";

    HSTRING_HEADER xmlClassHdr = {};
    HSTRING hXmlClass = nullptr;
    const wchar_t kXmlDocClass[] = L"Windows.Data.Xml.Dom.XmlDocument";
    if (FAILED(WindowsCreateStringReference(kXmlDocClass, ARRAYSIZE(kXmlDocClass) - 1, &xmlClassHdr, &hXmlClass))) {
        return;
    }

    ComPtr<IXmlDocument> xmlDoc;
    if (FAILED(RoActivateInstance(hXmlClass, reinterpret_cast<IInspectable**>(xmlDoc.GetAddressOf())))) {
        return;
    }

    ComPtr<IXmlDocumentIO> xmlDocIO;
    if (FAILED(xmlDoc.As(&xmlDocIO))) return;

    HSTRING_HEADER payloadHdr = {};
    HSTRING hPayload = nullptr;
    if (FAILED(WindowsCreateStringReference(xmlPayload.c_str(), static_cast<UINT32>(xmlPayload.size()), &payloadHdr, &hPayload))) {
        return;
    }

    if (FAILED(xmlDocIO->LoadXml(hPayload))) return;

    HSTRING_HEADER mgrClassHdr = {};
    HSTRING hMgrClass = nullptr;
    const wchar_t kToastMgrClass[] = L"Windows.UI.Notifications.ToastNotificationManager";
    if (FAILED(WindowsCreateStringReference(kToastMgrClass, ARRAYSIZE(kToastMgrClass) - 1, &mgrClassHdr, &hMgrClass))) {
        return;
    }

    ComPtr<IToastNotificationManagerStatics> toastMgr;
    if (FAILED(RoGetActivationFactory(hMgrClass, IID_PPV_ARGS(&toastMgr)))) return;

    HSTRING_HEADER aumidHdr = {};
    HSTRING hAumid = nullptr;
    if (FAILED(WindowsCreateStringReference(kAppUserModelId, ARRAYSIZE(kAppUserModelId) - 1, &aumidHdr, &hAumid))) {
        return;
    }

    ComPtr<IToastNotifier> notifier;
    if (FAILED(toastMgr->CreateToastNotifierWithId(hAumid, &notifier))) return;

    HSTRING_HEADER toastClassHdr = {};
    HSTRING hToastClass = nullptr;
    const wchar_t kToastClass[] = L"Windows.UI.Notifications.ToastNotification";
    if (FAILED(WindowsCreateStringReference(kToastClass, ARRAYSIZE(kToastClass) - 1, &toastClassHdr, &hToastClass))) {
        return;
    }

    ComPtr<IToastNotificationFactory> toastFactory;
    if (FAILED(RoGetActivationFactory(hToastClass, IID_PPV_ARGS(&toastFactory)))) return;

    ComPtr<IToastNotification> toast;
    if (FAILED(toastFactory->CreateToastNotification(xmlDoc.Get(), &toast))) return;

    notifier->Show(toast.Get());
}

void LaunchEditorForLatestCapture() {
    static ULONGLONG s_lastLaunchTick = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastLaunchTick < 1000) return;
    s_lastLaunchTick = now;

    wchar_t pngPath[MAX_PATH] = {};
    if (!GetLatestCapturePath(pngPath, MAX_PATH)) return;

    wchar_t editorExe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, editorExe, MAX_PATH) > 0) {
        wchar_t* lastSlash = wcsrchr(editorExe, L'\\');
        if (lastSlash) {
            wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - editorExe), L"BlitPaint.exe");
        }
    }

    wchar_t cmdLine[MAX_PATH * 2 + 32] = {};
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    if (!g_config.editorPath.empty() && GetFileAttributesW(g_config.editorPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        swprintf_s(cmdLine, L"\"%s\" \"%s\"", g_config.editorPath.c_str(), pngPath);
        if (CreateProcessW(g_config.editorPath.c_str(), cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }

    if (editorExe[0] && GetFileAttributesW(editorExe) != INVALID_FILE_ATTRIBUTES) {
        swprintf_s(cmdLine, L"\"%s\" --from-snap \"%s\"", editorExe, pngPath);
        if (CreateProcessW(editorExe, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }

    wchar_t mspaintPath[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"mspaint.exe", nullptr, MAX_PATH, mspaintPath, nullptr) > 0) {
        swprintf_s(cmdLine, L"\"%s\" \"%s\"", mspaintPath, pngPath);
        if (CreateProcessW(mspaintPath, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
}