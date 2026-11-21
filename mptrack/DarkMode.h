/*
 * DarkMode.h
 * ----------
 * Purpose: Dark mode support for the OpenMPT user interface.
 * Notes  : Requires Windows 10 1809 or newer. On older systems, the light theme is always used.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#pragma once

#include "openmpt/all/BuildSettings.hpp"

OPENMPT_NAMESPACE_BEGIN

enum class DarkModeSetting : int32;

namespace DarkMode
{

// Must be called on the GUI thread before the main window is created.
void Initialize(DarkModeSetting setting);
void Shutdown();

// Returns true if dark mode can be used on this system.
bool IsSupported();
// Returns true if dark mode is currently active.
bool IsEnabled();

// Changes the user's preference and immediately re-themes all windows if the effective mode changes.
void SetSetting(DarkModeSetting setting);

// Forward WM_SETTINGCHANGE of the main window here, so that the "follow system" setting can react to changes of the Windows theme.
void OnSettingChange(UINT flags, LPCTSTR section);

// System colour that is not affected by dark mode.
COLORREF GetRealSysColor(int index);

// Never apply dark mode to this window or any of its children (e.g. third-party plugin GUIs).
void ExcludeWindow(HWND hwnd);

// Draw a status bar control in dark colours. For MFC status bars, which need to update their pane texts before painting.
void DrawStatusBar(HWND hwnd, HDC hdc);

}  // namespace DarkMode

OPENMPT_NAMESPACE_END
