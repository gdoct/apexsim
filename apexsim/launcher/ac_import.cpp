// See ac_import.h.

#include "ac_import.h"
#include "process.h"
#include "ui.h"

#include <shellapi.h>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static const int kMinPython = 311;  // 3.11: what the importers are written and tested on

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

// The command that starts a Python of at least kMinPython ("py -3", "python", ...), and
// the highest too-old version seen (0 when none). The Microsoft Store's python.exe stub
// prints nothing and exits non-zero, so it simply does not count.
static bool FindPython(std::wstring& command, int& tooOld)
{
    static const wchar_t* candidates[] = { L"py -3", L"python", L"python3" };
    tooOld = 0;
    for (const wchar_t* c : candidates)
    {
        std::string out;
        std::wstring line = std::wstring(c) + L" -c \"import sys;print(sys.version_info[0]*100+sys.version_info[1])\"";
        if (!RunCapture(line, out, 15000)) continue;
        int v = atoi(out.c_str());
        if (v >= kMinPython) { command = c; return true; }
        if (v > tooOld) tooOld = v;
    }
    return false;
}

static void ExplainNoPython(HWND hw, int tooOld)
{
    std::wstring m = tooOld
        ? L"Importing needs Python 3.11 or newer. Python " + std::to_wstring(tooOld / 100) + L"." + std::to_wstring(tooOld % 100) +
              L" is installed, which is too old.\n\n"
        : L"Importing needs Python 3.11 or newer, and none was found on this PC.\n\n";
    m += L"Install it from python.org/downloads (3.12 is a good choice). In the installer, leave the \"py launcher\" ticked "
         L"or tick \"Add python.exe to PATH\", then try again. Nothing else needs installing by hand: the launcher sets up "
         L"the importer's packages itself the first time (this needs an internet connection).\n\n"
         L"Open the download page now?";
    if (MessageBoxW(hw, m.c_str(), L"Import from Assetto Corsa", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(hw, L"open", L"https://www.python.org/downloads/windows/", nullptr, nullptr, SW_SHOWNORMAL);
}

static std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static fs::path VenvDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)std::size(buf));
    fs::path base = (n && n < std::size(buf)) ? fs::path(buf) : fs::temp_directory_path();
    return base / L"ApexSim" / L"importer-venv";
}

HANDLE StartAcImport(HWND hw, const Env& env, bool wantCar)
{
    const fs::path& tools = env.importerDir;
    fs::path req = tools / L"importer-requirements.txt";
    if (tools.empty() || !Exists(tools / L"ac_import.py") || !Exists(req))
    {
        MessageBoxW(hw,
            L"This installation does not include the importer (Tools\\importer).\n\n"
            L"Download a release that includes it, or run the importer from a source checkout.",
            L"Import from Assetto Corsa", MB_OK | MB_ICONINFORMATION);
        return nullptr;
    }
    std::wstring py;
    int tooOld = 0;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool havePython = FindPython(py, tooOld);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!havePython) { ExplainNoPython(hw, tooOld); return nullptr; }

    fs::path dir = PickFolder(hw, wantCar ? L"Select an Assetto Corsa car folder (or content\\cars)"
                                          : L"Select an Assetto Corsa track folder (or content\\tracks)");
    if (dir.empty()) return nullptr;
    ImportKind k = DetectKind(hw, dir);
    if (k == ImpNone) return nullptr;

    bool car = (k == ImpCar || k == ImpCars), all = (k == ImpCars || k == ImpTracks);
    std::wstring d = dir.wstring();
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();  // a trailing \ would escape the quote

    fs::path venv = VenvDir();
    fs::path venvPy = venv / L"Scripts" / L"python.exe";
    fs::path stamp = venv / L"importer-requirements.installed.txt";
    // A release carries the packages as wheels for each Python it supports
    // (Tools\importer\wheels), so a PC with no internet can import too;
    // PyPI is the fallback, for a Python the wheels do not cover.
    fs::path wheels = tools / L"wheels";
    std::error_code wheelsError;
    bool haveWheels = fs::is_directory(wheels, wheelsError);
    bool haveVenv = Exists(venvPy);
    bool upToDate = haveVenv && Exists(stamp) && ReadAll(stamp) == ReadAll(req);

    // One console line: set up if needed, then import. `&&` stops at the first failure and the
    // console stays open (pause) so the report or the error can be read. /S makes cmd strip
    // exactly the outer quotes.
    const std::wstring q = L"\"";
    std::wstring line;
    if (!upToDate)
    {
        line += haveWheels
            ? L"echo Setting up the importer (first use or updated requirements)... && "
            : L"echo Setting up the importer (first use or updated requirements; needs an internet connection)... && ";
        line += L"echo If this fails, delete " + q + venv.wstring() + q + L" and try again. && ";
        if (!haveVenv) line += py + L" -m venv " + q + venv.wstring() + q + L" && ";
        const std::wstring pip = q + venvPy.wstring() + q + L" -m pip install --disable-pip-version-check ";
        const std::wstring fromIndex = pip + L"-r " + q + req.wstring() + q;
        if (haveWheels)
        {
            line += L"(" + pip + L"--no-index --find-links " + q + wheels.wstring() + q + L" -r " + q + req.wstring() + q +
                L" || " + fromIndex + L") && ";
        }
        else
        {
            line += fromIndex + L" && ";
        }
        line += L"copy /Y " + q + req.wstring() + q + L" " + q + stamp.wstring() + q + L" >nul && ";
    }
    line += q + venvPy.wstring() + q + L" " + q + (tools / (car ? L"ac_car_import.py" : L"ac_import.py")).wstring() + q + L" ";
    if (env.release) line += L"--root " + q + env.root.wstring() + q + L" ";  // into this install's Game and Server
    line += all ? L"--all " : L"";
    line += q + d + q;
    std::wstring cmd = L"cmd.exe /S /C \"" + line + L" & echo. & pause\"";

    STARTUPINFOW si = { sizeof si };
    si.lpTitle = const_cast<wchar_t*>(L"ApexSim import");
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, env.root.c_str(), &si, &pi))
    {
        MessageBoxW(hw, L"Could not start the importer.", L"Import", MB_OK | MB_ICONERROR);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
