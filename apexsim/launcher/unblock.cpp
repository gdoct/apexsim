// See unblock.h.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <string>

#include "unblock.h"

namespace fs = std::filesystem;

static const wchar_t* kZoneStream = L":Zone.Identifier";

static std::wstring WidenUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// A manifest line as a path under root, or empty when it is blank, absolute or
// climbs out with "..".
static fs::path ListedPath(const fs::path& root, std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line.erase(0, 3);  // a BOM, should the manifest ever gain one
    if (line.empty()) return {};
    fs::path rel = fs::path(WidenUtf8(line)).lexically_normal();
    if (rel.empty() || rel.has_root_name() || rel.has_root_directory()) return {};
    if (!rel.empty() && *rel.begin() == L"..") return {};
    return root / rel;
}

// Deletes file's Zone.Identifier stream. A read-only file refuses a stream
// delete, so the attribute is cleared for the delete and put back after it.
static bool RemoveZoneStream(const std::wstring& file, const std::wstring& stream, DWORD attrs)
{
    if (DeleteFileW(stream.c_str())) return true;
    if (GetLastError() != ERROR_ACCESS_DENIED || !(attrs & FILE_ATTRIBUTE_READONLY)) return false;
    if (!SetFileAttributesW(file.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) return false;
    bool ok = DeleteFileW(stream.c_str()) != 0;
    SetFileAttributesW(file.c_str(), attrs);
    return ok;
}

UnblockResult UnblockListed(const fs::path& root)
{
    UnblockResult r;
    std::ifstream f(root / kUnblockManifest);
    std::string line;
    while (f && std::getline(f, line))
    {
        fs::path p = ListedPath(root, line);
        if (p.empty()) continue;
        std::wstring file = p.wstring();
        DWORD attrs = GetFileAttributesW(file.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        ++r.listed;
        std::wstring stream = file + kZoneStream;
        if (GetFileAttributesW(stream.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (RemoveZoneStream(file, stream, attrs)) ++r.removed;
        else ++r.failed;
    }
    return r;
}
