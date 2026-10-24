// Mark-of-the-web removal for a release package. A zip downloaded through a
// browser carries a Zone.Identifier stream, and Explorer copies it onto every
// file it extracts; SmartScreen then blocks or warns on ApexSim.exe, the server
// and the DLLs they load. The launcher strips the streams on startup, as
// PowerShell's Unblock-File would.
//
// Only for a release package (Env::release): a source checkout is never touched.

#pragma once

#include <cstddef>
#include <filesystem>

struct UnblockResult
{
    size_t files = 0;    // regular files looked at
    size_t removed = 0;  // Zone.Identifier streams deleted
    size_t failed = 0;   // streams present that could not be deleted
};

// Walk every file under root (not following junctions or symlinks) and delete
// its Zone.Identifier stream where one is present.
UnblockResult UnblockTree(const std::filesystem::path& root);
