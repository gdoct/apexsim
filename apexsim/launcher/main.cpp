// ApexSim launcher ("launcher.exe"). Plain Win32 + GDI+, no runtime beyond the CRT
// (linked statically), so it appears in a few tens of milliseconds.
//
// Flow (see README.md): splash with progress bar -> environment / config /
// content checks on a worker thread -> buttons enable.
//
// Build: launcher\build.bat  ->  launcher\out\launcher.exe
//
// Modules: env (where the install is), content (Manage content) with ac_import
// (the Assetto Corsa importers), ui (secondary windows), process, unblock.

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

#include "content.h"
#include "env.h"
#include "text.h"
#include "ui.h"
#include "unblock.h"

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
static const unsigned short kServerPort = 9000;  // the default server port

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
    std::string graphicsApi = "auto";  // auto | dx12 | dx11; the game's boot module applies it
    std::string host = "127.0.0.1";
    int port = kServerPort;
    // Not edited here, only carried through the rewrite (the game owns them):
    // server.tls (auto | on | off), tls_verify, tls_fingerprint.
    std::string tls = "auto";
    std::string tlsVerify = "true";
    std::string tlsFingerprint;
    bool showLauncher = true;  // false: launcher.exe starts the game at once and shows no window
    // telemetry_out (enabled, host, port, rate_hz): carried through, the game owns them.
    std::string telemetryEnabled = "false";
    std::string telemetryHost = "127.0.0.1";
    std::string telemetryPort = "9996";
    std::string telemetryRate = "60";
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
            else if (key == "graphics_api" && (val == "auto" || val == "dx12" || val == "dx11")) out.graphicsApi = val;
        }
        else if (section == "server")
        {
            if (key == "host" && !val.empty()) out.host = val;
            else if (key == "port") out.port = atoi(val.c_str());
            // Kept as written (less any trailing comment); the game validates them.
            else if (key == "tls" || key == "tls_verify" || key == "tls_fingerprint")
            {
                size_t hash = val.find('#');
                if (hash != std::string::npos) val = Trim(val.substr(0, hash));
                if (key == "tls") { if (!val.empty()) out.tls = val; }
                else if (key == "tls_verify") { if (!val.empty()) out.tlsVerify = val; }
                else out.tlsFingerprint = val;
            }
        }
        else if (section == "telemetry_out")
        {
            size_t hash = val.find('#');
            if (hash != std::string::npos) val = Trim(val.substr(0, hash));
            if (val.empty()) continue;
            if (key == "enabled") out.telemetryEnabled = val;
            else if (key == "host") out.telemetryHost = val;
            else if (key == "port") out.telemetryPort = val;
            else if (key == "rate_hz") out.telemetryRate = val;
        }
        else if (section == "launcher")
        {
            // The same spellings the game's parser takes; anything else keeps the default.
            if (key == "show")
            {
                if (!_stricmp(val.c_str(), "true") || !_stricmp(val.c_str(), "yes") || !_stricmp(val.c_str(), "on") || val == "1")
                    out.showLauncher = true;
                else if (!_stricmp(val.c_str(), "false") || !_stricmp(val.c_str(), "no") || !_stricmp(val.c_str(), "off") || val == "0")
                    out.showLauncher = false;
            }
        }
    }
    return true;
}

static bool SaveSettings(const fs::path& path, const Settings& s)
{
    char buf[4096];
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
        "  # auto | dx12 | dx11: the DirectX the game renders with, read when it\n"
        "  # starts. auto is DirectX 12 where the graphics card supports it. dx11\n"
        "  # is for a card or driver that draws the scene wrongly under DirectX 12;\n"
        "  # it looks plainer. -dx11 or -dx12 on the command line wins.\n"
        "  graphics_api: %s\n"
        "\n"
        "server:\n"
        "  # The server the game connects to when it starts. 127.0.0.1 is a server\n"
        "  # on this machine, such as the one the launcher starts for you.\n"
        "  host: %s\n"
        "  port: %d\n"
        "  # TLS on the connection: auto | on | off. auto tries TLS and falls back\n"
        "  # to plaintext only when the server does not speak it; on never falls\n"
        "  # back. Telemetry and input (UDP) are never encrypted.\n"
        "  tls: %s\n"
        "  # Check the server's certificate against the system's trusted roots and\n"
        "  # the host name. false encrypts without checking: development only.\n"
        "  tls_verify: %s\n"
        "  # Trust exactly this certificate instead (SHA-256, as the server's\n"
        "  # operator gives it, or as the game logs it when it refuses one): how a\n"
        "  # self-signed certificate is trusted. Empty: none. Cleared when host changes.\n"
        "  tls_fingerprint: %s\n"
        "\n"
        "telemetry_out:\n"
        "  # A UDP packet of the car's motion, revs, pedals, tyres and session for\n"
        "  # SimHub, motion rigs, shakers and dash apps (the layout is in the game's\n"
        "  # docs: telemetry-output). 127.0.0.1 is an app on this machine.\n"
        "  enabled: %s\n"
        "  host: %s\n"
        "  port: %s\n"
        "  # Packets per second while driving, 1 to 400.\n"
        "  rate_hz: %s\n"
        "\n"
        "launcher:\n"
        "  # false: launcher.exe starts the game straight away, without a server, and\n"
        "  # shows no window. Run launcher.exe --show to bring the window back.\n"
        "  show: %s\n",
        s.resX, s.resY, s.mode.c_str(), s.vsync ? "true" : "false", s.frameLimit, s.screens, s.graphicsApi.c_str(),
        s.host.c_str(), s.port, s.tls.c_str(), s.tlsVerify.c_str(), s.tlsFingerprint.c_str(),
        s.telemetryEnabled.c_str(), s.telemetryHost.c_str(), s.telemetryPort.c_str(), s.telemetryRate.c_str(),
        s.showLauncher ? "true" : "false");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << buf;
    return (bool)f;
}

// ----------------------------------------------------------------- app state

enum
{
    WM_APP_STATUS = WM_APP + 1,  // wParam = progress in 1/1000, lParam = new std::wstring*
    WM_APP_DONE = WM_APP + 2,    // worker finished; buttons may enable
};

enum { BtnTrouble, BtnLaunch, BtnArrow, BtnConfig, BtnContent, BtnClose, BtnServer, BtnShow, BtnCount };

struct App
{
    Env env;
    Settings settings;
    HWND hwnd = nullptr;
    int dpi = 96;
    std::wstring status = L"Starting...", note, version, versionFound;
    float target = 0, shown = 0;
    bool ready = false, busy = false, failed = false;
    bool gameOk = false, serverOk = false;
    int hover = -1, down = -1;
    bool menuOpen = false;  // the launch dropdown (BtnServer), drawn in the window like the buttons
    RECT btn[BtnCount] = {};
} g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }

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
    if (g.env.found) g.versionFound = ReadVersion(g.env);  // shown once WM_APP_DONE lands on the UI thread

    // A downloaded package carries the mark of the web on every file, and
    // SmartScreen would stop the game or the server on it: unblock the shipped
    // binaries (unblock.txt). Never in a checkout.
    if (g.env.release)
    {
        Post(h, L"Unblocking downloaded files...", 250);
        UnblockListed(g.env.root);
    }

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

// Starts the client (no server): ApexSim.exe in a package, the editor build in a checkout.
static bool SpawnGame(std::wstring& err)
{
    if (g.env.release)
    {
        if (!Spawn(g.env.gameExe, g.env.gameExe.parent_path(), 0, false, nullptr))
        { err = L"Could not start ApexSim.exe."; return false; }
    }
    else if (!SpawnEditorBuild())
    { err = L"Could not start scripts\\play_editor.ps1."; return false; }
    return true;
}

// Starts the local server unless one is already listening. Needs Winsock up. `h` is
// the window to report progress to, or null when there is none (command-line launch).
static bool StartServer(HWND h, std::wstring& err)
{
    if (PortListening(kServerPort))
    {
        if (h) Post(h, L"Using the server already running...", 600);
        return true;
    }
    if (h) Post(h, L"Starting local server...", 500);
    if (!g.env.contentRoot.empty())
        SetEnvironmentVariableW(L"APEXSIM_CONTENT", g.env.contentRoot.c_str());
    if (!Spawn(g.env.serverExe, g.env.serverDir, CREATE_NEW_CONSOLE, true, L"ApexSim Server"))
    { err = L"Could not start the server."; return false; }
    // Poll rather than wait a fixed time, so a quick server is not slowed.
    for (int i = 0; i < 40 && !PortListening(kServerPort); ++i) Sleep(100);
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
        std::wstring err;
        bool ok = !withServer || StartServer(h, err);
        if (ok)
        {
            Post(h, L"Launching ApexSim...", 900);
            ok = SpawnGame(err);
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
    int y = H - S(184), x = m;  // the checkbox, progress bar and status lines sit below
    auto place = [&](int i, int w) { g.btn[i] = { x, y, x + w, y + h }; x += w + gap; };
    place(BtnConfig, S(170));
    place(BtnContent, S(150));
    place(BtnTrouble, S(140));
    // LAUNCH sits apart, flush with the right margin.
    int lw = S(220), aw = S(40);
    x = W - m - lw;
    g.btn[BtnLaunch] = { x, y, x + lw - aw, y + h };
    g.btn[BtnArrow] = { x + lw - aw, y, x + lw, y + h };
    g.btn[BtnClose] = { W - S(48), 0, W, S(36) };
    g.btn[BtnServer] = { g.btn[BtnLaunch].left, y + h + S(6), g.btn[BtnArrow].right, y + h + S(6) + S(44) };
    // "Show launcher" sits under the button row, flush left; the hit area takes in its label.
    g.btn[BtnShow] = { m, y + h + S(10), m + S(190), y + h + S(30) };
}

static bool Enabled(int i)
{
    if (i == BtnClose) return true;
    if (i == BtnServer) return g.menuOpen && g.serverOk && g.gameOk && g.ready && !g.busy;
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
        if ((i != BtnServer || g.menuOpen) && PtInRect(&g.btn[i], p)) return i;
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

static void DrawShowCheck(Gdiplus::Graphics& gr)
{
    using namespace Gdiplus;
    const RECT& r = g.btn[BtnShow];
    bool en = Enabled(BtnShow), on = g.settings.showLauncher;
    bool hov = en && g.hover == BtnShow;
    float side = (float)S(18), top = r.top + ((r.bottom - r.top) - side) / 2;
    RectF box((float)r.left, top, side, side);

    GraphicsPath path;
    RoundPath(path, box, (float)S(4));
    if (on)
    {
        SolidBrush fill(!en ? Color(255, 27, 30, 39) : hov ? Color(255, 238, 58, 74) : Color(255, 214, 28, 44));
        gr.FillPath(&fill, &path);
        Pen tick(Color(255, 255, 255, 255), (float)S(2));
        tick.SetLineJoin(LineJoinRound);
        PointF pts[3] = { PointF(box.X + side * 0.24f, box.Y + side * 0.54f), PointF(box.X + side * 0.43f, box.Y + side * 0.72f),
                          PointF(box.X + side * 0.78f, box.Y + side * 0.30f) };
        gr.DrawLines(&tick, pts, 3);
    }
    else
    {
        SolidBrush fill(Color(255, 34, 39, 52));
        gr.FillPath(&fill, &path);
        Pen edge(hov ? Color(200, 255, 255, 255) : Color(110, 255, 255, 255), 1.0f);
        gr.DrawPath(&edge, &path);
    }

    FontFamily fam(L"Segoe UI");
    Font font(&fam, (float)S(14), FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush ink(en ? Color(255, 214, 219, 230) : Color(255, 98, 104, 120));
    RectF label((float)r.left + side + S(8), (float)r.top, (float)(r.right - r.left) - side - S(8), (float)(r.bottom - r.top));
    gr.DrawString(L"Show launcher", -1, &font, label, &sf, &ink);

    if (!on)
    {
        // The way back, where the player who just unticked it will read it.
        Font hint(&fam, (float)S(13), FontStyleRegular, UnitPixel);
        SolidBrush dim(Color(255, 138, 146, 164));
        gr.DrawString(L"Next time the game starts directly. Bring this window back with: launcher.exe --show", -1, &hint,
                      RectF((float)r.right + S(8), (float)r.top, (float)S(520), (float)(r.bottom - r.top)), &sf, &dim);
    }
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
        float wx = (float)S(40), wy = (float)S(15);
        gr.MeasureString(L"APEX", -1, &word, PointF(0, 0), &tight, &bounds);
        SolidBrush ink(Color(255, 246, 247, 250)), accent(Color(255, 230, 36, 54));
        gr.DrawString(L"APEX", -1, &word, PointF(wx, wy), &tight, &ink);
        gr.DrawString(L"SIM", -1, &word, PointF(wx + bounds.Width + S(3), wy), &tight, &accent);

        Font tag(&fam, (float)S(13), FontStyleBold, UnitPixel);
        SolidBrush dim(Color(255, 138, 146, 164));
        StringFormat spaced;
        gr.DrawString(L"S I M R A C I N G   P L A T F O R M", -1, &tag, PointF(wx + S(4), wy + S(92)), &spaced, &dim);

        if (!g.version.empty())
        {
            std::wstring v = L"v" + g.version + (g.env.release ? L"" : L"  (source)");
            gr.DrawString(v.c_str(), -1, &tag, PointF(wx + S(4), wy + S(116)), &spaced, &dim);
        }

        // Progress bar, under the buttons.
        float bx = (float)S(40), by = (float)(g.btn[BtnLaunch].bottom + S(40)), bw = (float)(W - S(80)), bh = (float)S(6);
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
        DrawShowCheck(gr);
        if (g.menuOpen) DrawButton(gr, BtnServer, L"Launch with local server", true);

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

// ---- Edit configuration

enum { IdHost = 200, IdPort, IdRes, IdMode, IdLimit, IdScreens, IdApi, IdVsync };
static const char* kModes[] = { "fullscreen", "borderless", "windowed" };
static const char* kApis[] = { "auto", "dx12", "dx11" };
static const wchar_t* kApiLabels[] = { L"Automatic", L"DirectX 12", L"DirectX 11 (older GPUs)" };
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

        Ctl(hw, L"BUTTON", L"Graphics", BS_GROUPBOX, 12, 112, 436, 238, 0, dpi);
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
        Ctl(hw, L"STATIC", L"Graphics API", 0, 28, 276, 120, 20, 0, dpi);
        HWND api = Ctl(hw, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 160, 273, 200, 100, IdApi, dpi);
        int ai = 0;
        for (int i = 0; i < 3; ++i)
        {
            SendMessageW(api, CB_ADDSTRING, 0, (LPARAM)kApiLabels[i]);
            if (s.graphicsApi == kApis[i]) ai = i;
        }
        SendMessageW(api, CB_SETCURSEL, ai, 0);
        HWND vs = Ctl(hw, L"BUTTON", L"V-Sync", BS_AUTOCHECKBOX | WS_TABSTOP, 160, 310, 140, 22, IdVsync, dpi);
        SendMessageW(vs, BM_SETCHECK, s.vsync ? BST_CHECKED : BST_UNCHECKED, 0);

        Ctl(hw, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 252, 364, 90, 30, IDOK, dpi);
        Ctl(hw, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 352, 364, 90, 30, IDCANCEL, dpi);
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
            // A pinned TLS certificate belongs to the old host (as in the game).
            if (_stricmp(host.c_str(), n.host.c_str()) != 0) n.tlsFingerprint.clear();
            n.host = host; n.port = port; n.resX = x; n.resY = y; n.frameLimit = limit;
            n.mode = kModes[std::max(0, (int)SendDlgItemMessageW(hw, IdMode, CB_GETCURSEL, 0, 0))];
            n.screens = SendDlgItemMessageW(hw, IdScreens, CB_GETCURSEL, 0, 0) == 1 ? 3 : 1;
            n.graphicsApi = kApis[std::max(0, (int)SendDlgItemMessageW(hw, IdApi, CB_GETCURSEL, 0, 0))];
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

// -------------------------------------------------------------- main window

static void Activate(int i)
{
    switch (i)
    {
    case BtnTrouble: ShellExecuteW(g.hwnd, L"open", kTroubleshootUrl, nullptr, nullptr, SW_SHOWNORMAL); break;
    case BtnLaunch: g.menuOpen = false; LaunchAsync(false); break;
    case BtnArrow: g.menuOpen = !g.menuOpen; break;
    case BtnServer: g.menuOpen = false; LaunchAsync(true); break;
    case BtnConfig: CreateDialogWindow(g.hwnd, L"ApexCfg", L"Edit configuration", 460, 412, g.dpi); break;
    case BtnContent: OpenContentWindow(g.hwnd, g.env, g.dpi); break;
    case BtnShow:
    {
        Settings n = g.settings;
        n.showLauncher = !n.showLauncher;
        if (SaveSettings(g.env.settings, n)) g.settings = n;
        else MessageBoxW(g.hwnd, (L"Could not write " + g.env.settings.wstring()).c_str(), L"Show launcher", MB_OK | MB_ICONERROR);
        break;
    }
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
        if (g.menuOpen && h != BtnServer && h != BtnArrow) { g.menuOpen = false; InvalidateRect(hw, nullptr, FALSE); }
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
    case WM_NCLBUTTONDOWN:
        if (g.menuOpen) { g.menuOpen = false; InvalidateRect(hw, nullptr, FALSE); }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g.menuOpen) { g.menuOpen = false; InvalidateRect(hw, nullptr, FALSE); }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g.menuOpen) { g.menuOpen = false; InvalidateRect(hw, nullptr, FALSE); }
        else if (wp == VK_ESCAPE) PostMessageW(hw, WM_CLOSE, 0, 0);
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
        g.version = g.versionFound;
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

// Command line and the "Show launcher" setting, decided before any window exists.
//   --launch   start the game now, no window
//   --server   with a launch, start the local server first (unless one is listening)
//   --show     show the window, and turn "Show launcher" back on in settings.yml
// With "Show launcher" unticked a plain start behaves like --launch. Returns true
// when the game was started and main should exit; any failure falls through to the
// window, which is where the player can see what is wrong.
static bool AutoLaunch()
{
    bool show = false, launch = false, server = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        auto is = [&](int i, const wchar_t* name) {
            return _wcsicmp(argv[i], (std::wstring(L"--") + name).c_str()) == 0 ||
                   _wcsicmp(argv[i], (std::wstring(L"/") + name).c_str()) == 0;
        };
        for (int i = 1; i < argc; ++i)
        {
            if (is(i, L"show")) show = true;
            else if (is(i, L"launch")) launch = true;
            else if (is(i, L"server")) server = true;
        }
        LocalFree(argv);
    }

    Env env = Locate();
    if (!env.found) return false;
    Settings s = DefaultSettings();
    bool have = LoadSettings(env.settings, s);
    if (show)
    {
        if (have && !s.showLauncher) { s.showLauncher = true; SaveSettings(env.settings, s); }
        return false;
    }
    if (!launch && !(have && !s.showLauncher)) return false;

    if (!Exists(env.release ? env.gameExe : env.playScript)) return false;
    if (env.release) UnblockListed(env.root);  // as WorkerMain does, before anything is started
    g.env = env;
    std::wstring err;
    if (server)
    {
        if (!Exists(env.serverExe)) return false;
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        bool ok = StartServer(nullptr, err);
        WSACleanup();
        if (!ok) return false;
    }
    return SpawnGame(err);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    if (AutoLaunch()) return 0;

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
    reg(kContentWindowClass, ContentProc, (HBRUSH)(COLOR_BTNFACE + 1));

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
        if (HWND d = ActiveDialog(); d && IsDialogMessageW(d, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
