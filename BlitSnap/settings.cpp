#include "settings.h"
#include "config.h"
#include "theme.h"
#include <shobjidl.h>
#include <windowsx.h>
#include <wchar.h>

constexpr wchar_t kSettingsWndClass[] = L"BlitSnap_Settings_Class";

enum CtrlId {
    ID_CHK_AUTOSAVE = 101,
    ID_EDIT_FOLDER = 102,
    ID_BTN_BROWSE_DIR = 103,
    ID_EDIT_PATTERN = 104,
    ID_LBL_PREVIEW = 105,
    ID_EDIT_EDITOR = 106,
    ID_BTN_BROWSE_EXE = 107,
    ID_CHK_TOAST = 108,
    ID_BTN_SHUTDOWN = 109,
    ID_BTN_SAVE = 110,
    ID_BTN_CANCEL = 111
};

static HWND    g_hSettingsWnd = nullptr;
static HBRUSH  g_hBgBrush = nullptr;
static HBRUSH  g_hInputBrush = nullptr;
static HFONT   g_hUiFont = nullptr;
static HFONT   g_hBoldFont = nullptr;
static bool    g_chkAutoSave = false;
static bool    g_chkShowToast = true;

static LRESULT CALLBACK DarkEditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto origProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    LRESULT res = CallWindowProcW(origProc, hWnd, msg, wParam, lParam);

    if (msg == WM_NCPAINT || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_PAINT) {
        HDC hdc = GetWindowDC(hWnd);
        if (hdc) {
            RECT rc = {};
            GetWindowRect(hWnd, &rc);
            OffsetRect(&rc, -rc.left, -rc.top);

            bool focused = (GetFocus() == hWnd);
            HBRUSH hBorder = CreateSolidBrush(focused ? BlitTheme::kBorderFocus : BlitTheme::kBorder);
            FrameRect(hdc, &rc, hBorder);
            DeleteObject(hBorder);

            InflateRect(&rc, -1, -1);
            HBRUSH hInner = CreateSolidBrush(BlitTheme::kBgInput);
            FrameRect(hdc, &rc, hInner);
            DeleteObject(hInner);

            ReleaseDC(hWnd, hdc);
        }
    }
    return res;
}

static void SubclassDarkEdit(HWND hEdit) {
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DarkEditSubclassProc))
        );
    SetWindowLongPtrW(hEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(orig));
}

static LRESULT CALLBACK HoverButtonSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto origProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_MOUSEMOVE) {
        if ((GetWindowLongPtrW(hWnd, GWLP_USERDATA + 0) != 0)) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        if (GetPropW(hWnd, L"Hover") == nullptr) {
            SetPropW(hWnd, L"Hover", reinterpret_cast<HANDLE>(1));
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }
    else if (msg == WM_MOUSELEAVE) {
        RemovePropW(hWnd, L"Hover");
        InvalidateRect(hWnd, nullptr, FALSE);
    }

    return CallWindowProcW(origProc, hWnd, msg, wParam, lParam);
}

static void SubclassHoverButton(HWND hBtn) {
    WNDPROC orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hBtn, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HoverButtonSubclassProc))
        );
    SetWindowLongPtrW(hBtn, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(orig));
}

static void UpdatePatternPreview(HWND hParent) {
    wchar_t pat[512] = {};
    GetDlgItemTextW(hParent, ID_EDIT_PATTERN, pat, 512);
    std::wstring sample = FormatCaptureFilename(pat, L"Notepad");
    std::wstring line = L"Preview: " + sample + L".png";
    SetDlgItemTextW(hParent, ID_LBL_PREVIEW, line.c_str());
}

static std::wstring BrowseForFolder(HWND hOwner) {
    std::wstring chosen;
    IFileOpenDialog* pDlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg)))) {
        DWORD opts = 0;
        pDlg->GetOptions(&opts);
        pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pDlg->SetTitle(L"Select Screenshot Save Folder");

        if (SUCCEEDED(pDlg->Show(hOwner))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    chosen = path;
                    CoTaskMemFree(path);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
    }
    return chosen;
}

static std::wstring BrowseForExecutable(HWND hOwner) {
    std::wstring chosen;
    IFileOpenDialog* pDlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg)))) {
        COMDLG_FILTERSPEC filters[] = {
            { L"Executables (*.exe)", L"*.exe" },
            { L"All Files (*.*)", L"*.*" }
        };
        pDlg->SetFileTypes(2, filters);
        pDlg->SetTitle(L"Select Image Editor Executable");

        if (SUCCEEDED(pDlg->Show(hOwner))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    chosen = path;
                    CoTaskMemFree(path);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
    }
    return chosen;
}

static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        BlitTheme::EnableDarkTitleBar(hWnd);

        HDC hdc = GetDC(hWnd);
        UINT dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(hWnd, hdc);
        if (dpi == 0) dpi = 96;

        g_hBgBrush = CreateSolidBrush(BlitTheme::kBgWindow);
        g_hInputBrush = CreateSolidBrush(BlitTheme::kBgInput);
        g_hUiFont = BlitTheme::CreateUiFont(dpi, 9, false);
        g_hBoldFont = BlitTheme::CreateUiFont(dpi, 9, true);

        g_chkAutoSave = g_config.autoSave;
        g_chkShowToast = g_config.showToast;

        int pad = BlitTheme::Scale(18, dpi);
        int fullW = BlitTheme::Scale(464, dpi);
        int browseW = BlitTheme::Scale(34, dpi);
        int gap = BlitTheme::Scale(6, dpi);
        int rowH = BlitTheme::Scale(24, dpi);
        int lblH = BlitTheme::Scale(18, dpi);
        int y = pad;

        auto makeCtrl = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, DWORD exStyle,
            int x, int cy, int w, int h, int id, bool bold = false) -> HWND {
                HWND hCtrl = CreateWindowExW(exStyle, cls, txt, WS_CHILD | WS_VISIBLE | style,
                    x, cy, w, h, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    g_app.hInst, nullptr);
                SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_hBoldFont : g_hUiFont), TRUE);
                return hCtrl;
            };

        HWND hChkAuto = makeCtrl(L"BUTTON", L"Auto-save PNG copies to disk (in addition to clipboard)",
            BS_OWNERDRAW, 0, pad, y, fullW, rowH, ID_CHK_AUTOSAVE);
        SubclassHoverButton(hChkAuto);
        y += rowH + BlitTheme::Scale(8, dpi);

        makeCtrl(L"STATIC", L"Save Folder:", 0, 0, pad, y, fullW, lblH, 0, true);
        y += lblH + BlitTheme::Scale(3, dpi);

        HWND hEditDir = makeCtrl(L"EDIT", g_config.saveFolder.c_str(), ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
            pad, y, fullW - browseW - gap, rowH, ID_EDIT_FOLDER);
        SubclassDarkEdit(hEditDir);

        HWND hBtnDir = makeCtrl(L"BUTTON", L"...", BS_OWNERDRAW, 0,
            pad + fullW - browseW, y, browseW, rowH, ID_BTN_BROWSE_DIR);
        SubclassHoverButton(hBtnDir);
        y += rowH + BlitTheme::Scale(14, dpi);

        makeCtrl(L"STATIC", L"Filename Pattern  ({date}, {time}, {ms}, {app}, {rand_hex:6}, {rand_num:4}):",
            0, 0, pad, y, fullW, lblH, 0, true);
        y += lblH + BlitTheme::Scale(3, dpi);

        HWND hEditPat = makeCtrl(L"EDIT", g_config.filePattern.c_str(), ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
            pad, y, fullW, rowH, ID_EDIT_PATTERN);
        SubclassDarkEdit(hEditPat);
        y += rowH + BlitTheme::Scale(4, dpi);

        makeCtrl(L"STATIC", L"Preview:", 0, 0, pad, y, fullW, lblH, ID_LBL_PREVIEW, false);
        UpdatePatternPreview(hWnd);
        y += lblH + BlitTheme::Scale(14, dpi);

        makeCtrl(L"STATIC", L"Custom Editor Executable (leave blank for BlitPaint.exe / mspaint.exe):",
            0, 0, pad, y, fullW, lblH, 0, true);
        y += lblH + BlitTheme::Scale(3, dpi);

        HWND hEditExe = makeCtrl(L"EDIT", g_config.editorPath.c_str(), ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
            pad, y, fullW - browseW - gap, rowH, ID_EDIT_EDITOR);
        SubclassDarkEdit(hEditExe);

        HWND hBtnExe = makeCtrl(L"BUTTON", L"...", BS_OWNERDRAW, 0,
            pad + fullW - browseW, y, browseW, rowH, ID_BTN_BROWSE_EXE);
        SubclassHoverButton(hBtnExe);
        y += rowH + BlitTheme::Scale(14, dpi);

        HWND hChkToast = makeCtrl(L"BUTTON", L"Show Action Center toast preview after capture",
            BS_OWNERDRAW, 0, pad, y, fullW, rowH, ID_CHK_TOAST);
        SubclassHoverButton(hChkToast);
        y += rowH + BlitTheme::Scale(22, dpi);

        int btnH = BlitTheme::Scale(28, dpi);
        int shutdownW = BlitTheme::Scale(136, dpi);
        int actionW = BlitTheme::Scale(84, dpi);

        HWND hBtnShut = makeCtrl(L"BUTTON", L"Shutdown BlitSnap", BS_OWNERDRAW, 0,
            pad, y, shutdownW, btnH, ID_BTN_SHUTDOWN);
        SubclassHoverButton(hBtnShut);

        HWND hBtnSave = makeCtrl(L"BUTTON", L"Save", BS_OWNERDRAW, 0,
            pad + fullW - actionW * 2 - gap, y, actionW, btnH, ID_BTN_SAVE, true);
        SubclassHoverButton(hBtnSave);

        HWND hBtnCancel = makeCtrl(L"BUTTON", L"Cancel", BS_OWNERDRAW, 0,
            pad + fullW - actionW, y, actionW, btnH, ID_BTN_CANCEL);
        SubclassHoverButton(hBtnCancel);

        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc = {};
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_hBgBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        int id = GetDlgCtrlID(hCtrl);
        SetBkColor(hdc, BlitTheme::kBgWindow);
        SetTextColor(hdc, (id == ID_LBL_PREVIEW) ? BlitTheme::kTextMuted : BlitTheme::kTextPrimary);
        return reinterpret_cast<LRESULT>(g_hBgBrush);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, BlitTheme::kBgInput);
        SetTextColor(hdc, BlitTheme::kTextPrimary);
        return reinterpret_cast<LRESULT>(g_hInputBrush);
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;

        HDC hdc = GetDC(hWnd);
        UINT dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(hWnd, hdc);
        if (dpi == 0) dpi = 96;

        if (GetPropW(dis->hwndItem, L"Hover") != nullptr) {
            dis->itemData |= 1;
        }
        else {
            dis->itemData &= ~static_cast<ULONG_PTR>(1);
        }

        switch (dis->CtlID) {
        case ID_CHK_AUTOSAVE:
            BlitTheme::DrawFlatCheckbox(dis, dpi, g_chkAutoSave);
            return TRUE;
        case ID_CHK_TOAST:
            BlitTheme::DrawFlatCheckbox(dis, dpi, g_chkShowToast);
            return TRUE;
        case ID_BTN_SAVE:
            BlitTheme::DrawFlatButton(dis, dpi, true, false);
            return TRUE;
        case ID_BTN_SHUTDOWN:
            BlitTheme::DrawFlatButton(dis, dpi, false, true);
            return TRUE;
        default:
            BlitTheme::DrawFlatButton(dis, dpi, false, false);
            return TRUE;
        }
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == ID_EDIT_PATTERN && code == EN_CHANGE) {
            UpdatePatternPreview(hWnd);
            return 0;
        }

        if (code == BN_CLICKED) {
            switch (id) {
            case ID_CHK_AUTOSAVE:
                g_chkAutoSave = !g_chkAutoSave;
                InvalidateRect(GetDlgItem(hWnd, ID_CHK_AUTOSAVE), nullptr, FALSE);
                return 0;

            case ID_CHK_TOAST:
                g_chkShowToast = !g_chkShowToast;
                InvalidateRect(GetDlgItem(hWnd, ID_CHK_TOAST), nullptr, FALSE);
                return 0;

            case ID_BTN_BROWSE_DIR: {
                std::wstring dir = BrowseForFolder(hWnd);
                if (!dir.empty()) {
                    SetDlgItemTextW(hWnd, ID_EDIT_FOLDER, dir.c_str());
                }
                return 0;
            }

            case ID_BTN_BROWSE_EXE: {
                std::wstring exe = BrowseForExecutable(hWnd);
                if (!exe.empty()) {
                    SetDlgItemTextW(hWnd, ID_EDIT_EDITOR, exe.c_str());
                }
                return 0;
            }

            case ID_BTN_SAVE: {
                wchar_t buf[1024] = {};
                g_config.autoSave = g_chkAutoSave;
                g_config.showToast = g_chkShowToast;

                GetDlgItemTextW(hWnd, ID_EDIT_FOLDER, buf, 1024);
                g_config.saveFolder = buf;

                GetDlgItemTextW(hWnd, ID_EDIT_PATTERN, buf, 1024);
                g_config.filePattern = buf;

                GetDlgItemTextW(hWnd, ID_EDIT_EDITOR, buf, 1024);
                g_config.editorPath = buf;

                SaveConfig();
                DestroyWindow(hWnd);
                return 0;
            }

            case ID_BTN_CANCEL:
                DestroyWindow(hWnd);
                return 0;

            case ID_BTN_SHUTDOWN:
                DestroyWindow(hWnd);
                if (g_app.hMsgWnd) {
                    PostMessageW(g_app.hMsgWnd, WM_CLOSE, 0, 0);
                }
                return 0;
            }
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_hSettingsWnd = nullptr;
        if (g_hBgBrush) { DeleteObject(g_hBgBrush);    g_hBgBrush = nullptr; }
        if (g_hInputBrush) { DeleteObject(g_hInputBrush); g_hInputBrush = nullptr; }
        if (g_hUiFont) { DeleteObject(g_hUiFont);     g_hUiFont = nullptr; }
        if (g_hBoldFont) { DeleteObject(g_hBoldFont);   g_hBoldFont = nullptr; }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RegisterSettingsClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kSettingsWndClass;
    RegisterClassExW(&wc);
}

void ShowSettingsWindow() {
    if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) {
        ShowWindow(g_hSettingsWnd, SW_RESTORE);
        SetForegroundWindow(g_hSettingsWnd);
        return;
    }

    HDC hdc = GetDC(nullptr);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
    ReleaseDC(nullptr, hdc);
    if (dpi == 0) dpi = 96;

    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT rcClient = { 0, 0, BlitTheme::Scale(500, dpi), BlitTheme::Scale(334, dpi) };
    AdjustWindowRectEx(&rcClient, style, FALSE, 0);

    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom - rcClient.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hSettingsWnd = CreateWindowExW(
        0,
        kSettingsWndClass,
        L"BlitSnap Settings",
        style,
        x, y, w, h,
        nullptr, nullptr, g_app.hInst, nullptr
    );

    ShowWindow(g_hSettingsWnd, SW_SHOW);
    UpdateWindow(g_hSettingsWnd);
}