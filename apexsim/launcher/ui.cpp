// See ui.h.

#include "ui.h"

#include <shobjidl.h>

namespace fs = std::filesystem;

static HWND gModeless;  // the open dialog window
static const wchar_t* kPrevDialog = L"ApexPrevDialog";  // the dialog window open under it

HFONT MakeUiFont(int dpi)
{
    NONCLIENTMETRICSW ncm = { sizeof ncm };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

void CenterOn(HWND w, HWND owner)
{
    RECT r, o;
    GetWindowRect(w, &r);
    GetWindowRect(owner, &o);
    int x = o.left + ((o.right - o.left) - (r.right - r.left)) / 2;
    int y = o.top + ((o.bottom - o.top) - (r.bottom - r.top)) / 2;
    SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int dpi)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
                           MulDiv(w, dpi, 96), MulDiv(h, dpi, 96), parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

void SetFontAll(HWND parent, HFONT f)
{
    EnumChildWindows(parent, [](HWND c, LPARAM lp) -> BOOL { SendMessageW(c, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE; }, (LPARAM)f);
}

HWND CreateDialogWindow(HWND owner, const wchar_t* cls, const wchar_t* title, int w, int h, int dpi)
{
    RECT r = { 0, 0, MulDiv(w, dpi, 96), MulDiv(h, dpi, 96) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dpi);
    EnableWindow(owner, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, cls, title, style, 0, 0, r.right - r.left, r.bottom - r.top,
                              owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hw) { EnableWindow(owner, TRUE); return nullptr; }
    CenterOn(hw, owner);
    ShowWindow(hw, SW_SHOW);
    // A dialog opened from another one hands the message loop back to it on close.
    SetPropW(hw, kPrevDialog, gModeless);
    gModeless = hw;
    return hw;
}

void CloseDialogWindow(HWND hw)
{
    HWND owner = GetWindow(hw, GW_OWNER);
    gModeless = (HWND)RemovePropW(hw, kPrevDialog);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(hw);
}

HWND ActiveDialog() { return gModeless; }

static fs::path Pick(HWND owner, const wchar_t* title, bool folders, const wchar_t* filterName, const wchar_t* filterSpec)
{
    static bool com = false;
    if (!com) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); com = true; }
    fs::path result;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
    {
        if (folders)
        {
            DWORD opts = 0;
            d->GetOptions(&opts);
            d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        else
        {
            COMDLG_FILTERSPEC filter[] = { { filterName, filterSpec } };
            d->SetFileTypes(1, filter);
        }
        d->SetTitle(title);
        if (SUCCEEDED(d->Show(owner)))
        {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        d->Release();
    }
    return result;
}

fs::path PickFolder(HWND owner, const wchar_t* title) { return Pick(owner, title, true, nullptr, nullptr); }

fs::path PickFile(HWND owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec)
{
    return Pick(owner, title, false, filterName, filterSpec);
}
