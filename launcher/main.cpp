// ApexSim launcher ("launcher.exe"). Plain Win32 + GDI+, no runtime beyond the CRT
// (linked statically), so it appears in a few tens of milliseconds.
//
// Flow (see README.md): splash with progress bar -> environment / config /
// content checks on a worker thread -> buttons enable.
//
// Build: bootstrap\build.bat  ->  bootstrap\out\launcher.exe

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "uuid.lib")

namespace fs = std::filesystem;

static const wchar_t* kTroubleshootUrl = L"https://gdoct.github.io/apexsim";
static const unsigned short kServerPort = 9000;  // Play.bat's check

// ---------------------------------------------------------------- text utils

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

static bool EndsWithI(const std::wstring& s, const std::wstring& suffix)
{
    return s.size() >= suffix.size() &&
           _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

// -------------------------------------------------------------- installation

struct Env
{
    bool found = false;     // a game install or a source checkout was located
    bool release = false;   // packaged layout (Game\, Server\) vs. the repo
    fs::path root;
    fs::path gameExe, playScript, serverExe, serverDir, settings, scripts;
    fs::path carsDefault, carsCustom, wheels, hudDefault, hudCustom, tracksDefault, tracksCustom;
    bool tracksAreExports = false;  // packaged tracks are .uescene.json, the repo's are .yaml
};

static bool Exists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

// Walk up from the launcher's own folder: it may sit at the root of a release
// folder (beside Game\ and Server\), inside Game\, or in the repo's bootstrap\.
static Env Locate()
{
    Env e;
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    fs::path d = fs::path(buf).parent_path();
    e.root = d;

    for (int i = 0; i < 5; ++i)
    {
        if (Exists(d / L"Game" / L"ApexSim.exe"))
        {
            e.found = e.release = true;
            e.root = d;
            fs::path game = d / L"Game";
            e.gameExe = game / L"ApexSim.exe";
            e.serverExe = d / L"Server" / L"apexsim-server.exe";
            e.serverDir = d / L"Server";
            e.settings = game / L"settings.yml";
            e.carsDefault = game / L"Cars" / L"default";
            e.carsCustom = game / L"Cars" / L"custom";
            e.wheels = game / L"Wheels";
            e.hudDefault = game / L"Hud" / L"default";
            e.hudCustom = game / L"Hud" / L"custom";
            e.tracksDefault = game / L"Tracks";
            e.tracksAreExports = true;
            break;
        }
        if (Exists(d / L"content" / L"cars") && Exists(d / L"server"))
        {
            e.found = true;
            e.root = d;
            // The repo launches through play_editor.ps1 (builds, then runs the editor
            // build with -game); there is no packaged ApexSim.exe to start.
            e.playScript = d / L"scripts" / L"play_editor.ps1";
            e.serverExe = d / L"server" / L"target" / L"release" / L"apexsim-server.exe";
            e.serverDir = d;  // server.toml and content/ resolve from here
            e.settings = d / L"game-unreal" / L"settings.yml";
            e.scripts = d / L"scripts";
            e.carsDefault = d / L"content" / L"cars" / L"default";
            e.carsCustom = d / L"content" / L"cars" / L"custom";
            e.wheels = d / L"content" / L"wheels";
            e.hudDefault = d / L"content" / L"hud" / L"default";
            e.hudCustom = d / L"content" / L"hud" / L"custom";
            e.tracksDefault = d / L"content" / L"tracks" / L"default";
            e.tracksCustom = d / L"content" / L"tracks" / L"custom";
            break;
        }
        if (!d.has_parent_path() || d.parent_path() == d) break;
        d = d.parent_path();
    }
    return e;
}

// ------------------------------------------------------------------ settings
// The same flat two-level YAML subset UApexBootSettingsSubsystem reads, written
// with the same text as ApexBootSettingsIo::Serialise. Only the keys the
// launcher edits (network / graphics) exist; the game rewrites the file whole
// too, so nothing else is lost.

struct Settings
{
    int resX = 1920, resY = 1080;
    std::string mode = "borderless";  // fullscreen | borderless | windowed
    bool vsync = false;
    int frameLimit = 0;
    int screens = 1;
    std::string host = "127.0.0.1";
    int port = kServerPort;
};

static Settings DefaultSettings()
{
    Settings s;
    // Seeded from the desktop, as the game does on a first run: a fixed
    // 1920x1080 is wrong on most monitors.
    s.resX = GetSystemMetrics(SM_CXSCREEN);
    s.resY = GetSystemMetrics(SM_CYSCREEN);
    return s;
}

static bool LoadSettings(const fs::path& path, Settings& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        bool indented = line[0] == ' ' || line[0] == '\t';
        if (!indented && t.back() == ':')
        {
            section = t.substr(0, t.size() - 1);
            continue;
        }
        size_t c = t.find(':');
        if (c == std::string::npos) continue;
        std::string key = Trim(t.substr(0, c)), val = Trim(t.substr(c + 1));
        if (section == "display")
        {
            int x, y;
            if (key == "resolution" && sscanf_s(val.c_str(), "%dx%d", &x, &y) == 2) { out.resX = x; out.resY = y; }
            else if (key == "window_mode") out.mode = val;
            else if (key == "vsync") out.vsync = (val == "true");
            else if (key == "frame_limit") out.frameLimit = atoi(val.c_str());
            else if (key == "screens") out.screens = atoi(val.c_str()) == 3 ? 3 : 1;
        }
        else if (section == "server")
        {
            if (key == "host" && !val.empty()) out.host = val;
            else if (key == "port") out.port = atoi(val.c_str());
        }
    }
    return true;
}

static bool SaveSettings(const fs::path& path, const Settings& s)
{
    char buf[2048];
    snprintf(buf, sizeof buf,
        "# ApexSim settings.\n"
        "#\n"
        "# Edit this file to change how the game starts up. It is rewritten whenever\n"
        "# these settings are changed from inside the game, so comments of your own\n"
        "# will not survive that. Delete the file to go back to the defaults.\n"
        "\n"
        "display:\n"
        "  # WIDTHxHEIGHT, e.g. 2560x1440. Ignored in borderless, which always\n"
        "  # takes the size of the monitor it opens on.\n"
        "  resolution: %dx%d\n"
        "  # fullscreen | borderless | windowed\n"
        "  window_mode: %s\n"
        "  vsync: %s\n"
        "  # Frames per second, or 0 for uncapped.\n"
        "  frame_limit: %d\n"
        "  # 1 for one monitor, or 3 for a triple-monitor rig: the window then spans\n"
        "  # the three monitors standing in a row and each shows its own view. The\n"
        "  # rig's measurements are under Settings > Graphics > Screens.\n"
        "  screens: %d\n"
        "\n"
        "server:\n"
        "  # The server the game connects to when it starts. 127.0.0.1 is a server\n"
        "  # on this machine, such as the one Play.bat starts for you.\n"
        "  host: %s\n"
        "  port: %d\n",
        s.resX, s.resY, s.mode.c_str(), s.vsync ? "true" : "false", s.frameLimit, s.screens,
        s.host.c_str(), s.port);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << buf;
    return (bool)f;
}

// ------------------------------------------------------------------- content

struct Item { std::wstring name, origin; };

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

enum Tab { TabTracks, TabCars, TabHud, TabWheels, TabCount };

static std::vector<Item> ListContent(const Env& e, int tab)
{
    std::vector<Item> v;
    switch (tab)
    {
    case TabTracks:
        if (e.tracksAreExports) AddFiles(v, e.tracksDefault, L".uescene.json", L"installed");
        else { AddFiles(v, e.tracksDefault, L".yaml", L"default"); AddFiles(v, e.tracksCustom, L".yaml", L"custom"); }
        break;
    case TabCars: AddDirs(v, e.carsDefault, L"default"); AddDirs(v, e.carsCustom, L"custom"); break;
    case TabHud: AddDirs(v, e.hudDefault, L"default"); AddDirs(v, e.hudCustom, L"custom"); break;
    case TabWheels: AddFiles(v, e.wheels, L".glb", L"installed"); break;
    }
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return v;
}

// Where "Open folder" goes: the folder a player drops their own content into.
static fs::path TabFolder(const Env& e, int tab)
{
    switch (tab)
    {
    case TabTracks: return e.tracksAreExports ? e.tracksDefault : e.tracksCustom;
    case TabCars: return e.carsCustom;
    case TabHud: return e.hudCustom;
    default: return e.wheels;
    }
}

// ----------------------------------------------------------------- app state

enum
{
    WM_APP_STATUS = WM_APP + 1,  // wParam = progress in 1/1000, lParam = new std::wstring*
    WM_APP_DONE = WM_APP + 2,    // worker finished; buttons may enable
};

enum { BtnTrouble, BtnLaunch, BtnArrow, BtnConfig, BtnContent, BtnClose, BtnCount };

struct App
{
    Env env;
    Settings settings;
    HWND hwnd = nullptr;
    HWND modeless = nullptr;  // secondary window that wants IsDialogMessage
    int dpi = 96;
    std::wstring status = L"Starting...", note;
    float target = 0, shown = 0;
    bool ready = false, busy = false, failed = false;
    bool gameOk = false, serverOk = false;
    int hover = -1, down = -1;
    RECT btn[BtnCount] = {};
} g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }

static HFONT MakeUiFont(int dpi)
{
    NONCLIENTMETRICSW ncm = { sizeof ncm };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

// ------------------------------------------------------------------- worker

static void Post(HWND h, const wchar_t* text, int permille)
{
    PostMessageW(h, WM_APP_STATUS, (WPARAM)permille, (LPARAM) new std::wstring(text));
}

static void WorkerMain(HWND h)
{
    Post(h, L"Checking environment...", 120);
    g.env = Locate();
    g.gameOk = g.env.found && Exists(g.env.release ? g.env.gameExe : g.env.playScript);
    g.serverOk = g.env.found && Exists(g.env.serverExe);

    Post(h, L"Generating config files...", 450);
    g.settings = DefaultSettings();
    if (g.env.found)
    {
        if (!LoadSettings(g.env.settings, g.settings))
        {
            // First run: seed from the desktop. A write that fails (read-only
            // folder) is not fatal; the game falls back to its own defaults.
            SaveSettings(g.env.settings, g.settings);
        }
    }

    Post(h, L"Checking content...", 750);
    int cars = 0, tracks = 0;
    if (g.env.found)
    {
        cars = (int)ListContent(g.env, TabCars).size();
        tracks = (int)ListContent(g.env, TabTracks).size();
    }

    g.failed = true;
    const wchar_t* final = L"Ready";
    if (!g.env.found)
    {
        final = L"No ApexSim installation found.";
        g.note = L"Put Game.exe next to the Game folder (or in the source checkout) and start it again.";
    }
    else if (!g.gameOk)
    {
        final = g.env.release ? L"ApexSim.exe was not found." : L"scripts\\play_editor.ps1 was not found.";
        g.note = L"Expected at " + (g.env.release ? g.env.gameExe : g.env.playScript).wstring();
    }
    else if (cars == 0 || tracks == 0)
    {
        final = L"Ready, but content is missing.";
        g.note = L"Found " + std::to_wstring(cars) + L" cars and " + std::to_wstring(tracks) +
                 L" tracks. Use Manage content, or see Troubleshooting.";
        g.failed = false;
    }
    else
    {
        g.note = std::to_wstring(cars) + L" cars  ·  " + std::to_wstring(tracks) + L" tracks";
        g.failed = false;
    }
    Post(h, final, 1000);
    PostMessageW(h, WM_APP_DONE, 0, 0);
}

// ------------------------------------------------------------------- launch

static bool PortListening(unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    connect(s, (sockaddr*)&a, sizeof a);
    fd_set w, x;
    FD_ZERO(&w); FD_ZERO(&x);
    FD_SET(s, &w); FD_SET(s, &x);
    timeval tv = { 0, 250000 };
    bool ok = select(0, nullptr, &w, &x, &tv) > 0 && FD_ISSET(s, &w);
    closesocket(s);
    return ok;
}

static bool Spawn(const fs::path& exe, const fs::path& cwd, DWORD flags, bool minimized, const wchar_t* title)
{
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si = { sizeof si };
    if (minimized) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWMINNOACTIVE; }
    if (title) si.lpTitle = const_cast<wchar_t*>(title);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, flags, nullptr, cwd.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Source checkout: the same as `scripts/play_editor.ps1 -Build`, minus its server
// (-NoServer; "Launch with local server" is the launcher's own). The build and
// the script run in a console of their own, kept open if the script fails.
static bool SpawnEditorBuild()
{
    std::wstring cmd = L"cmd.exe /S /C \"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
                       g.env.playScript.wstring() + L"\" -Build -NoServer || pause\"";
    STARTUPINFOW si = { sizeof si };
    si.lpTitle = const_cast<wchar_t*>(L"ApexSim build");
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, g.env.root.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void LaunchAsync(bool withServer)
{
    g.busy = true;
    InvalidateRect(g.hwnd, nullptr, FALSE);
    HWND h = g.hwnd;
    std::thread([h, withServer] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        bool ok = true;
        std::wstring err;
        if (withServer)
        {
            if (PortListening(kServerPort))
                Post(h, L"Using the server already running...", 600);
            else
            {
                Post(h, L"Starting local server...", 500);
                if (!Spawn(g.env.serverExe, g.env.serverDir, CREATE_NEW_CONSOLE, true, L"ApexSim Server"))
                { ok = false; err = L"Could not start the server."; }
                else
                {
                    // Play.bat waits two seconds; poll so a quick server is not slowed.
                    for (int i = 0; i < 40 && !PortListening(kServerPort); ++i) Sleep(100);
                }
            }
        }
        if (ok)
        {
            Post(h, L"Launching ApexSim...", 900);
            if (g.env.release)
            {
                if (!Spawn(g.env.gameExe, g.env.gameExe.parent_path(), 0, false, nullptr))
                { ok = false; err = L"Could not start ApexSim.exe."; }
            }
            else if (!SpawnEditorBuild())
            { ok = false; err = L"Could not start scripts\\play_editor.ps1."; }
        }
        WSACleanup();
        if (ok) { PostMessageW(h, WM_CLOSE, 0, 0); return; }
        g.failed = true;
        g.note = err;
        Post(h, L"Launch failed.", 1000);
        PostMessageW(h, WM_APP_DONE, 0, 0);
    }).detach();
}

// ------------------------------------------------------------------ painting

static void Layout()
{
    int W = S(820), H = S(460), m = S(40), gap = S(16), h = S(56);
    int y = H - S(112), x = m;
    auto place = [&](int i, int w) { g.btn[i] = { x, y, x + w, y + h }; x += w + gap; };
    place(BtnTrouble, S(130));
    int lw = S(230), aw = S(40);
    g.btn[BtnLaunch] = { x, y, x + lw - aw, y + h };
    g.btn[BtnArrow] = { x + lw - aw, y, x + lw, y + h };
    x += lw + gap;
    place(BtnConfig, S(180));
    place(BtnContent, S(150));
    g.btn[BtnClose] = { W - S(48), 0, W, S(36) };
}

static bool Enabled(int i)
{
    if (i == BtnClose) return true;
    if (!g.ready || g.busy) return i == BtnTrouble && g.ready;
    switch (i)
    {
    case BtnTrouble: return true;
    case BtnLaunch: case BtnArrow: return g.gameOk;
    default: return g.env.found;
    }
}

static int HitTest(POINT p)
{
    for (int i = 0; i < BtnCount; ++i)
        if (PtInRect(&g.btn[i], p)) return i;
    return -1;
}

static void RoundPath(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float rad)
{
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

static Gdiplus::RectF F(const RECT& r)
{
    return Gdiplus::RectF((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
}

static void DrawButton(Gdiplus::Graphics& gr, int i, const wchar_t* text, bool primary)
{
    using namespace Gdiplus;
    const RECT& r = g.btn[i];
    bool en = Enabled(i);
    bool hov = en && g.hover == i, dn = hov && g.down == i;
    Color base = primary ? Color(255, 214, 28, 44) : Color(255, 34, 39, 52);
    Color lit = primary ? Color(255, 238, 58, 74) : Color(255, 50, 57, 76);
    Color press = primary ? Color(255, 170, 18, 34) : Color(255, 24, 28, 38);
    Color off = Color(255, 27, 30, 39);
    SolidBrush fill(!en ? off : dn ? press : hov ? lit : base);
    GraphicsPath path;
    RoundPath(path, F(r), (float)S(6));
    gr.FillPath(&fill, &path);
    if (en && !hov) { Pen edge(Color(40, 255, 255, 255), 1.0f); gr.DrawPath(&edge, &path); }

    FontFamily fam(L"Segoe UI");
    Font font(&fam, (float)S(15), FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush ink(en ? Color(255, 255, 255, 255) : Color(255, 98, 104, 120));
    gr.DrawString(text, -1, &font, F(r), &sf, &ink);
}

static void DrawLaunch(Gdiplus::Graphics& gr)
{
    using namespace Gdiplus;
    const RECT& m = g.btn[BtnLaunch];
    const RECT& a = g.btn[BtnArrow];
    RECT u = { m.left, m.top, a.right, m.bottom };
    bool en = Enabled(BtnLaunch);
    Color base(255, 214, 28, 44), lit(255, 238, 58, 74), press(255, 170, 18, 34), off(255, 27, 30, 39);
    GraphicsPath path;
    RoundPath(path, F(u), (float)S(6));
    SolidBrush fill(en ? base : off);
    gr.FillPath(&fill, &path);
    for (int i : { BtnLaunch, BtnArrow })
    {
        if (!en || g.hover != i) continue;
        SolidBrush hb(g.down == i ? press : lit);
        gr.SetClip(F(g.btn[i]));
        gr.FillPath(&hb, &path);
        gr.ResetClip();
    }
    Pen div(Color(en ? 90 : 30, 0, 0, 0), 1.0f);
    gr.DrawLine(&div, (float)a.left, (float)(u.top + S(10)), (float)a.left, (float)(u.bottom - S(10)));

    FontFamily fam(L"Segoe UI");
    Font font(&fam, (float)S(16), FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush ink(en ? Color(255, 255, 255, 255) : Color(255, 98, 104, 120));
    gr.DrawString(L"LAUNCH", -1, &font, F(m), &sf, &ink);

    float cx = (a.left + a.right) / 2.0f, cy = (a.top + a.bottom) / 2.0f, t = (float)S(5);
    PointF tri[3] = { PointF(cx - t, cy - t / 2), PointF(cx + t, cy - t / 2), PointF(cx, cy + t / 2 + 1) };
    gr.FillPolygon(&ink, tri, 3);
}

static void Paint(HDC hdc, int W, int H)
{
    using namespace Gdiplus;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics gr(mem);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        LinearGradientBrush bg(Point(0, 0), Point(0, H), Color(255, 10, 12, 17), Color(255, 26, 30, 41));
        gr.FillRectangle(&bg, 0, 0, W, H);

        // Racing stripes, top right.
        SolidBrush red(Color(210, 214, 28, 44)), white(Color(60, 255, 255, 255));
        PointF s1[4] = { PointF((float)W - S(300), 0), PointF((float)W - S(215), 0), PointF((float)W - S(500), (float)S(300)), PointF((float)W - S(585), (float)S(300)) };
        PointF s2[4] = { PointF((float)W - S(195), 0), PointF((float)W - S(170), 0), PointF((float)W - S(455), (float)S(300)), PointF((float)W - S(480), (float)S(300)) };
        gr.FillPolygon(&red, s1, 4);
        gr.FillPolygon(&white, s2, 4);

        // Wordmark.
        FontFamily fam(L"Segoe UI");
        Font word(&fam, (float)S(70), FontStyleBold | FontStyleItalic, UnitPixel);
        StringFormat tight(StringFormat::GenericTypographic());
        RectF bounds;
        float wx = (float)S(40), wy = (float)S(80);
        gr.MeasureString(L"APEX", -1, &word, PointF(0, 0), &tight, &bounds);
        SolidBrush ink(Color(255, 246, 247, 250)), accent(Color(255, 230, 36, 54));
        gr.DrawString(L"APEX", -1, &word, PointF(wx, wy), &tight, &ink);
        gr.DrawString(L"SIM", -1, &word, PointF(wx + bounds.Width + S(3), wy), &tight, &accent);

        Font tag(&fam, (float)S(13), FontStyleBold, UnitPixel);
        SolidBrush dim(Color(255, 138, 146, 164));
        StringFormat spaced;
        gr.DrawString(L"S I M R A C I N G   P L A T F O R M", -1, &tag, PointF(wx + S(4), wy + S(92)), &spaced, &dim);

        // Progress bar.
        float bx = (float)S(40), by = (float)S(268), bw = (float)(W - S(80)), bh = (float)S(6);
        SolidBrush track(Color(255, 38, 43, 57));
        GraphicsPath tp;
        RoundPath(tp, RectF(bx, by, bw, bh), bh / 2);
        gr.FillPath(&track, &tp);
        float fw = bw * std::min(1.0f, std::max(0.0f, g.shown));
        if (fw > bh)
        {
            LinearGradientBrush fillb(PointF(bx, 0), PointF(bx + bw, 0), Color(255, 214, 28, 44), Color(255, 255, 110, 90));
            GraphicsPath fp;
            RoundPath(fp, RectF(bx, by, fw, bh), bh / 2);
            gr.FillPath(&fillb, &fp);
        }

        Font body(&fam, (float)S(14), FontStyleRegular, UnitPixel);
        SolidBrush statusInk(g.failed && g.ready ? Color(255, 255, 128, 120) : Color(255, 214, 219, 230));
        gr.DrawString(g.status.c_str(), -1, &body, PointF(bx, by + S(14)), &spaced, &statusInk);
        if (!g.note.empty())
            gr.DrawString(g.note.c_str(), -1, &body, RectF(bx, by + S(38), bw, (float)S(40)), &spaced, &dim);

        // Buttons.
        DrawButton(gr, BtnTrouble, L"Troubleshooting", false);
        DrawLaunch(gr);
        DrawButton(gr, BtnConfig, L"Edit configuration", false);
        DrawButton(gr, BtnContent, L"Manage content", false);

        // Close glyph.
        const RECT& c = g.btn[BtnClose];
        if (g.hover == BtnClose)
        {
            SolidBrush hb(Color(255, 196, 30, 48));
            gr.FillRectangle(&hb, F(c));
        }
        Pen x(Color(255, 190, 196, 210), 1.4f);
        float cx = (c.left + c.right) / 2.0f, cy = (c.top + c.bottom) / 2.0f, d = (float)S(5);
        gr.DrawLine(&x, cx - d, cy - d, cx + d, cy + d);
        gr.DrawLine(&x, cx - d, cy + d, cx + d, cy - d);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------- dialogs

static void CenterOn(HWND w, HWND owner)
{
    RECT r, o;
    GetWindowRect(w, &r);
    GetWindowRect(owner, &o);
    int x = o.left + ((o.right - o.left) - (r.right - r.left)) / 2;
    int y = o.top + ((o.bottom - o.top) - (r.bottom - r.top)) / 2;
    SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int dpi)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
                           MulDiv(w, dpi, 96), MulDiv(h, dpi, 96), parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

static void SetFontAll(HWND parent, HFONT f)
{
    EnumChildWindows(parent, [](HWND c, LPARAM lp) -> BOOL { SendMessageW(c, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE; }, (LPARAM)f);
}

static HWND CreateDialogWindow(const wchar_t* cls, const wchar_t* title, int w, int h, int dpi)
{
    RECT r = { 0, 0, MulDiv(w, dpi, 96), MulDiv(h, dpi, 96) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dpi);
    HWND owner = g.hwnd;
    EnableWindow(owner, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, cls, title, style, 0, 0, r.right - r.left, r.bottom - r.top,
                              owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hw) { EnableWindow(owner, TRUE); return nullptr; }
    CenterOn(hw, owner);
    ShowWindow(hw, SW_SHOW);
    g.modeless = hw;
    return hw;
}

static void CloseDialogWindow(HWND hw)
{
    g.modeless = nullptr;
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
    DestroyWindow(hw);
}

// ---- Edit configuration

enum { IdHost = 200, IdPort, IdRes, IdMode, IdLimit, IdScreens, IdVsync };
static const char* kModes[] = { "fullscreen", "borderless", "windowed" };
static HFONT gCfgFont;

static LRESULT CALLBACK ConfigProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        int dpi = g.dpi;
        gCfgFont = MakeUiFont(dpi);
        const Settings& s = g.settings;
        Ctl(hw, L"BUTTON", L"Network", BS_GROUPBOX, 12, 10, 436, 92, 0, dpi);
        Ctl(hw, L"STATIC", L"Server address", 0, 28, 38, 120, 20, 0, dpi);
        Ctl(hw, L"EDIT", Widen(s.host).c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 160, 35, 272, 24, IdHost, dpi);
        Ctl(hw, L"STATIC", L"Port", 0, 28, 70, 120, 20, 0, dpi);
        Ctl(hw, L"EDIT", std::to_wstring(s.port).c_str(), WS_BORDER | WS_TABSTOP | ES_NUMBER, 160, 67, 100, 24, IdPort, dpi);

        Ctl(hw, L"BUTTON", L"Graphics", BS_GROUPBOX, 12, 112, 436, 206, 0, dpi);
        Ctl(hw, L"STATIC", L"Resolution", 0, 28, 140, 120, 20, 0, dpi);
        wchar_t res[32];
        swprintf_s(res, L"%dx%d", s.resX, s.resY);
        Ctl(hw, L"EDIT", res, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 160, 137, 140, 24, IdRes, dpi);
        Ctl(hw, L"STATIC", L"Window mode", 0, 28, 172, 120, 20, 0, dpi);
        HWND mode = Ctl(hw, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 160, 169, 140, 120, IdMode, dpi);
        for (const char* m : kModes) SendMessageW(mode, CB_ADDSTRING, 0, (LPARAM)Widen(m).c_str());
        int mi = 1;
        for (int i = 0; i < 3; ++i) if (s.mode == kModes[i]) mi = i;
        SendMessageW(mode, CB_SETCURSEL, mi, 0);
        Ctl(hw, L"STATIC", L"Frame limit (0 = uncapped)", 0, 28, 204, 130, 34, 0, dpi);
        Ctl(hw, L"EDIT", std::to_wstring(s.frameLimit).c_str(), WS_BORDER | WS_TABSTOP | ES_NUMBER, 160, 201, 100, 24, IdLimit, dpi);
        Ctl(hw, L"STATIC", L"Screens", 0, 28, 244, 120, 20, 0, dpi);
        HWND scr = Ctl(hw, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 160, 241, 200, 100, IdScreens, dpi);
        SendMessageW(scr, CB_ADDSTRING, 0, (LPARAM)L"1  (one monitor)");
        SendMessageW(scr, CB_ADDSTRING, 0, (LPARAM)L"3  (triple monitors)");
        SendMessageW(scr, CB_SETCURSEL, s.screens == 3 ? 1 : 0, 0);
        HWND vs = Ctl(hw, L"BUTTON", L"V-Sync", BS_AUTOCHECKBOX | WS_TABSTOP, 160, 278, 140, 22, IdVsync, dpi);
        SendMessageW(vs, BM_SETCHECK, s.vsync ? BST_CHECKED : BST_UNCHECKED, 0);

        Ctl(hw, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 252, 332, 90, 30, IDOK, dpi);
        Ctl(hw, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 352, 332, 90, 30, IDCANCEL, dpi);
        SetFontAll(hw, gCfgFont);
        SetFocus(GetDlgItem(hw, IdHost));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { CloseDialogWindow(hw); return 0; }
        if (LOWORD(wp) == IDOK)
        {
            Settings n = g.settings;
            std::string host = Trim(Narrow(GetText(GetDlgItem(hw, IdHost))));
            int port = _wtoi(GetText(GetDlgItem(hw, IdPort)).c_str());
            int x = 0, y = 0;
            std::string res = Narrow(GetText(GetDlgItem(hw, IdRes)));
            int limit = _wtoi(GetText(GetDlgItem(hw, IdLimit)).c_str());
            const wchar_t* err = nullptr;
            if (host.empty() || host.find(' ') != std::string::npos) err = L"Enter the server's host name or IP address.";
            else if (port < 1 || port > 65535) err = L"The port must be between 1 and 65535.";
            else if (sscanf_s(res.c_str(), "%dx%d", &x, &y) != 2 || x < 320 || y < 240 || x > 16384 || y > 16384)
                err = L"The resolution must look like 1920x1080.";
            else if (limit < 0 || limit > 1000) err = L"The frame limit must be between 0 and 1000.";
            if (err) { MessageBoxW(hw, err, L"Edit configuration", MB_OK | MB_ICONWARNING); return 0; }
            n.host = host; n.port = port; n.resX = x; n.resY = y; n.frameLimit = limit;
            n.mode = kModes[std::max(0, (int)SendDlgItemMessageW(hw, IdMode, CB_GETCURSEL, 0, 0))];
            n.screens = SendDlgItemMessageW(hw, IdScreens, CB_GETCURSEL, 0, 0) == 1 ? 3 : 1;
            n.vsync = SendDlgItemMessageW(hw, IdVsync, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!SaveSettings(g.env.settings, n))
            {
                MessageBoxW(hw, (L"Could not write " + g.env.settings.wstring()).c_str(), L"Edit configuration", MB_OK | MB_ICONERROR);
                return 0;
            }
            g.settings = n;
            CloseDialogWindow(hw);
            return 0;
        }
        break;
    case WM_CLOSE: CloseDialogWindow(hw); return 0;
    case WM_NCDESTROY:
        if (gCfgFont) { DeleteObject(gCfgFont); gCfgFont = nullptr; }
        break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---- Manage content

enum { IdTab = 100, IdList, IdInfo, IdImport, IdOpen };
static HFONT gContentFont;
static HANDLE gImport;  // a running import, so the list refreshes when it ends

static fs::path PickFolder(HWND owner, const wchar_t* title)
{
    static bool com = false;
    if (!com) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); com = true; }
    fs::path result;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
    {
        DWORD opts = 0;
        d->GetOptions(&opts);
        d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
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

static void PopulateList(HWND hw)
{
    HWND tab = GetDlgItem(hw, IdTab), list = GetDlgItem(hw, IdList);
    int t = TabCtrl_GetCurSel(tab);
    std::vector<Item> items = ListContent(g.env, t);
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
    if (items.empty()) info += L" - nothing found in " + TabFolder(g.env, t).wstring();
    SetWindowTextW(GetDlgItem(hw, IdInfo), info.c_str());
    EnableWindow(GetDlgItem(hw, IdImport), t == TabTracks || t == TabCars);
}

enum ImportKind { ImpNone, ImpCar, ImpTrack, ImpCars, ImpTracks };

static ImportKind DetectKind(HWND hw, const fs::path& dir)
{
    std::wstring n = dir.filename().wstring();
    if (_wcsicmp(n.c_str(), L"cars") == 0) return ImpCars;
    if (_wcsicmp(n.c_str(), L"tracks") == 0) return ImpTracks;
    if (Exists(dir / L"data.acd") || Exists(dir / L"ui" / L"ui_car.json")) return ImpCar;
    if (Exists(dir / L"ui" / L"ui_track.json") || Exists(dir / L"ai")) return ImpTrack;
    int r = MessageBoxW(hw, L"Is this folder a car or a track?\n\nYes = car, No = track", L"Import from Assetto Corsa",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDYES ? ImpCar : r == IDNO ? ImpTrack : ImpNone;
}

static void StartImport(HWND hw)
{
    if (gImport) { MessageBoxW(hw, L"An import is already running.", L"Import", MB_OK | MB_ICONINFORMATION); return; }
    int tab = TabCtrl_GetCurSel(GetDlgItem(hw, IdTab));
    if (!Exists(g.env.scripts / L"ac_import.py"))
    {
        MessageBoxW(hw,
            L"Importing needs the ApexSim tools (the scripts folder) and Python 3 with numpy, Pillow and PyYAML.\n\n"
            L"This installation does not include them. Run the importer from a source checkout.",
            L"Import from Assetto Corsa", MB_OK | MB_ICONINFORMATION);
        return;
    }
    fs::path dir = PickFolder(hw, tab == TabCars ? L"Select an Assetto Corsa car folder (or content\\cars)"
                                                 : L"Select an Assetto Corsa track folder (or content\\tracks)");
    if (dir.empty()) return;
    ImportKind k = DetectKind(hw, dir);
    if (k == ImpNone) return;

    bool car = (k == ImpCar || k == ImpCars), all = (k == ImpCars || k == ImpTracks);
    fs::path script = g.env.scripts / (car ? L"ac_car_import.py" : L"ac_import.py");
    std::wstring d = dir.wstring();
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();  // a trailing \ would escape the quote
    // /S makes cmd strip exactly the outer quotes; the console stays open (pause) so the report can be read.
    std::wstring cmd = L"cmd.exe /S /C \"python \"" + script.wstring() + L"\" " + (all ? L"--all " : L"") +
                       L"\"" + d + L"\" & echo. & pause\"";
    STARTUPINFOW si = { sizeof si };
    si.lpTitle = const_cast<wchar_t*>(L"ApexSim import");
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, g.env.root.c_str(), &si, &pi))
    {
        MessageBoxW(hw, L"Could not start the importer.", L"Import", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    gImport = pi.hProcess;
    SetWindowTextW(GetDlgItem(hw, IdInfo), L"Importing... see the console window. This list refreshes when it closes.");
    SetTimer(hw, 1, 500, nullptr);
}

static void LayoutContent(HWND hw)
{
    RECT c;
    GetClientRect(hw, &c);
    int m = S(10);
    HWND tab = GetDlgItem(hw, IdTab);
    SetWindowPos(tab, nullptr, m, m, c.right - 2 * m, c.bottom - S(100), SWP_NOZORDER);
    RECT r = { 0, 0, c.right - 2 * m, c.bottom - S(100) };
    TabCtrl_AdjustRect(tab, FALSE, &r);
    SetWindowPos(GetDlgItem(hw, IdList), nullptr, m + r.left, m + r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    HWND list = GetDlgItem(hw, IdList);
    int w = r.right - r.left - GetSystemMetrics(SM_CXVSCROLL) - S(4);
    ListView_SetColumnWidth(list, 0, w * 7 / 10);
    ListView_SetColumnWidth(list, 1, w * 3 / 10);
    SetWindowPos(GetDlgItem(hw, IdInfo), nullptr, m, c.bottom - S(84), c.right - 2 * m, S(36), SWP_NOZORDER);
    int by = c.bottom - S(44), bh = S(32);
    SetWindowPos(GetDlgItem(hw, IdImport), nullptr, m, by, S(190), bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IdOpen), nullptr, m + S(200), by, S(120), bh, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hw, IDCANCEL), nullptr, c.right - m - S(100), by, S(100), bh, SWP_NOZORDER);
}

static LRESULT CALLBACK ContentProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        int dpi = g.dpi;
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
        case IdOpen:
        {
            fs::path p = TabFolder(g.env, TabCtrl_GetCurSel(GetDlgItem(hw, IdTab)));
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

// -------------------------------------------------------------- main window

static void Activate(int i)
{
    switch (i)
    {
    case BtnTrouble: ShellExecuteW(g.hwnd, L"open", kTroubleshootUrl, nullptr, nullptr, SW_SHOWNORMAL); break;
    case BtnLaunch: LaunchAsync(false); break;
    case BtnArrow:
    {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (g.serverOk ? 0 : MF_GRAYED), 1, L"Launch with local server");
        POINT p = { g.btn[BtnLaunch].left, g.btn[BtnLaunch].bottom + S(2) };
        ClientToScreen(g.hwnd, &p);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, g.hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 1) LaunchAsync(true);
        break;
    }
    case BtnConfig: CreateDialogWindow(L"ApexCfg", L"Edit configuration", 460, 380, g.dpi); break;
    case BtnContent: CreateDialogWindow(L"ApexContent", L"Manage content", 640, 500, g.dpi); break;
    case BtnClose: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
    }
}

static LRESULT CALLBACK MainProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT c;
        GetClientRect(hw, &c);
        Paint(dc, c.right, c.bottom);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_NCHITTEST:
    {
        LRESULT r = DefWindowProcW(hw, msg, wp, lp);
        if (r == HTCLIENT)
        {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hw, &p);
            if (HitTest(p) < 0) return HTCAPTION;  // drag the borderless window by its background
        }
        return r;
    }
    case WM_MOUSEMOVE:
    {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = HitTest(p);
        if (h != g.hover)
        {
            g.hover = h;
            InvalidateRect(hw, nullptr, FALSE);
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, hw, 0 };
            TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = -1;
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = HitTest(p);
        if (h >= 0 && Enabled(h)) { g.down = h; SetCapture(hw); InvalidateRect(hw, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (g.down < 0) return 0;
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int was = g.down;
        g.down = -1;
        ReleaseCapture();
        InvalidateRect(hw, nullptr, FALSE);
        if (HitTest(p) == was && Enabled(was)) Activate(was);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostMessageW(hw, WM_CLOSE, 0, 0);
        else if (wp == VK_RETURN && Enabled(BtnLaunch)) Activate(BtnLaunch);
        return 0;
    case WM_TIMER:
    {
        float d = g.target - g.shown;
        if (std::fabs(d) < 0.002f) g.shown = g.target;
        else g.shown += d * 0.18f + (d > 0 ? 0.002f : 0);
        InvalidateRect(hw, nullptr, FALSE);
        if (g.shown == g.target && g.ready) KillTimer(hw, 1);
        return 0;
    }
    case WM_APP_STATUS:
    {
        std::unique_ptr<std::wstring> text((std::wstring*)lp);
        g.status = *text;
        g.target = wp / 1000.0f;
        SetTimer(hw, 1, 16, nullptr);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }
    case WM_APP_DONE:
        g.ready = true;
        g.busy = false;
        SetTimer(hw, 1, 16, nullptr);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED:
    {
        g.dpi = HIWORD(wp);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hw, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    auto reg = [&](const wchar_t* name, WNDPROC proc, HBRUSH bg) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = bg;
        wc.lpszClassName = name;
        wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        RegisterClassExW(&wc);
    };
    reg(L"ApexLauncher", MainProc, nullptr);
    reg(L"ApexCfg", ConfigProc, (HBRUSH)(COLOR_BTNFACE + 1));
    reg(L"ApexContent", ContentProc, (HBRUSH)(COLOR_BTNFACE + 1));

    g.dpi = (int)GetDpiForSystem();
    Layout();
    int W = S(820), H = S(460);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"ApexLauncher", L"ApexSim", WS_POPUP,
                             wa.left + ((wa.right - wa.left) - W) / 2, wa.top + ((wa.bottom - wa.top) - H) / 2,
                             W, H, nullptr, nullptr, inst, nullptr);
    g.dpi = (int)GetDpiForWindow(g.hwnd);
    if (S(820) != W)
    {
        W = S(820); H = S(460);
        SetWindowPos(g.hwnd, nullptr, wa.left + ((wa.right - wa.left) - W) / 2, wa.top + ((wa.bottom - wa.top) - H) / 2,
                     W, H, SWP_NOZORDER);
    }
    Layout();
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    SetForegroundWindow(g.hwnd);

    // The splash is on screen; everything else happens off the UI thread.
    std::thread(WorkerMain, g.hwnd).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (g.modeless && IsDialogMessageW(g.modeless, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
