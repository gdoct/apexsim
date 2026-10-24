// See env.h.

#include "env.h"
#include "text.h"

#include <fstream>

namespace fs = std::filesystem;

bool Exists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

Env Locate()
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
            e.tracksCustom = e.serverDir / L"content" / L"tracks" / L"custom";
            e.importerDir = d / L"Tools" / L"importer";
            e.tracksAreExports = true;
            break;
        }
        // A source checkout: the code root (server\, scripts\, hud\) with the
        // content root, apexsim-content, beside it.
        fs::path content = (d / L".." / L"apexsim-content").lexically_normal();
        if (Exists(d / L"server") && Exists(content / L"cars"))
        {
            e.found = true;
            e.root = d;
            // The repo launches through play_editor.ps1 (builds, then runs the editor
            // build with -game); there is no packaged ApexSim.exe to start.
            e.playScript = d / L"scripts" / L"play_editor.ps1";
            e.serverExe = d / L"server" / L"server" / L"target" / L"release" / L"apexsim-server.exe";
            // server.toml here is the release one (content\ beside the exe), so the
            // server is pointed at the content root through APEXSIM_CONTENT.
            e.serverDir = d;
            e.contentRoot = content;
            e.settings = d / L"game-unreal" / L"settings.yml";
            e.scripts = d / L"scripts";
            e.importerDir = content / L"tools";
            e.carsDefault = content / L"cars" / L"default";
            e.carsCustom = content / L"cars" / L"custom";
            e.wheels = content / L"wheels";
            e.hudDefault = d / L"hud" / L"default";
            e.hudCustom = d / L"hud" / L"custom";
            e.tracksDefault = content / L"tracks" / L"default";
            e.tracksCustom = content / L"tracks" / L"custom";
            break;
        }
        if (!d.has_parent_path() || d.parent_path() == d) break;
        d = d.parent_path();
    }
    return e;
}

std::wstring ReadVersion(const Env& e)
{
    std::ifstream f(e.release ? e.root / L"release.json" : e.root / L"game-unreal" / L"Config" / L"DefaultGame.ini");
    std::string line;
    while (f && std::getline(f, line))
    {
        std::string v;
        if (e.release)
        {
            size_t k = line.find("\"version\"");
            if (k == std::string::npos) continue;
            size_t a = line.find('"', line.find(':', k) + 1);
            size_t b = a == std::string::npos ? a : line.find('"', a + 1);
            if (b == std::string::npos) continue;
            v = line.substr(a + 1, b - a - 1);
        }
        else
        {
            std::string t = Trim(line);
            if (t.rfind("ProjectVersion", 0) != 0) continue;
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            v = Trim(t.substr(eq + 1));
        }
        if (!v.empty()) return Widen(v);
    }
    return {};
}
