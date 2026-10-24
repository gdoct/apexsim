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
#include <string>

#include "unblock.h"

static const wchar_t* kZoneStream = L":Zone.Identifier";

static bool HasZoneStream(const std::wstring& stream)
{
    return GetFileAttributesW(stream.c_str()) != INVALID_FILE_ATTRIBUTES;
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

static void Walk(const std::wstring& dir, UnblockResult& r)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        // A junction or symlink may point outside the package: leave it alone.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring path = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            Walk(path, r);
            continue;
        }
        ++r.files;
        std::wstring stream = path + kZoneStream;
        if (!HasZoneStream(stream)) continue;
        if (RemoveZoneStream(path, stream, fd.dwFileAttributes)) ++r.removed;
        else ++r.failed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

UnblockResult UnblockTree(const std::filesystem::path& root)
{
    UnblockResult r;
    std::wstring dir = root.wstring();
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (!dir.empty()) Walk(dir, r);
    return r;
}
