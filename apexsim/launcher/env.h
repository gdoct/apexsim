// Where the launcher is: a release package (Game\, Server\) or a source
// checkout (apexsim\ with apexsim-content\ beside it), and the paths of what
// it starts and lists in each.

#pragma once

#include <filesystem>
#include <string>

struct Env
{
    bool found = false;     // a game install or a source checkout was located
    bool release = false;   // packaged layout (Game\, Server\) vs. the repo
    std::filesystem::path root;
    std::filesystem::path gameExe, playScript, serverExe, serverDir, settings, scripts;
    std::filesystem::path importerDir;   // holds ac_import.py, ac_car_import.py and importer-requirements.txt
    std::filesystem::path carsDefault, carsCustom, wheels, hudDefault, hudCustom, tracksDefault, tracksCustom;
    std::filesystem::path contentRoot;   // a checkout's apexsim-content, handed to its server as APEXSIM_CONTENT
    bool tracksAreExports = false;  // packaged tracks are .uescene.json, the repo's are .yaml
};

bool Exists(const std::filesystem::path& p);

// Walk up from the launcher's own folder: it may sit at the root of a release
// folder (beside Game\ and Server\), inside Game\, or in the checkout's launcher\.
Env Locate();

// The client's version: release.json's "version" beside Game\ in a package (written
// by build_release.ps1), else ProjectVersion from DefaultGame.ini in a checkout.
std::wstring ReadVersion(const Env& e);
