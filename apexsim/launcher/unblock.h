// Mark-of-the-web removal for a release package. A zip downloaded through a
// browser carries a Zone.Identifier stream, and Explorer copies it onto every
// file it extracts; SmartScreen then blocks or warns on ApexSim.exe, the server
// and the DLLs they load. The launcher strips the streams on startup, as
// PowerShell's Unblock-File would, from the binaries the package shipped and
// nothing else: content the player adds later keeps its mark.
//
// Only for a release package (Env::release): a source checkout is never touched.

#pragma once

#include <cstddef>
#include <filesystem>

struct UnblockResult
{
    size_t listed = 0;   // files the manifest names that are present
    size_t removed = 0;  // Zone.Identifier streams deleted
    size_t failed = 0;   // streams present that could not be deleted
};

// The manifest build_release.ps1 writes at the package root: one path per line,
// relative to the root, for every .exe and .dll in the package.
inline const wchar_t* kUnblockManifest = L"unblock.txt";

// Delete the Zone.Identifier stream of every file the manifest under root names.
// A missing manifest unblocks nothing; a line that is absolute or climbs out of
// root with .. is skipped.
UnblockResult UnblockListed(const std::filesystem::path& root);
