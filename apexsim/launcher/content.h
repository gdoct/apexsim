// The content an install holds and the Manage content window: tracks, cars,
// HUD components and wheels listed by tab, the folder a player drops their own
// into, the Assetto Corsa import (ac_import.h) and installing a content pack.

#pragma once

#include "env.h"
#include "text.h"

#include <vector>

struct Item { std::wstring name, origin; };

enum Tab { TabTracks, TabCars, TabHud, TabWheels, TabCount };

// What a tab lists, sorted by name.
std::vector<Item> ListContent(const Env& e, int tab);

// Where "Open folder" goes: the folder a player drops their own content into.
std::filesystem::path TabFolder(const Env& e, int tab);

// The window class the Manage content window is made of; register it under
// kContentWindowClass before OpenContentWindow.
extern const wchar_t* kContentWindowClass;
LRESULT CALLBACK ContentProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);

// Opens Manage content over owner, listing env (which must outlive the window).
void OpenContentWindow(HWND owner, const Env& env, int dpi);
