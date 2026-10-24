// Win32 helpers for the launcher's secondary windows (Edit configuration,
// Manage content): child controls, fonts, the owned dialog window that
// disables the main one while it is open, and the shell's file pickers.

#pragma once

#include "text.h"

#include <filesystem>

HFONT MakeUiFont(int dpi);
void CenterOn(HWND w, HWND owner);

// A child control at 96-dpi coordinates scaled to dpi.
HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int dpi);
void SetFontAll(HWND parent, HFONT f);

// A captioned window of class cls, w x h at 96 dpi, centred on owner, which is
// disabled until CloseDialogWindow. Only one is open at a time.
HWND CreateDialogWindow(HWND owner, const wchar_t* cls, const wchar_t* title, int w, int h, int dpi);
void CloseDialogWindow(HWND hw);
// The open dialog window, for IsDialogMessage in the message loop; null when none.
HWND ActiveDialog();

// The shell's folder and file pickers; empty when cancelled.
std::filesystem::path PickFolder(HWND owner, const wchar_t* title);
std::filesystem::path PickFile(HWND owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec);
