// See content.h.

#include "content.h"
#include "ac_import.h"
#include "process.h"
#include "ui.h"

#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>

namespace fs = std::filesystem;

// ------------------------------------------------------------------ listing

static void AddDirs(std::vector<Item>& v, const fs::path& dir, const wchar_t* origin)
{
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::wstring n = it->path().filename().wstring();
        if (!n.empty() && n[0] != L'_' && n[0] != L'.') v.push_back({ n, origin });
    }
}

static void AddFiles(std::vector<Item>& v, const fs::path& dir, const std::wstring& suffix, const wchar_t* origin)
{
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        std::wstring n = it->path().filename().wstring();
        if (EndsWithI(n, suffix)) v.push_back({ n.substr(0, n.size() - suffix.size()), origin });
    }
}

std::vector<Item> ListContent(const Env& e, int tab)
{
    std::vector<Item> v;
    switch (tab)
    {
    case TabTracks:
        if (e.tracksAreExports) AddFiles(v, e.tracksDefault, L".uescene.json", L"installed");
        else { AddDirs(v, e.tracksDefault, L"default"); AddDirs(v, e.tracksCustom, L"custom"); }  // a track is a folder
        break;
    case TabCars: AddDirs(v, e.carsDefault, L"default"); AddDirs(v, e.carsCustom, L"custom"); break;
    case TabHud: AddDirs(v, e.hudDefault, L"default"); AddDirs(v, e.hudCustom, L"custom"); break;
    case TabWheels: AddFiles(v, e.wheels, L".glb", L"installed"); break;
    }
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return v;
}

fs::path TabFolder(const Env& e, int tab)
{
    switch (tab)
    {
    case TabTracks: return e.tracksAreExports ? e.tracksDefault : e.tracksCustom;
    case TabCars: return e.carsCustom;
    case TabHud: return e.hudCustom;
    default: return e.wheels;
    }
}

// ------------------------------------------------------------------- window

enum { IdTab = 100, IdList, IdInfo, IdImport, IdOpen, IdPack };

const wchar_t* kContentWindowClass = L"ApexContent";

static const Env* gEnv;  // what the open window lists
static HFONT gContentFont;
static HANDLE gImport;  // a running import, so the list refreshes when it ends

static int S(HWND hw, int v) { return MulDiv(v, (int)GetDpiForWindow(hw), 96); }

static void PopulateList(HWND hw)
{
    HWND tab = GetDlgItem(hw, IdTab), list = GetDlgItem(hw, IdList);
    int t = TabCtrl_GetCurSel(tab);
    std::vector<Item> items = ListContent(*gEnv, t);
    ListView_DeleteAllItems(list);
    int def = 0, cust = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        LVITEMW li = {};
        li.mask = LVIF_TEXT;
        li.iItem = (int)i;
        li.pszText = const_cast<wchar_t*>(items[i].name.c_str());
        ListView_InsertItem(list, &li);
        ListView_SetItemText(list, (int)i, 1, const_cast<wchar_t*>(items[i].origin.c_str()));
        (items[i].origin == L"custom" ? cust : def)++;
    }
    static const wchar_t* names[] = { L"tracks", L"cars", L"HUD components", L"wheels" };
    std::wstring info = std::to_wstring(items.size()) + L" " + names[t];
    if (cust) info += L"  (" + std::to_wstring(cust) + L" custom)";
    if (items.empty()) info += L" - nothing found in " + TabFolder(*gEnv, t).wstring();
    SetWindowTextW(GetDlgItem(hw, IdInfo), info.c_str());
    EnableWindow(GetDlgItem(hw, IdImport), t == TabTracks || t == TabCars);
}

// Install a pack with the server's own installer (apexsim-server content install,
// Server\CMDLINE: it checks the pack whole before it writes), and say what it did.
static void InstallPack(HWND hw)
{
    const Env& env = *gEnv;
    if (!env.release || !Exists(env.serverExe))
    {
        MessageBoxW(hw, L"Content packs install into a release package (its Server and Game folders).",
                    L"Install a content pack", MB_OK | MB_ICONINFORMATION);
        return;
    }
    fs::path pack = PickFile(hw, L"Install a content pack", L"ApexSim content pack", L"*.apxpack");
    if (pack.empty()) return;
    std::wstring line = L"\"" + env.serverExe.wstring() + L"\" content install \"" + pack.wstring() + L"\"";
    std::string out;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ran = RunCapture(line, out, 120000);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    std::wstring text = Widen(Trim(out));
    bool ok = ran && text.find(L"apexsim-server:") == std::wstring::npos && text.find(L"\"version\"") != std::wstring::npos;
    std::wstring m = ok ? L"Installed " + pack.filename().wstring() + L". Restart the game and the server to use it."
                        : L"Could not install " + pack.filename().wstring() + L":\n\n" + (ran ? text : L"the server's installer did not start");
    MessageBoxW(hw, m.c_str(), L"Install a content pack", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    PopulateList(hw);
}

static void StartImport(HWND hw)
{
    if (gImport) { MessageBoxW(hw, L"An import is already running.", L"Import", MB_OK | MB_ICONINFORMATION); return; }
    int tab = TabCtrl_GetCurSel(GetDlgItem(hw, IdTab));
    gImport = StartAcImport(hw, *gEnv, tab == TabCars);
    if (!gImport) return;
    SetWindowTextW(GetDlgItem(hw, IdInfo), L"Importing... see the console window. This list refreshes when it closes.");
    SetTimer(hw, 1, 500, nullptr);
}

static void LayoutContent(HWND hw)
{
    RECT c;
    GetClientRect(hw, &c);
    int m = S(hw, 10);
    HWND tab = GetDlgItem(hw, IdTab);
    SetWindowPos(tab, nullptr, m, m, c.right - 2 * m, c.bottom - S(hw, 100), SWP_NOZORDER);
    RECT r = { 0, 0, c.right - 2 * m, c.bottom - S(hw, 100) };
    TabCtrl_AdjustRect(tab, FALSE, &r);
    HWND list = GetDlgItem(hw, IdList);
    // The list sits over the tab control's page, so keep it above it in z-order.
    SetWindowPos(list, HWND_TOP, m + r.left, m + r.top, r.right - r.left, r.bottom - r.top, 0);
    int w = r.right - r.left - GetSystemMetrics(SM_CXVSCROLL) - S(hw, 4);
    ListView_SetColumnWidth(list, 0, w * 7 / 10);
    ListView_SetColumnWidth(list, 1, w * 3 / 10);
    SetWindowPos(GetDlgItem(hw, IdInfo), nullptr, m, c.bottom - S(hw, 84), c.right - 2 * m, S(hw, 36), SWP_NOZORDER);
    int by = c.bottom - S(hw, 44), bh = S(hw, 32);
    SetWindowPos(GetDlgItem(hw, IdImport), nullptr, m, by, S(hw, 190), bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IdOpen), nullptr, m + S(hw, 200), by, S(hw, 120), bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IdPack), nullptr, m + S(hw, 330), by, S(hw, 150), bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IDCANCEL), nullptr, c.right - m - S(hw, 100), by, S(hw, 100), bh, SWP_NOZORDER);
}

LRESULT CALLBACK ContentProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        int dpi = (int)GetDpiForWindow(hw);
        gContentFont = MakeUiFont(dpi);
        HWND tab = Ctl(hw, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 10, 10, IdTab, dpi);
        const wchar_t* names[] = { L"Tracks", L"Cars", L"HUD", L"Wheels" };
        for (int i = 0; i < TabCount; ++i)
        {
            TCITEMW ti = {};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<wchar_t*>(names[i]);
            TabCtrl_InsertItem(tab, i, &ti);
        }
        HWND list = Ctl(hw, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP | WS_CLIPSIBLINGS,
                        0, 0, 10, 10, IdList, dpi);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 100;
        col.pszText = const_cast<wchar_t*>(L"Name");
        ListView_InsertColumn(list, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"Source");
        ListView_InsertColumn(list, 1, &col);
        Ctl(hw, L"STATIC", L"", 0, 0, 0, 10, 10, IdInfo, dpi);
        Ctl(hw, L"BUTTON", L"Import from Assetto Corsa...", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, IdImport, dpi);
        Ctl(hw, L"BUTTON", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, IdOpen, dpi);
        Ctl(hw, L"BUTTON", L"Install pack...", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, IdPack, dpi);
        Ctl(hw, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, IDCANCEL, dpi);
        SetFontAll(hw, gContentFont);
        LayoutContent(hw);
        PopulateList(hw);
        return 0;
    }
    case WM_NOTIFY:
    {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == IdTab && nm->code == TCN_SELCHANGE) PopulateList(hw);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDCANCEL: CloseDialogWindow(hw); return 0;
        case IdImport: StartImport(hw); return 0;
        case IdPack: InstallPack(hw); return 0;
        case IdOpen:
        {
            fs::path p = TabFolder(*gEnv, TabCtrl_GetCurSel(GetDlgItem(hw, IdTab)));
            std::error_code ec;
            fs::create_directories(p, ec);
            ShellExecuteW(hw, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;
    case WM_TIMER:
        if (gImport && WaitForSingleObject(gImport, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(gImport);
            gImport = nullptr;
            KillTimer(hw, 1);
            PopulateList(hw);
            std::wstring t = GetText(GetDlgItem(hw, IdInfo));
            SetWindowTextW(GetDlgItem(hw, IdInfo), (t + L"\nImport finished. Restart the game to see it.").c_str());
        }
        return 0;
    case WM_CLOSE: CloseDialogWindow(hw); return 0;
    case WM_NCDESTROY:
        if (gContentFont) { DeleteObject(gContentFont); gContentFont = nullptr; }
        break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

void OpenContentWindow(HWND owner, const Env& env, int dpi)
{
    gEnv = &env;
    CreateDialogWindow(owner, kContentWindowClass, L"Manage content", 640, 500, dpi);
}
