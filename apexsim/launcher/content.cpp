// See content.h.

#include "content.h"
#include "ac_import.h"
#include "process.h"
#include "ui.h"

#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>

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

// The JSON string at the start of s (just past its opening quote), unescaped; pos
// ends past the closing quote. Enough for what the server's installer prints.
static std::string JsonStringAt(const std::string& s, size_t& pos)
{
    std::string v;
    while (pos < s.size() && s[pos] != '"')
    {
        char c = s[pos++];
        if (c != '\\' || pos >= s.size()) { v += c; continue; }
        char e = s[pos++];
        switch (e)
        {
        case 'n': v += '\n'; break;
        case 't': v += '\t'; break;
        case 'r': break;
        case 'u':
            // Only control characters are escaped this way; they are dropped.
            pos = std::min(pos + 4, s.size());
            break;
        default: v += e; break;
        }
    }
    ++pos;
    return v;
}

// The strings of "key" in json: the value when it is a string, or each string of an array.
static std::vector<std::string> JsonStrings(const std::string& json, const char* key)
{
    std::vector<std::string> out;
    size_t k = json.find("\"" + std::string(key) + "\":");
    if (k == std::string::npos) return out;
    size_t pos = json.find_first_not_of(" \t\r\n:", k + strlen(key) + 2);
    if (pos == std::string::npos) return out;
    if (json[pos] == '"')
    {
        ++pos;
        out.push_back(JsonStringAt(json, pos));
        return out;
    }
    if (json[pos] != '[') return out;
    for (++pos; pos < json.size() && json[pos] != ']'; ++pos)
        if (json[pos] == '"') { ++pos; out.push_back(JsonStringAt(json, pos)); --pos; }
    return out;
}

// Runs the server's content installer with args; out is what it printed, and the
// result whether it ran and reported no error ("apexsim-server: <why>" on failure).
static bool RunInstaller(const Env& env, const std::wstring& args, std::wstring& out)
{
    std::wstring line = L"\"" + env.serverExe.wstring() + L"\" content install " + args;
    std::string text;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ran = RunCapture(line, text, 300000);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    out = ran ? Widen(Trim(text)) : L"the server's installer did not start";
    return ran && out.find(L"apexsim-server:") == std::wstring::npos;
}

// A loose pack (.acp): the server's installer says what the pack holds (--dry-run),
// the player confirms, and its cars, tracks and HUD components go into custom/ on
// both sides, in a release or a checkout (CMDLINE.md, "content").
static void InstallLoosePack(HWND hw, const fs::path& pack)
{
    const Env& env = *gEnv;
    const wchar_t* title = L"Install a content pack";
    if (!Exists(env.serverExe))
    {
        std::wstring m = env.release
            ? L"The server (" + env.serverExe.wstring() + L") is missing, and it installs the pack."
            : L"Packs install with the server's installer: build the server first (cargo build --release in server\\server).";
        MessageBoxW(hw, m.c_str(), title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring args = L"\"" + pack.wstring() + L"\"";
    if (!env.release)
    {
        std::wstring code = env.root.wstring();
        while (code.size() > 3 && (code.back() == L'\\' || code.back() == L'/')) code.pop_back();  // a trailing \ would escape the quote
        args += L" --checkout \"" + code + L"\"";
    }
    std::wstring out;
    if (!RunInstaller(env, args + L" --dry-run", out))
    {
        MessageBoxW(hw, (L"Could not install " + pack.filename().wstring() + L":\n\n" + out).c_str(), title, MB_OK | MB_ICONWARNING);
        return;
    }
    std::string json = Narrow(out);
    std::vector<std::string> summary = JsonStrings(json, "summary"), warnings = JsonStrings(json, "warnings");
    std::wstring m = pack.filename().wstring() + L" holds:\n\n" + (summary.empty() ? L"" : Widen(summary[0])) + L"\n";
    if (!warnings.empty())
    {
        m += L"\nNote:\n";
        for (const std::string& w : warnings) m += L"  " + Widen(w) + L"\n";
    }
    m += L"\nInstall it?";
    if (MessageBoxW(hw, m.c_str(), title, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    bool ok = RunInstaller(env, args, out);
    m = ok ? L"Installed " + pack.filename().wstring() + L". Restart the game and the server to use it."
           : L"Could not install " + pack.filename().wstring() + L":\n\n" + out;
    MessageBoxW(hw, m.c_str(), title, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    PopulateList(hw);
}

// Install a pack with the server's own installer (apexsim-server content install,
// Server\CMDLINE: it checks the pack whole before it writes), and say what it did.
// A release's .apxpack goes to default/, a loose .acp to custom/ (InstallLoosePack).
static void InstallPack(HWND hw)
{
    const Env& env = *gEnv;
    fs::path pack = PickFile(hw, L"Install a content pack", L"ApexSim content pack (.acp, .apxpack)", L"*.acp;*.apxpack");
    if (pack.empty()) return;
    if (EndsWithI(pack.wstring(), L".acp")) { InstallLoosePack(hw, pack); return; }
    if (!env.release || !Exists(env.serverExe))
    {
        MessageBoxW(hw, L"An .apxpack installs into a release package (its Server and Game folders).",
                    L"Install a content pack", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring out;
    bool ok = RunInstaller(env, L"\"" + pack.wstring() + L"\"", out) && out.find(L"\"version\"") != std::wstring::npos;
    std::wstring m = ok ? L"Installed " + pack.filename().wstring() + L". Restart the game and the server to use it."
                        : L"Could not install " + pack.filename().wstring() + L":\n\n" + out;
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
