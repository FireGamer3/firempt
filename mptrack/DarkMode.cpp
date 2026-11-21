/*
 * DarkMode.cpp
 * ------------
 * Purpose: Dark mode support for the OpenMPT user interface.
 * Notes  : Windows has no public dark mode API for classic Win32 applications. This implementation relies on:
 *          - undocumented uxtheme.dll exports (available since Windows 10 1809) for dark popup menus, scroll bars and title bars,
 *          - the "DarkMode_*" visual style classes for controls that have a dark variant (buttons, edits, combo boxes, lists, ...),
 *          - custom drawing for controls without one (check boxes, radio buttons, group boxes, tabs, status bars, toolbars, menu bar),
 *          - remapping GetSysColor / GetSysColorBrush in the import address tables of OpenMPT and MFC,
 *            so that all custom-drawn OpenMPT views and MFC control bars pick up the dark palette automatically.
 *          Windows are themed when they are created (through a CBT hook) and all existing windows are re-themed when the mode changes.
 *          When dark mode is disabled, none of this machinery is active.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#include "stdafx.h"
#include "DarkMode.h"
#include "HighDPISupport.h"
#include "TrackerSettings.h"
#include "../misc/mptOS.h"
#include "mpt/osinfo/windows_version.hpp"

#include <richedit.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <array>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <vector>


OPENMPT_NAMESPACE_BEGIN


#if MPT_WINNT_AT_LEAST(MPT_WIN_VISTA)


namespace DarkMode
{

namespace
{

//////////////////////////////////////////////////////////////////////////
// Palette

constexpr COLORREF ColorBackground    = RGB(0x20, 0x20, 0x20);  // Dialogs, control bars, menu bar
constexpr COLORREF ColorWindow        = RGB(0x2B, 0x2B, 0x2B);  // Edit fields, lists, trees
constexpr COLORREF ColorWorkspace     = RGB(0x18, 0x18, 0x18);  // MDI client area
constexpr COLORREF ColorText          = RGB(0xE0, 0xE0, 0xE0);
constexpr COLORREF ColorDimText       = RGB(0xA8, 0xA8, 0xA8);  // Inactive tabs, inactive menu bar
constexpr COLORREF ColorDisabledText  = RGB(0x80, 0x80, 0x80);
constexpr COLORREF ColorEdge          = RGB(0x55, 0x55, 0x55);  // Borders and separators
constexpr COLORREF ColorHot           = RGB(0x3A, 0x3A, 0x3A);  // Item under the mouse cursor
constexpr COLORREF ColorPressed       = RGB(0x4A, 0x4A, 0x4A);  // Pressed or checked items
constexpr COLORREF ColorPressedHot    = RGB(0x56, 0x56, 0x56);
constexpr COLORREF ColorTabInactive   = RGB(0x1A, 0x1A, 0x1A);
constexpr COLORREF ColorHighlight     = RGB(0x00, 0x5A, 0x9E);
constexpr COLORREF ColorHighlightText = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF ColorLink          = RGB(0x66, 0xB2, 0xFF);


// Dark replacements for system colours used by OpenMPT and MFC drawing code.
std::optional<COLORREF> GetDarkSysColor(int index) noexcept
{
	switch(index)
	{
	case COLOR_BTNFACE:
	case COLOR_MENU:
	case COLOR_MENUBAR:
	case COLOR_ACTIVEBORDER:
	case COLOR_INACTIVEBORDER:
		return ColorBackground;
	case COLOR_WINDOW:
	case COLOR_INFOBK:
	case COLOR_SCROLLBAR:
		return ColorWindow;
	case COLOR_APPWORKSPACE:
		return ColorWorkspace;
	case COLOR_WINDOWTEXT:
	case COLOR_BTNTEXT:
	case COLOR_MENUTEXT:
	case COLOR_INFOTEXT:
	case COLOR_CAPTIONTEXT:
		return ColorText;
	case COLOR_GRAYTEXT:
	case COLOR_INACTIVECAPTIONTEXT:
		return ColorDisabledText;
	case COLOR_HIGHLIGHT:
		return ColorHighlight;
	case COLOR_HIGHLIGHTTEXT:
		return ColorHighlightText;
	case COLOR_MENUHILIGHT:
		return ColorHot;
	case COLOR_HOTLIGHT:
		return ColorLink;
	case COLOR_BTNSHADOW:
		return RGB(0x48, 0x48, 0x48);
	case COLOR_BTNHIGHLIGHT:
		return RGB(0x5C, 0x5C, 0x5C);
	case COLOR_3DDKSHADOW:
		return RGB(0x10, 0x10, 0x10);
	case COLOR_3DLIGHT:
		return RGB(0x38, 0x38, 0x38);
	case COLOR_WINDOWFRAME:
		return ColorEdge;
	default:
		return std::nullopt;
	}
}


//////////////////////////////////////////////////////////////////////////
// Undocumented API

enum class PreferredAppMode : int
{
	Default = 0,
	AllowDark = 1,
	ForceDark = 2,
	ForceLight = 3,
};

using SetPreferredAppModeFunc = PreferredAppMode(WINAPI *)(PreferredAppMode mode);         // uxtheme.dll #135, Windows 10 1903+
using AllowDarkModeForAppFunc = bool(WINAPI *)(bool allow);                                // uxtheme.dll #135, Windows 10 1809
using AllowDarkModeForWindowFunc = bool(WINAPI *)(HWND hwnd, bool allow);                  // uxtheme.dll #133
using FlushMenuThemesFunc = void(WINAPI *)();                                              // uxtheme.dll #136
using RefreshImmersiveColorPolicyStateFunc = void(WINAPI *)();                             // uxtheme.dll #104
using DwmSetWindowAttributeFunc = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

struct API
{
	SetPreferredAppModeFunc SetPreferredAppMode = nullptr;
	AllowDarkModeForAppFunc AllowDarkModeForApp = nullptr;
	AllowDarkModeForWindowFunc AllowDarkModeForWindow = nullptr;
	FlushMenuThemesFunc FlushMenuThemes = nullptr;
	RefreshImmersiveColorPolicyStateFunc RefreshImmersiveColorPolicyState = nullptr;
	DwmSetWindowAttributeFunc DwmSetWindowAttribute = nullptr;
	DWORD dwmDarkModeAttribute = 0;
	bool supported = false;
};

// Undocumented messages for drawing the menu bar
constexpr UINT WM_UAH_DRAWMENU = 0x0091;
constexpr UINT WM_UAH_DRAWMENUITEM = 0x0092;

struct UAHMENU
{
	HMENU hmenu;
	HDC hdc;
	DWORD dwFlags;
};

union UAHMENUITEMMETRICS
{
	struct
	{
		DWORD cx;
		DWORD cy;
	} rgsizeBar[2];
	struct
	{
		DWORD cx;
		DWORD cy;
	} rgsizePopup[4];
};

struct UAHMENUPOPUPMETRICS
{
	DWORD rgcx[4];
	DWORD fUpdateMaxWidths : 2;
};

struct UAHMENUITEM
{
	int iPosition;
	UAHMENUITEMMETRICS umim;
	UAHMENUPOPUPMETRICS umpm;
};

struct UAHDRAWMENUITEM
{
	DRAWITEMSTRUCT dis;
	UAHMENU um;
	UAHMENUITEM umi;
};


//////////////////////////////////////////////////////////////////////////
// State

API g_api;
DarkModeSetting g_setting = DarkModeSetting::Light;
std::atomic<bool> g_enabled = false;
HHOOK g_cbtHook = nullptr;
ATOM g_excludeAtom = 0;
bool g_sysColorHooksInstalled = false;

using GetSysColorFunc = DWORD(WINAPI *)(int);
using GetSysColorBrushFunc = HBRUSH(WINAPI *)(int);
using DrawEdgeFunc = BOOL(WINAPI *)(HDC, LPRECT, UINT, UINT);
using DrawFrameControlFunc = BOOL(WINAPI *)(HDC, LPRECT, UINT, UINT);
GetSysColorFunc g_realGetSysColor = nullptr;
GetSysColorBrushFunc g_realGetSysColorBrush = nullptr;
DrawEdgeFunc g_realDrawEdge = nullptr;
DrawFrameControlFunc g_realDrawFrameControl = nullptr;
std::array<HBRUSH, COLOR_MENUBAR + 1> g_darkSysColorBrushes{};

std::unordered_map<COLORREF, HBRUSH> g_brushes;  // Only accessed from the GUI thread
std::unordered_map<HWND, int> g_hotTabs;         // Tab controls: index of the tab under the mouse cursor

constexpr UINT_PTR SubclassID = 0x4F4D5054;


enum class Kind : DWORD_PTR
{
	Generic = 0,
	PendingDialog,  // Top-level dialog; decided in WM_INITDIALOG whether it can be themed (shell dialogs cannot)
	Ignore,         // Excluded from theming
	Button,
	Edit,
	ComboBox,
	ComboBoxEx,
	ListBox,
	ListView,
	TreeView,
	Header,
	Tab,
	StatusBar,
	Toolbar,
	Tooltip,
	ScrollBar,
	UpDown,
	Trackbar,
	RichEdit,
	Static,
	MDIClient,
	Progress,
};


HBRUSH GetBrush(COLORREF color)
{
	auto it = g_brushes.find(color);
	if(it != g_brushes.end())
		return it->second;
	HBRUSH brush = CreateSolidBrush(color);
	g_brushes[color] = brush;
	return brush;
}


//////////////////////////////////////////////////////////////////////////
// System colour remapping

DWORD WINAPI HookedGetSysColor(int index)
{
	if(g_enabled.load(std::memory_order_relaxed))
	{
		if(const auto color = GetDarkSysColor(index))
			return *color;
	}
	return g_realGetSysColor(index);
}


HBRUSH WINAPI HookedGetSysColorBrush(int index)
{
	if(g_enabled.load(std::memory_order_relaxed) && index >= 0 && static_cast<size_t>(index) < g_darkSysColorBrushes.size() && g_darkSysColorBrushes[index])
		return g_darkSysColorBrushes[index];
	return g_realGetSysColorBrush(index);
}


// DrawEdge and DrawFrameControl use the real system colours internally.

void FillSolidThreadSafe(HDC dc, const RECT &rect, COLORREF color)
{
	const COLORREF oldColor = SetDCBrushColor(dc, color);
	FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
	SetDCBrushColor(dc, oldColor);
}


// Draws one ring of a 3D edge and shrinks the rectangle on the drawn sides.
void DrawEdgeRing(HDC dc, RECT &rect, UINT flags, COLORREF topLeft, COLORREF bottomRight)
{
	if(flags & BF_LEFT)
		FillSolidThreadSafe(dc, RECT{rect.left, rect.top, rect.left + 1, rect.bottom}, topLeft);
	if(flags & BF_TOP)
		FillSolidThreadSafe(dc, RECT{rect.left, rect.top, rect.right, rect.top + 1}, topLeft);
	if(flags & BF_RIGHT)
		FillSolidThreadSafe(dc, RECT{rect.right - 1, rect.top, rect.right, rect.bottom}, bottomRight);
	if(flags & BF_BOTTOM)
		FillSolidThreadSafe(dc, RECT{rect.left, rect.bottom - 1, rect.right, rect.bottom}, bottomRight);
	if(flags & BF_LEFT)
		rect.left++;
	if(flags & BF_TOP)
		rect.top++;
	if(flags & BF_RIGHT)
		rect.right--;
	if(flags & BF_BOTTOM)
		rect.bottom--;
}


BOOL WINAPI HookedDrawEdge(HDC dc, LPRECT rect, UINT edge, UINT flags)
{
	if(!g_enabled.load(std::memory_order_relaxed) || !rect || (flags & BF_DIAGONAL))
		return g_realDrawEdge(dc, rect, edge, flags);

	const COLORREF light = *GetDarkSysColor(COLOR_3DLIGHT), highlight = *GetDarkSysColor(COLOR_BTNHIGHLIGHT);
	const COLORREF shadow = *GetDarkSysColor(COLOR_BTNSHADOW), darkShadow = *GetDarkSysColor(COLOR_3DDKSHADOW);
	RECT rc = *rect;
	const auto drawRing = [&](COLORREF topLeft, COLORREF bottomRight)
	{
		if(flags & BF_MONO)
			DrawEdgeRing(dc, rc, flags, ColorEdge, ColorEdge);
		else if(flags & BF_FLAT)
			DrawEdgeRing(dc, rc, flags, shadow, shadow);
		else
			DrawEdgeRing(dc, rc, flags, topLeft, bottomRight);
	};
	if(edge & BDR_RAISEDOUTER)
		drawRing(light, darkShadow);
	else if(edge & BDR_SUNKENOUTER)
		drawRing(shadow, highlight);
	if(edge & BDR_RAISEDINNER)
		drawRing(highlight, shadow);
	else if(edge & BDR_SUNKENINNER)
		drawRing(darkShadow, light);

	if((flags & BF_MIDDLE) && rc.right > rc.left && rc.bottom > rc.top)
		FillSolidThreadSafe(dc, rc, (flags & (BF_MONO | BF_FLAT)) ? ColorWindow : ColorBackground);
	if(flags & BF_ADJUST)
		*rect = rc;
	return TRUE;
}


BOOL WINAPI HookedDrawFrameControl(HDC dc, LPRECT rect, UINT type, UINT state)
{
	// Only push buttons are drawn by OpenMPT
	if(!g_enabled.load(std::memory_order_relaxed) || !rect || type != DFC_BUTTON || (state & 0x1F) != DFCS_BUTTONPUSH)
		return g_realDrawFrameControl(dc, rect, type, state);
	RECT rc = *rect;
	const bool pushed = (state & (DFCS_PUSHED | DFCS_CHECKED)) != 0;
	const UINT edge = (state & DFCS_FLAT) ? BDR_SUNKENOUTER : (pushed ? EDGE_SUNKEN : EDGE_RAISED);
	HookedDrawEdge(dc, &rc, edge, BF_RECT | BF_MIDDLE | BF_ADJUST | ((state & DFCS_FLAT) ? BF_FLAT : 0));
	if(state & DFCS_ADJUSTRECT)
		*rect = rc;
	return TRUE;
}


// Replace all references to a function in a module's import address table.
void PatchImportAddressTable(HMODULE module, const void *original, const void *replacement)
{
	if(!module || !original)
		return;
	auto base = reinterpret_cast<uint8 *>(module);
	const auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if(dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	const auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dosHeader->e_lfanew);
	if(ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		return;
	const IMAGE_DATA_DIRECTORY &importDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if(!importDir.VirtualAddress || !importDir.Size)
		return;

	for(auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + importDir.VirtualAddress); descriptor->Name; descriptor++)
	{
		for(auto thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk); thunk->u1.Function; thunk++)
		{
			if(reinterpret_cast<const void *>(thunk->u1.Function) != original)
				continue;
			DWORD oldProtect = 0;
			if(VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &oldProtect))
			{
				thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
				VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &oldProtect);
			}
		}
	}
}


void InstallSysColorHooks()
{
	if(g_sysColorHooksInstalled)
		return;
	g_sysColorHooksInstalled = true;

	for(size_t i = 0; i < g_darkSysColorBrushes.size(); i++)
	{
		if(const auto color = GetDarkSysColor(static_cast<int>(i)))
			g_darkSysColorBrushes[i] = CreateSolidBrush(*color);
	}

	// OpenMPT itself and statically linked MFC, or the MFC DLL in shared builds
	const HMODULE modules[] =
	{
		GetModuleHandle(nullptr),
		GetModuleHandle(_T("mfc140u.dll")),
		GetModuleHandle(_T("mfc140ud.dll")),
		GetModuleHandle(_T("mfc140.dll")),
		GetModuleHandle(_T("mfc140d.dll")),
	};
	for(HMODULE module : modules)
	{
		PatchImportAddressTable(module, reinterpret_cast<const void *>(g_realGetSysColor), reinterpret_cast<const void *>(&HookedGetSysColor));
		PatchImportAddressTable(module, reinterpret_cast<const void *>(g_realGetSysColorBrush), reinterpret_cast<const void *>(&HookedGetSysColorBrush));
		PatchImportAddressTable(module, reinterpret_cast<const void *>(g_realDrawEdge), reinterpret_cast<const void *>(&HookedDrawEdge));
		PatchImportAddressTable(module, reinterpret_cast<const void *>(g_realDrawFrameControl), reinterpret_cast<const void *>(&HookedDrawFrameControl));
	}
}


bool IsDefaultControlBrush(HBRUSH brush)
{
	if(!brush)
		return true;
	const auto value = reinterpret_cast<uintptr_t>(brush);
	if(value <= COLOR_MENUBAR + 1)
		return true;
	for(int index : {COLOR_BTNFACE, COLOR_WINDOW, COLOR_SCROLLBAR, COLOR_MENU, COLOR_3DLIGHT})
	{
		if(brush == g_realGetSysColorBrush(index) || brush == g_darkSysColorBrushes[index])
			return true;
	}
	return false;
}


// Returns the system colour index if the brush is a system colour brush that has a dark replacement.
std::optional<int> GetSysColorIndexOfBrush(HBRUSH brush)
{
	if(!brush)
		return std::nullopt;
	const auto value = reinterpret_cast<uintptr_t>(brush);
	if(value <= g_darkSysColorBrushes.size())
	{
		const int index = static_cast<int>(value - 1);
		if(g_darkSysColorBrushes[index])
			return index;
		return std::nullopt;
	}
	for(size_t i = 0; i < g_darkSysColorBrushes.size(); i++)
	{
		if(g_darkSysColorBrushes[i] && brush == g_realGetSysColorBrush(static_cast<int>(i)))
			return static_cast<int>(i);
	}
	return std::nullopt;
}


//////////////////////////////////////////////////////////////////////////
// System state

bool IsHighContrast()
{
	HIGHCONTRAST hc{};
	hc.cbSize = sizeof(hc);
	return SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}


bool SystemUsesDarkMode()
{
	HKEY key = nullptr;
	if(RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return false;
	DWORD value = 1, size = sizeof(value), type = 0;
	const bool ok = RegQueryValueEx(key, _T("AppsUseLightTheme"), nullptr, &type, reinterpret_cast<BYTE *>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD;
	RegCloseKey(key);
	return ok && value == 0;
}


bool ShouldUseDarkMode()
{
	if(!g_api.supported || IsHighContrast())
		return false;
	switch(g_setting)
	{
	case DarkModeSetting::Dark:
		return true;
	case DarkModeSetting::System:
		return SystemUsesDarkMode();
	default:
		return false;
	}
}


template<typename T>
T GetProcAddressAs(HMODULE module, LPCSTR name)
{
	return reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(module, name)));
}


void LoadAPI()
{
	if(mpt::OS::Windows::IsWine())
		return;
	const auto version = mpt::osinfo::windows::Version::Current();
	if(!version.IsAtLeast(mpt::osinfo::windows::Version::Win10, 17763))
		return;

	HMODULE uxtheme = LoadLibraryEx(_T("uxtheme.dll"), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if(!uxtheme)
		return;
	g_api.RefreshImmersiveColorPolicyState = GetProcAddressAs<RefreshImmersiveColorPolicyStateFunc>(uxtheme, MAKEINTRESOURCEA(104));
	g_api.AllowDarkModeForWindow = GetProcAddressAs<AllowDarkModeForWindowFunc>(uxtheme, MAKEINTRESOURCEA(133));
	if(version.IsAtLeast(mpt::osinfo::windows::Version::Win10, 18362))
		g_api.SetPreferredAppMode = GetProcAddressAs<SetPreferredAppModeFunc>(uxtheme, MAKEINTRESOURCEA(135));
	else
		g_api.AllowDarkModeForApp = GetProcAddressAs<AllowDarkModeForAppFunc>(uxtheme, MAKEINTRESOURCEA(135));
	g_api.FlushMenuThemes = GetProcAddressAs<FlushMenuThemesFunc>(uxtheme, MAKEINTRESOURCEA(136));

	if(HMODULE dwmapi = LoadLibraryEx(_T("dwmapi.dll"), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
		g_api.DwmSetWindowAttribute = GetProcAddressAs<DwmSetWindowAttributeFunc>(dwmapi, "DwmSetWindowAttribute");
	// DWMWA_USE_IMMERSIVE_DARK_MODE, which had a different value before Windows 10 20H1
	g_api.dwmDarkModeAttribute = version.IsAtLeast(mpt::osinfo::windows::Version::Win10, 18985) ? 20 : 19;

	g_api.supported = g_api.AllowDarkModeForWindow && (g_api.SetPreferredAppMode || g_api.AllowDarkModeForApp);
}


void SetAppMode(bool dark)
{
	if(g_api.SetPreferredAppMode)
		g_api.SetPreferredAppMode(dark ? PreferredAppMode::ForceDark : PreferredAppMode::Default);
	else if(g_api.AllowDarkModeForApp)
		g_api.AllowDarkModeForApp(dark);
	if(g_api.RefreshImmersiveColorPolicyState)
		g_api.RefreshImmersiveColorPolicyState();
	if(g_api.FlushMenuThemes)
		g_api.FlushMenuThemes();
}


//////////////////////////////////////////////////////////////////////////
// Window classification

LPCTSTR ExcludeProp()
{
	return MAKEINTATOM(g_excludeAtom);
}


bool IsDialogClassName(LPCTSTR className)
{
	return !_tcscmp(className, _T("#32770"));
}


Kind ClassifyClassName(LPCTSTR className)
{
	static constexpr std::pair<LPCTSTR, Kind> Classes[] =
	{
		{_T("Button"),             Kind::Button},
		{_T("Edit"),               Kind::Edit},
		{_T("ComboBox"),           Kind::ComboBox},
		{_T("ComboBoxEx32"),       Kind::ComboBoxEx},
		{_T("ListBox"),            Kind::ListBox},
		{_T("ComboLBox"),          Kind::ListBox},
		{_T("SysListView32"),      Kind::ListView},
		{_T("SysTreeView32"),      Kind::TreeView},
		{_T("SysHeader32"),        Kind::Header},
		{_T("SysTabControl32"),    Kind::Tab},
		{_T("msctls_statusbar32"), Kind::StatusBar},
		{_T("ToolbarWindow32"),    Kind::Toolbar},
		{_T("tooltips_class32"),   Kind::Tooltip},
		{_T("ScrollBar"),          Kind::ScrollBar},
		{_T("msctls_updown32"),    Kind::UpDown},
		{_T("msctls_trackbar32"),  Kind::Trackbar},
		{_T("msctls_progress32"),  Kind::Progress},
		{_T("RichEdit20A"),        Kind::RichEdit},
		{_T("RichEdit20W"),        Kind::RichEdit},
		{_T("RICHEDIT50W"),        Kind::RichEdit},
		{_T("Static"),             Kind::Static},
		{_T("SysLink"),            Kind::Static},
		{_T("MDIClient"),          Kind::MDIClient},
		// Windows that we must not touch: menus, input methods, shell views (file dialogs), drag images
		{_T("#32768"),                Kind::Ignore},
		{_T("IME"),                   Kind::Ignore},
		{_T("MSCTFIME UI"),           Kind::Ignore},
		{_T("DirectUIHWND"),          Kind::Ignore},
		{_T("DUIViewWndClassName"),   Kind::Ignore},
		{_T("SHELLDLL_DefView"),      Kind::Ignore},
		{_T("NamespaceTreeControl"),  Kind::Ignore},
		{_T("CtrlNotifySink"),        Kind::Ignore},
		{_T("ReBarWindow32"),         Kind::Ignore},
		{_T("Breadcrumb Parent"),     Kind::Ignore},
		{_T("Auto-Suggest Dropdown"), Kind::Ignore},
		{_T("SysDragImage"),          Kind::Ignore},
	};
	for(const auto &[name, kind] : Classes)
	{
		if(!_tcsicmp(className, name))
			return kind;
	}
	return Kind::Generic;
}


Kind ClassifyWindow(HWND hwnd)
{
	TCHAR className[64] = {};
	if(!GetClassName(hwnd, className, static_cast<int>(std::size(className))))
		return Kind::Ignore;
	return ClassifyClassName(className);
}


LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData);


std::optional<Kind> GetAttachedKind(HWND hwnd)
{
	DWORD_PTR refData = 0;
	if(!GetWindowSubclass(hwnd, SubclassProc, SubclassID, &refData))
		return std::nullopt;
	return static_cast<Kind>(refData);
}


// Is this window or any of its parents / owners excluded from theming?
bool IsExcluded(HWND hwnd)
{
	for(HWND wnd = hwnd; wnd; wnd = ::GetParent(wnd))
	{
		if(GetProp(wnd, ExcludeProp()))
			return true;
	}
	return false;
}


// Was this window class registered by OpenMPT (or MFC on behalf of OpenMPT)?
bool IsOwnWindowClass(HWND hwnd)
{
	return reinterpret_cast<HINSTANCE>(GetClassLongPtr(hwnd, GCLP_HMODULE)) == AfxGetInstanceHandle();
}


BOOL CALLBACK FindShellViewProc(HWND hwnd, LPARAM lParam)
{
	TCHAR className[64] = {};
	GetClassName(hwnd, className, static_cast<int>(std::size(className)));
	for(LPCTSTR shellClass : {_T("DirectUIHWND"), _T("DUIViewWndClassName"), _T("SHELLDLL_DefView"), _T("NamespaceTreeControl")})
	{
		if(!_tcsicmp(className, shellClass))
		{
			*reinterpret_cast<bool *>(lParam) = true;
			return FALSE;
		}
	}
	return TRUE;
}


// Top-level dialog not created by OpenMPT / MFC, such as a message box.
bool IsForeignDialog(HWND hwnd)
{
	if(GetWindowStyle(hwnd) & WS_CHILD)
		return false;
	TCHAR className[64] = {};
	GetClassName(hwnd, className, static_cast<int>(std::size(className)));
	return IsDialogClassName(className) && !CWnd::FromHandlePermanent(hwnd);
}


// File dialogs and task dialogs render themselves and must not be touched.
bool IsShellDialog(HWND hwnd)
{
	bool found = false;
	EnumChildWindows(hwnd, FindShellViewProc, reinterpret_cast<LPARAM>(&found));
	return found;
}


//////////////////////////////////////////////////////////////////////////
// Applying and removing themes

void SetTitleBarTheme(HWND hwnd, bool dark)
{
	if(!g_api.DwmSetWindowAttribute)
		return;
	const BOOL value = dark ? TRUE : FALSE;
	g_api.DwmSetWindowAttribute(hwnd, g_api.dwmDarkModeAttribute, &value, sizeof(value));
}


void ApplyVisualStyle(HWND hwnd, Kind kind, bool dark)
{
	g_api.AllowDarkModeForWindow(hwnd, dark);

	const wchar_t *theme = nullptr;
	switch(kind)
	{
	case Kind::ListView:
		// Same as Explorer's file list; unlike DarkMode_Explorer, this also has readable group headers.
		theme = L"DarkMode_ItemsView";
		break;
	case Kind::Generic:
	case Kind::MDIClient:
	case Kind::Button:
	case Kind::ListBox:
	case Kind::TreeView:
	case Kind::Tab:
	case Kind::Toolbar:
	case Kind::Tooltip:
	case Kind::ScrollBar:
	case Kind::UpDown:
	case Kind::Trackbar:
	case Kind::RichEdit:
		theme = L"DarkMode_Explorer";
		break;
	case Kind::Edit:
		theme = (GetWindowStyle(hwnd) & (WS_VSCROLL | WS_HSCROLL)) ? L"DarkMode_Explorer" : L"DarkMode_CFD";
		break;
	case Kind::ComboBox:
		theme = L"DarkMode_CFD";
		break;
	case Kind::Header:
		theme = L"DarkMode_ItemsView";
		break;
	default:
		break;
	}
	if(theme)
		SetWindowTheme(hwnd, dark ? theme : nullptr, nullptr);
}


// Colours that can only be set after the control has been fully created.
void ApplyControlColors(HWND hwnd, Kind kind, bool dark)
{
	switch(kind)
	{
	case Kind::ListView:
		ListView_SetBkColor(hwnd, dark ? ColorWindow : g_realGetSysColor(COLOR_WINDOW));
		ListView_SetTextBkColor(hwnd, dark ? ColorWindow : g_realGetSysColor(COLOR_WINDOW));
		ListView_SetTextColor(hwnd, dark ? ColorText : g_realGetSysColor(COLOR_WINDOWTEXT));
		break;
	case Kind::TreeView:
		TreeView_SetBkColor(hwnd, dark ? ColorWindow : static_cast<COLORREF>(-1));
		TreeView_SetTextColor(hwnd, dark ? ColorText : static_cast<COLORREF>(-1));
		TreeView_SetLineColor(hwnd, dark ? ColorEdge : CLR_DEFAULT);
		break;
	case Kind::RichEdit:
		{
			SendMessage(hwnd, EM_SETBKGNDCOLOR, dark ? 0 : 1, ColorWindow);
			CHARFORMAT2 format{};
			format.cbSize = sizeof(format);
			format.dwMask = CFM_COLOR;
			format.dwEffects = dark ? 0 : CFE_AUTOCOLOR;
			format.crTextColor = ColorText;
			SendMessage(hwnd, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
		}
		break;
	case Kind::Generic:
		// MFC dock bars and dialog bars draw their background using the "Rebar" visual style, which has no dark variant.
		// Without visual styles, MFC falls back to its cached system colours instead.
		if(CWnd *wnd = CWnd::FromHandlePermanent(hwnd); dark && wnd && wnd->IsKindOf(RUNTIME_CLASS(CControlBar)))
			SetWindowTheme(hwnd, L"", L"");
		break;
	default:
		break;
	}

	if(!(GetWindowStyle(hwnd) & WS_CHILD) && kind != Kind::Tooltip && kind != Kind::ListBox)
		SetTitleBarTheme(hwnd, dark);
}


void AttachToWindow(HWND hwnd, Kind kind, bool alreadyCreated)
{
	SetWindowSubclass(hwnd, SubclassProc, SubclassID, static_cast<DWORD_PTR>(kind));
	if(kind == Kind::PendingDialog || kind == Kind::Ignore)
		return;
	ApplyVisualStyle(hwnd, kind, true);
	if(alreadyCreated)
		ApplyControlColors(hwnd, kind, true);
}


void DetachFromWindow(HWND hwnd)
{
	const auto kind = GetAttachedKind(hwnd);
	if(!kind || *kind == Kind::Ignore)
		return;
	RemoveWindowSubclass(hwnd, SubclassProc, SubclassID);
	g_hotTabs.erase(hwnd);
	if(*kind == Kind::PendingDialog)
		return;
	ApplyVisualStyle(hwnd, *kind, false);
	ApplyControlColors(hwnd, *kind, false);
}


void MarkExcluded(HWND hwnd)
{
	SetProp(hwnd, ExcludeProp(), reinterpret_cast<HANDLE>(1));
	// The subclass removes the property again when the window is destroyed.
	SetWindowSubclass(hwnd, SubclassProc, SubclassID, static_cast<DWORD_PTR>(Kind::Ignore));
}


BOOL CALLBACK AttachChildProc(HWND hwnd, LPARAM)
{
	if(GetAttachedKind(hwnd) || IsExcluded(hwnd))
		return TRUE;
	const Kind kind = ClassifyWindow(hwnd);
	if(kind != Kind::Ignore)
		AttachToWindow(hwnd, kind, true);
	return TRUE;
}


BOOL CALLBACK DetachChildProc(HWND hwnd, LPARAM)
{
	DetachFromWindow(hwnd);
	return TRUE;
}


BOOL CALLBACK AttachTopLevelProc(HWND hwnd, LPARAM)
{
	if(GetAttachedKind(hwnd) || IsExcluded(hwnd))
		return TRUE;
	TCHAR className[64] = {};
	GetClassName(hwnd, className, static_cast<int>(std::size(className)));
	Kind kind = ClassifyClassName(className);
	if(kind == Kind::Ignore)
		return TRUE;
	if(IsDialogClassName(className))
	{
		if(IsShellDialog(hwnd))
		{
			MarkExcluded(hwnd);
			return TRUE;
		}
	} else if(kind != Kind::Tooltip && kind != Kind::ListBox && !IsOwnWindowClass(hwnd))
	{
		return TRUE;
	}
	AttachToWindow(hwnd, kind, true);
	EnumChildWindows(hwnd, AttachChildProc, 0);
	return TRUE;
}


BOOL CALLBACK DetachTopLevelProc(HWND hwnd, LPARAM)
{
	DetachFromWindow(hwnd);
	EnumChildWindows(hwnd, DetachChildProc, 0);
	return TRUE;
}


BOOL CALLBACK RedrawTopLevelProc(HWND hwnd, LPARAM)
{
	if(IsWindowVisible(hwnd))
		RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
	return TRUE;
}


void CALLBACK DeferredRedrawProc(HWND, UINT, UINT_PTR id, DWORD)
{
	KillTimer(nullptr, id);
	EnumThreadWindows(GetCurrentThreadId(), RedrawTopLevelProc, 0);
}


void ResolvePendingDialog(HWND hwnd)
{
	if(IsShellDialog(hwnd))
	{
		MarkExcluded(hwnd);
		return;
	}
	AttachToWindow(hwnd, Kind::Generic, true);
	EnumChildWindows(hwnd, AttachChildProc, 0);
}


// Decide how a window that is about to be created should be themed.
Kind ClassifyNewWindow(HWND hwnd, const CREATESTRUCT &cs)
{
	if(cs.hwndParent == HWND_MESSAGE)
		return Kind::Ignore;
	TCHAR className[64] = {};
	GetClassName(hwnd, className, static_cast<int>(std::size(className)));
	const Kind kind = ClassifyClassName(className);
	if(kind == Kind::Ignore)
		return Kind::Ignore;
	if(cs.hwndParent && IsExcluded(cs.hwndParent))
		return Kind::Ignore;

	if(!(cs.style & WS_CHILD))
	{
		// Top-level windows: OpenMPT's own windows, dialogs (decided later), and some harmless system popups.
		if(kind == Kind::Tooltip || kind == Kind::ListBox)
			return kind;
		if(IsDialogClassName(className))
			return Kind::PendingDialog;
		if(IsOwnWindowClass(hwnd))
			return kind;
		return Kind::Ignore;
	}

	// Child windows are themed if their top-level window is themed.
	HWND topLevel = cs.hwndParent;
	while(topLevel && (GetWindowStyle(topLevel) & WS_CHILD))
		topLevel = ::GetParent(topLevel);
	if(!topLevel)
		return Kind::Ignore;
	const auto topLevelKind = GetAttachedKind(topLevel);
	if(!topLevelKind || *topLevelKind == Kind::PendingDialog || *topLevelKind == Kind::Ignore)
		return Kind::Ignore;
	return kind;
}


LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam)
{
	if(code == HCBT_CREATEWND && g_enabled)
	{
		const HWND hwnd = reinterpret_cast<HWND>(wParam);
		const CREATESTRUCT *cs = reinterpret_cast<const CBT_CREATEWND *>(lParam)->lpcs;
		if(cs)
		{
			const Kind kind = ClassifyNewWindow(hwnd, *cs);
			if(kind != Kind::Ignore)
				AttachToWindow(hwnd, kind, false);
		}
	}
	return CallNextHookEx(g_cbtHook, code, wParam, lParam);
}


//////////////////////////////////////////////////////////////////////////
// Painting helpers

template<typename Func>
void PaintBuffered(HWND hwnd, HDC targetDC, Func &&paint)
{
	RECT rect;
	GetClientRect(hwnd, &rect);
	const int width = rect.right - rect.left, height = rect.bottom - rect.top;
	if(width <= 0 || height <= 0)
		return;
	HDC memDC = CreateCompatibleDC(targetDC);
	HBITMAP bitmap = memDC ? CreateCompatibleBitmap(targetDC, width, height) : nullptr;
	if(!bitmap)
	{
		if(memDC)
			DeleteDC(memDC);
		paint(targetDC);
		return;
	}
	HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);
	paint(memDC);
	BitBlt(targetDC, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
	SelectObject(memDC, oldBitmap);
	DeleteObject(bitmap);
	DeleteDC(memDC);
}


class FontSelector
{
	HDC m_dc;
	HGDIOBJ m_oldFont = nullptr;
public:
	FontSelector(HDC dc, HWND hwnd)
		: m_dc{dc}
	{
		if(HFONT font = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0)))
			m_oldFont = SelectObject(dc, font);
	}
	~FontSelector()
	{
		if(m_oldFont)
			SelectObject(m_dc, m_oldFont);
	}
	FontSelector(const FontSelector &) = delete;
	FontSelector &operator=(const FontSelector &) = delete;
};


std::basic_string<TCHAR> GetText(HWND hwnd)
{
	std::basic_string<TCHAR> text(static_cast<size_t>(GetWindowTextLength(hwnd)) + 1, _T('\0'));
	text.resize(static_cast<size_t>(GetWindowText(hwnd, text.data(), static_cast<int>(text.size()))));
	return text;
}


void FillSolid(HDC dc, const RECT &rect, COLORREF color)
{
	FillRect(dc, &rect, GetBrush(color));
}


//////////////////////////////////////////////////////////////////////////
// Check boxes, radio buttons and group boxes
// The themed versions of these controls ignore WM_CTLCOLORSTATIC text colours, so their text would be unreadable.

bool IsCustomPaintButton(HWND hwnd)
{
	const DWORD style = GetWindowStyle(hwnd);
	if(style & (BS_PUSHLIKE | BS_BITMAP | BS_ICON))
		return false;
	switch(style & BS_TYPEMASK)
	{
	case BS_CHECKBOX:
	case BS_AUTOCHECKBOX:
	case BS_3STATE:
	case BS_AUTO3STATE:
	case BS_RADIOBUTTON:
	case BS_AUTORADIOBUTTON:
	case BS_GROUPBOX:
		return true;
	default:
		return false;
	}
}


bool IsGroupBox(HWND hwnd)
{
	return (GetWindowStyle(hwnd) & BS_TYPEMASK) == BS_GROUPBOX;
}


HBRUSH GetParentBackgroundBrush(HWND hwnd, HDC dc)
{
	HBRUSH brush = reinterpret_cast<HBRUSH>(SendMessage(::GetParent(hwnd), WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(hwnd)));
	return brush ? brush : GetBrush(ColorBackground);
}


UINT GetPrefixFlags(HWND hwnd)
{
	return (SendMessage(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
}


bool ShouldDrawFocus(HWND hwnd)
{
	return GetFocus() == hwnd && !(SendMessage(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}


void PaintCheckBox(HWND hwnd, HDC dc)
{
	const DWORD style = GetWindowStyle(hwnd);
	const DWORD type = style & BS_TYPEMASK;
	const bool radio = (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON);
	const bool enabled = IsWindowEnabled(hwnd) != FALSE;
	const UINT dpi = HighDPISupport::GetDpiForWindow(hwnd);

	RECT client;
	GetClientRect(hwnd, &client);
	FillRect(dc, &client, GetParentBackgroundBrush(hwnd, dc));

	FontSelector font{dc, hwnd};

	const LRESULT checkState = SendMessage(hwnd, BM_GETCHECK, 0, 0);
	const LRESULT buttonState = SendMessage(hwnd, BM_GETSTATE, 0, 0);
	int partState;
	if(radio)
		partState = (checkState == BST_CHECKED) ? RBS_CHECKEDNORMAL : RBS_UNCHECKEDNORMAL;
	else if(checkState == BST_CHECKED)
		partState = CBS_CHECKEDNORMAL;
	else if(checkState == BST_INDETERMINATE)
		partState = CBS_MIXEDNORMAL;
	else
		partState = CBS_UNCHECKEDNORMAL;
	if(!enabled)
		partState += 3;
	else if(buttonState & BST_PUSHED)
		partState += 2;
	else if(buttonState & BST_HOT)
		partState += 1;
	const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;

	HTHEME theme = OpenThemeData(hwnd, L"Button");
	SIZE glyphSize{MulDiv(13, dpi, 96), MulDiv(13, dpi, 96)};
	if(theme)
		GetThemePartSize(theme, dc, part, partState, nullptr, TS_DRAW, &glyphSize);

	const bool glyphOnRight = (style & BS_LEFTTEXT) || (GetWindowExStyle(hwnd) & WS_EX_RIGHT);
	RECT glyphRect;
	glyphRect.top = client.top + (client.bottom - client.top - glyphSize.cy) / 2;
	glyphRect.bottom = glyphRect.top + glyphSize.cy;
	glyphRect.left = glyphOnRight ? (client.right - glyphSize.cx) : client.left;
	glyphRect.right = glyphRect.left + glyphSize.cx;
	if(theme)
	{
		DrawThemeBackground(theme, dc, part, partState, &glyphRect, nullptr);
		CloseThemeData(theme);
	} else
	{
		UINT frameState = (radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK) | (checkState ? DFCS_CHECKED : 0) | (enabled ? 0 : DFCS_INACTIVE);
		DrawFrameControl(dc, &glyphRect, DFC_BUTTON, frameState);
	}

	const auto text = GetText(hwnd);
	if(text.empty())
		return;

	const int gap = MulDiv(4, dpi, 96);
	RECT textRect = client;
	if(glyphOnRight)
		textRect.right = glyphRect.left - gap;
	else
		textRect.left = glyphRect.right + gap;

	UINT flags = GetPrefixFlags(hwnd);
	switch(style & BS_CENTER)
	{
	case BS_CENTER: flags |= DT_CENTER; break;
	case BS_RIGHT: flags |= DT_RIGHT; break;
	default: flags |= DT_LEFT; break;
	}
	flags |= (style & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE;

	// Center text vertically
	RECT measured = textRect;
	DrawText(dc, text.c_str(), -1, &measured, flags | DT_CALCRECT);
	const int textHeight = measured.bottom - measured.top;
	textRect.top += (textRect.bottom - textRect.top - textHeight) / 2;
	textRect.bottom = textRect.top + textHeight;

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, enabled ? ColorText : ColorDisabledText);
	DrawText(dc, text.c_str(), -1, &textRect, flags);

	if(ShouldDrawFocus(hwnd))
	{
		RECT focusRect = textRect;
		if(!(flags & (DT_CENTER | DT_RIGHT)))
			focusRect.right = std::min(textRect.right, textRect.left + (measured.right - measured.left));
		InflateRect(&focusRect, 1, 1);
		SetTextColor(dc, ColorText);
		DrawFocusRect(dc, &focusRect);
	}
}


void PaintGroupBox(HWND hwnd, HDC dc)
{
	const UINT dpi = HighDPISupport::GetDpiForWindow(hwnd);
	RECT client;
	GetClientRect(hwnd, &client);

	FontSelector font{dc, hwnd};
	TEXTMETRIC tm{};
	GetTextMetrics(dc, &tm);

	const auto text = GetText(hwnd);
	RECT frame = client;
	frame.top += tm.tmHeight / 2;

	RECT textRect{};
	const UINT flags = DT_SINGLELINE | DT_LEFT | DT_VCENTER | GetPrefixFlags(hwnd);
	if(!text.empty())
	{
		RECT measured{};
		DrawText(dc, text.c_str(), -1, &measured, flags | DT_CALCRECT);
		const int padding = MulDiv(2, dpi, 96);
		textRect.left = client.left + MulDiv(7, dpi, 96);
		textRect.right = std::min(client.right, textRect.left + measured.right + 2 * padding);
		textRect.top = client.top;
		textRect.bottom = client.top + tm.tmHeight;
	}

	const int savedDC = SaveDC(dc);
	if(!text.empty())
		ExcludeClipRect(dc, textRect.left, textRect.top, textRect.right, textRect.bottom);
	FrameRect(dc, &frame, GetBrush(ColorEdge));
	RestoreDC(dc, savedDC);

	if(!text.empty())
	{
		FillRect(dc, &textRect, GetParentBackgroundBrush(hwnd, dc));
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, IsWindowEnabled(hwnd) ? ColorText : ColorDisabledText);
		RECT drawRect = textRect;
		InflateRect(&drawRect, -MulDiv(2, dpi, 96), 0);
		DrawText(dc, text.c_str(), -1, &drawRect, flags);
	}
}


LRESULT HandleButtonMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if(!IsCustomPaintButton(hwnd))
		return DefSubclassProc(hwnd, msg, wParam, lParam);

	switch(msg)
	{
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			if(IsGroupBox(hwnd))
				PaintGroupBox(hwnd, dc);
			else
				PaintBuffered(hwnd, dc, [hwnd](HDC paintDC) { PaintCheckBox(hwnd, paintDC); });
			EndPaint(hwnd, &ps);
			return 0;
		}
	case WM_PRINTCLIENT:
		if(IsGroupBox(hwnd))
			PaintGroupBox(hwnd, reinterpret_cast<HDC>(wParam));
		else
			PaintCheckBox(hwnd, reinterpret_cast<HDC>(wParam));
		return 0;
	case WM_ERASEBKGND:
		if(IsGroupBox(hwnd))
			break;
		return TRUE;
	case WM_SETTEXT:
		if(IsGroupBox(hwnd))
		{
			// Text might get shorter, so the parent needs to redraw the area behind it.
			const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
			RECT rect;
			GetWindowRect(hwnd, &rect);
			MapWindowPoints(nullptr, ::GetParent(hwnd), reinterpret_cast<POINT *>(&rect), 2);
			RedrawWindow(::GetParent(hwnd), &rect, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
			return result;
		}
		[[fallthrough]];
	case WM_ENABLE:
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	case WM_UPDATEUISTATE:
	case BM_SETCHECK:
	case BM_SETSTATE:
	case WM_MOUSEMOVE:
	case WM_MOUSELEAVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		{
			// The default implementation may paint the control outside of WM_PAINT.
			const LRESULT oldState = SendMessage(hwnd, BM_GETSTATE, 0, 0);
			const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
			if((msg != WM_MOUSEMOVE && msg != WM_MOUSELEAVE) || SendMessage(hwnd, BM_GETSTATE, 0, 0) != oldState)
				InvalidateRect(hwnd, nullptr, FALSE);
			return result;
		}
	default:
		break;
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}


//////////////////////////////////////////////////////////////////////////
// Tab controls

void PaintTabControl(HWND hwnd, HDC dc)
{
	RECT client;
	GetClientRect(hwnd, &client);
	FillSolid(dc, client, ColorBackground);

	const int count = TabCtrl_GetItemCount(hwnd);
	if(count <= 0)
		return;

	const UINT dpi = HighDPISupport::GetDpiForWindow(hwnd);
	const int selected = TabCtrl_GetCurSel(hwnd);
	const auto hotIt = g_hotTabs.find(hwnd);
	const int hot = (hotIt != g_hotTabs.end()) ? hotIt->second : -1;

	// Border of the display area, aligned with the bottom of the tab row(s)
	int paneTop = client.top;
	for(int i = 0; i < count; i++)
	{
		RECT itemRect;
		if(TabCtrl_GetItemRect(hwnd, i, &itemRect))
			paneTop = std::max(paneTop, static_cast<int>(itemRect.bottom));
	}
	RECT pane{client.left, paneTop, client.right, client.bottom};
	if(pane.bottom - pane.top > 1)
		FrameRect(dc, &pane, GetBrush(ColorEdge));
	else
		FillSolid(dc, RECT{client.left, paneTop, client.right, paneTop + 1}, ColorEdge);

	FontSelector font{dc, hwnd};
	SetBkMode(dc, TRANSPARENT);
	HIMAGELIST imageList = TabCtrl_GetImageList(hwnd);
	const bool hasFocus = ShouldDrawFocus(hwnd);
	const int gap = MulDiv(4, dpi, 96);

	// Draw selected tab last so that it overlaps its neighbours
	for(int pass = 0; pass < 2; pass++)
	{
		for(int i = 0; i < count; i++)
		{
			const bool isSelected = (i == selected);
			if(isSelected != (pass == 1))
				continue;
			RECT rect;
			if(!TabCtrl_GetItemRect(hwnd, i, &rect))
				continue;
			if(isSelected)
			{
				rect.top -= MulDiv(2, dpi, 96);
				rect.left -= MulDiv(2, dpi, 96);
				rect.right += MulDiv(2, dpi, 96);
				rect.bottom += 1;
			}

			FillSolid(dc, rect, isSelected ? ColorBackground : (i == hot ? ColorHot : ColorTabInactive));
			FillSolid(dc, RECT{rect.left, rect.top, rect.right, rect.top + 1}, ColorEdge);
			FillSolid(dc, RECT{rect.left, rect.top, rect.left + 1, rect.bottom - (isSelected ? 1 : 0)}, ColorEdge);
			FillSolid(dc, RECT{rect.right - 1, rect.top, rect.right, rect.bottom - (isSelected ? 1 : 0)}, ColorEdge);

			TCHAR text[256] = {};
			TCITEM item{};
			item.mask = TCIF_TEXT | TCIF_IMAGE;
			item.pszText = text;
			item.cchTextMax = static_cast<int>(std::size(text));
			TabCtrl_GetItem(hwnd, i, &item);

			RECT textRect = rect;
			UINT flags = DT_SINGLELINE | DT_VCENTER | GetPrefixFlags(hwnd);
			int imageWidth = 0, imageHeight = 0;
			if(imageList && item.iImage >= 0)
				ImageList_GetIconSize(imageList, &imageWidth, &imageHeight);
			if(imageWidth > 0)
			{
				RECT measured{};
				DrawText(dc, text, -1, &measured, flags | DT_CALCRECT);
				const int contentWidth = imageWidth + gap + static_cast<int>(measured.right - measured.left);
				const int x = static_cast<int>(rect.left) + std::max(gap, static_cast<int>(rect.right - rect.left - contentWidth) / 2);
				ImageList_Draw(imageList, item.iImage, dc, x, rect.top + (rect.bottom - rect.top - imageHeight) / 2, ILD_TRANSPARENT);
				textRect.left = x + imageWidth + gap;
				flags |= DT_LEFT;
			} else
			{
				flags |= DT_CENTER;
			}
			SetTextColor(dc, isSelected ? ColorText : ColorDimText);
			DrawText(dc, text, -1, &textRect, flags);

			if(isSelected && hasFocus)
			{
				RECT focusRect = rect;
				InflateRect(&focusRect, -gap / 2 - 1, -gap / 2 - 1);
				SetTextColor(dc, ColorText);
				DrawFocusRect(dc, &focusRect);
			}
		}
	}
}


LRESULT HandleTabMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if(GetWindowStyle(hwnd) & (TCS_OWNERDRAWFIXED | TCS_VERTICAL | TCS_BOTTOM))
		return DefSubclassProc(hwnd, msg, wParam, lParam);

	switch(msg)
	{
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			PaintBuffered(hwnd, dc, [hwnd](HDC paintDC) { PaintTabControl(hwnd, paintDC); });
			EndPaint(hwnd, &ps);
			return 0;
		}
	case WM_PRINTCLIENT:
		PaintTabControl(hwnd, reinterpret_cast<HDC>(wParam));
		return 0;
	case WM_ERASEBKGND:
		return TRUE;
	case WM_MOUSEMOVE:
		{
			TCHITTESTINFO hitTest{};
			hitTest.pt = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
			const int hot = TabCtrl_HitTest(hwnd, &hitTest);
			auto &current = g_hotTabs.try_emplace(hwnd, -1).first->second;
			if(hot != current)
			{
				current = hot;
				InvalidateRect(hwnd, nullptr, FALSE);
				TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
				TrackMouseEvent(&tme);
			}
		}
		break;
	case WM_MOUSELEAVE:
		if(auto it = g_hotTabs.find(hwnd); it != g_hotTabs.end() && it->second != -1)
		{
			it->second = -1;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	case WM_UPDATEUISTATE:
		InvalidateRect(hwnd, nullptr, FALSE);
		break;
	default:
		break;
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}


//////////////////////////////////////////////////////////////////////////
// Status bars

void DrawStatusBarText(HDC dc, LPCTSTR text, RECT rect)
{
	// Tab characters separate left-aligned, centered and right-aligned text
	static constexpr UINT Alignments[] = {DT_LEFT, DT_CENTER, DT_RIGHT};
	size_t alignment = 0;
	LPCTSTR start = text;
	for(LPCTSTR pos = text;; pos++)
	{
		if(*pos == _T('\t') || *pos == _T('\0'))
		{
			if(pos != start)
				DrawText(dc, start, static_cast<int>(pos - start), &rect, Alignments[alignment] | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
			if(*pos == _T('\0'))
				break;
			start = pos + 1;
			alignment = std::min(alignment + 1, std::size(Alignments) - 1);
		}
	}
}


void PaintStatusBar(HWND hwnd, HDC dc)
{
	RECT client;
	GetClientRect(hwnd, &client);
	FillSolid(dc, client, ColorBackground);
	FillSolid(dc, RECT{client.left, client.top, client.right, client.top + 1}, ColorEdge);

	const UINT dpi = HighDPISupport::GetDpiForWindow(hwnd);
	FontSelector font{dc, hwnd};
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, ColorText);

	const bool simple = SendMessage(hwnd, SB_ISSIMPLE, 0, 0) != FALSE;
	const int numParts = simple ? 1 : static_cast<int>(SendMessage(hwnd, SB_GETPARTS, 0, 0));
	const int padding = MulDiv(4, dpi, 96);
	for(int i = 0; i < numParts; i++)
	{
		const int part = simple ? SB_SIMPLEID : i;
		RECT rect = client;
		if(!simple)
			SendMessage(hwnd, SB_GETRECT, i, reinterpret_cast<LPARAM>(&rect));
		const LRESULT lengthAndType = SendMessage(hwnd, SB_GETTEXTLENGTH, part, 0);
		const UINT type = HIWORD(lengthAndType);
		if(type & SBT_OWNERDRAW)
		{
			DRAWITEMSTRUCT dis{};
			dis.CtlID = static_cast<UINT>(GetDlgCtrlID(hwnd));
			dis.itemID = static_cast<UINT>(part);
			dis.hwndItem = hwnd;
			dis.hDC = dc;
			dis.rcItem = rect;
			dis.itemData = static_cast<ULONG_PTR>(SendMessage(hwnd, SB_GETTEXT, part, 0));
			SendMessage(::GetParent(hwnd), WM_DRAWITEM, dis.CtlID, reinterpret_cast<LPARAM>(&dis));
			continue;
		}
		if(!(type & SBT_NOBORDERS) && i < numParts - 1)
			FillSolid(dc, RECT{rect.right - 1, rect.top + padding / 2 + 1, rect.right, rect.bottom - padding / 2}, ColorEdge);

		std::vector<TCHAR> text(static_cast<size_t>(LOWORD(lengthAndType)) + 1, _T('\0'));
		SendMessage(hwnd, SB_GETTEXT, part, reinterpret_cast<LPARAM>(text.data()));
		InflateRect(&rect, -padding, 0);
		DrawStatusBarText(dc, text.data(), rect);
	}

	if((GetWindowStyle(hwnd) & SBARS_SIZEGRIP) && !IsZoomed(GetAncestor(hwnd, GA_ROOT)))
	{
		const int dot = std::max(1, MulDiv(2, dpi, 96)), spacing = dot * 2;
		for(int row = 0; row < 3; row++)
		{
			for(int col = 0; col <= row; col++)
			{
				const int x = client.right - spacing * (col + 1) - 1, y = client.bottom - spacing * (3 - row) - 1;
				FillSolid(dc, RECT{x, y, x + dot, y + dot}, ColorEdge);
			}
		}
	}
}


//////////////////////////////////////////////////////////////////////////
// Toolbars and list view headers (via NM_CUSTOMDRAW sent to their parent)

LRESULT ToolbarCustomDraw(NMTBCUSTOMDRAW &cd)
{
	switch(cd.nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		return CDRF_NOTIFYITEMDRAW;
	case CDDS_ITEMPREPAINT:
		{
			const UINT state = cd.nmcd.uItemState;
			std::optional<COLORREF> fill;
			if(state & CDIS_SELECTED)
				fill = ColorPressedHot;
			else if(state & CDIS_CHECKED)
				fill = (state & CDIS_HOT) ? ColorPressedHot : ColorPressed;
			else if(state & CDIS_HOT)
				fill = ColorHot;
			if(fill)
				FillSolid(cd.nmcd.hdc, cd.nmcd.rc, *fill);
			cd.clrText = (state & CDIS_DISABLED) ? ColorDisabledText : ColorText;
			cd.clrTextHighlight = ColorText;
			cd.clrBtnFace = ColorBackground;
			cd.clrBtnHighlight = ColorHot;
			cd.clrHighlightHotTrack = ColorHot;
			cd.nStringBkMode = TRANSPARENT;
			cd.nHLStringBkMode = TRANSPARENT;
			return TBCDRF_USECDCOLORS | TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES;
		}
	default:
		return CDRF_DODEFAULT;
	}
}


LRESULT HeaderCustomDraw(NMCUSTOMDRAW &cd)
{
	switch(cd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		return CDRF_NOTIFYITEMDRAW;
	case CDDS_ITEMPREPAINT:
		SetTextColor(cd.hdc, ColorText);
		return CDRF_DODEFAULT;
	default:
		return CDRF_DODEFAULT;
	}
}


//////////////////////////////////////////////////////////////////////////
// Menu bar

void DrawMenuBarItem(UAHDRAWMENUITEM &item)
{
	TCHAR text[256] = {};
	MENUITEMINFO mii{};
	mii.cbSize = sizeof(mii);
	mii.fMask = MIIM_STRING | MIIM_BITMAP | MIIM_DATA;
	mii.dwTypeData = text;
	mii.cch = static_cast<UINT>(std::size(text) - 1);
	GetMenuItemInfo(item.um.hmenu, item.umi.iPosition, TRUE, &mii);

	const UINT state = item.dis.itemState;
	HDC dc = item.um.hdc;
	RECT rect = item.dis.rcItem;
	COLORREF background = ColorBackground, textColor = ColorText;
	if(state & ODS_SELECTED)
		background = ColorPressed;
	else if(state & ODS_HOTLIGHT)
		background = ColorHot;
	if(state & (ODS_GRAYED | ODS_DISABLED))
		textColor = ColorDisabledText;
	else if(state & ODS_INACTIVE)
		textColor = ColorDimText;
	FillSolid(dc, rect, background);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, textColor);

	// Buttons of maximized MDI child windows
	const HBITMAP bitmap = mii.hbmpItem;
	if(bitmap == HBMMENU_SYSTEM)
	{
		HWND child = reinterpret_cast<HWND>(mii.dwItemData);
		HICON icon = IsWindow(child) ? reinterpret_cast<HICON>(SendMessage(child, WM_GETICON, ICON_SMALL, 0)) : nullptr;
		if(!icon && IsWindow(child))
			icon = reinterpret_cast<HICON>(GetClassLongPtr(child, GCLP_HICONSM));
		if(icon)
		{
			const int size = std::min(rect.bottom - rect.top, static_cast<LONG>(GetSystemMetrics(SM_CXSMICON)));
			DrawIconEx(dc, rect.left + (rect.right - rect.left - size) / 2, rect.top + (rect.bottom - rect.top - size) / 2, icon, size, size, 0, nullptr, DI_NORMAL);
		}
		return;
	}
	if(bitmap == HBMMENU_MBAR_MINIMIZE || bitmap == HBMMENU_MBAR_MINIMIZE_D || bitmap == HBMMENU_MBAR_RESTORE || bitmap == HBMMENU_MBAR_CLOSE || bitmap == HBMMENU_MBAR_CLOSE_D)
	{
		// Marlett glyphs: 0 = minimize, 2 = restore, r = close
		const TCHAR glyph[] = {(bitmap == HBMMENU_MBAR_RESTORE) ? _T('2') : (bitmap == HBMMENU_MBAR_CLOSE || bitmap == HBMMENU_MBAR_CLOSE_D) ? _T('r') : _T('0'), _T('\0')};
		if(bitmap == HBMMENU_MBAR_MINIMIZE_D || bitmap == HBMMENU_MBAR_CLOSE_D)
			SetTextColor(dc, ColorDisabledText);
		HFONT marlett = CreateFont(-(rect.bottom - rect.top) * 3 / 5, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SYMBOL_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, _T("Marlett"));
		HGDIOBJ oldFont = SelectObject(dc, marlett);
		DrawText(dc, glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, oldFont);
		DeleteObject(marlett);
		return;
	}

	UINT flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
	if(state & ODS_NOACCEL)
		flags |= DT_HIDEPREFIX;
	DrawText(dc, text, -1, &rect, flags);
}


void FillMenuBar(HWND hwnd, HDC dc)
{
	MENUBARINFO mbi{};
	mbi.cbSize = sizeof(mbi);
	if(!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi))
		return;
	RECT window;
	GetWindowRect(hwnd, &window);
	RECT rect = mbi.rcBar;
	OffsetRect(&rect, -window.left, -window.top);
	FillSolid(dc, rect, ColorBackground);
}


// Windows draws a light line between the menu bar and the client area, which we paint over.
void DrawMenuBarBottomLine(HWND hwnd)
{
	if(!GetMenu(hwnd) || IsIconic(hwnd))
		return;
	MENUBARINFO mbi{};
	mbi.cbSize = sizeof(mbi);
	if(!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi))
		return;
	RECT client;
	GetClientRect(hwnd, &client);
	MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&client), 2);
	RECT window;
	GetWindowRect(hwnd, &window);
	OffsetRect(&client, -window.left, -window.top);
	RECT line = client;
	line.bottom = line.top;
	line.top--;
	HDC dc = GetWindowDC(hwnd);
	FillSolid(dc, line, ColorBackground);
	ReleaseDC(hwnd, dc);
}


//////////////////////////////////////////////////////////////////////////
// Borders of child windows (WS_BORDER, WS_EX_CLIENTEDGE, WS_EX_STATICEDGE) are drawn with light system colours.

void PaintNonClientBorder(HWND hwnd, COLORREF innerColor)
{
	const DWORD style = GetWindowStyle(hwnd), exStyle = GetWindowExStyle(hwnd);
	if(!(style & WS_CHILD) || (style & WS_CAPTION) == WS_CAPTION)
		return;
	if(!(style & (WS_BORDER | WS_DLGFRAME | WS_THICKFRAME)) && !(exStyle & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE)))
		return;
	RECT window;
	GetWindowRect(hwnd, &window);
	POINT clientOrigin{0, 0};
	ClientToScreen(hwnd, &clientOrigin);
	const int border = std::min(static_cast<int>(clientOrigin.y - window.top), 8);
	if(border <= 0)
		return;
	HDC dc = GetWindowDC(hwnd);
	RECT rect{0, 0, window.right - window.left, window.bottom - window.top};
	FrameRect(dc, &rect, GetBrush(ColorEdge));
	for(int i = 1; i < border; i++)
	{
		InflateRect(&rect, -1, -1);
		FrameRect(dc, &rect, GetBrush(innerColor));
	}
	ReleaseDC(hwnd, dc);
}


std::optional<COLORREF> GetBorderInnerColor(HWND hwnd, Kind kind)
{
	switch(kind)
	{
	case Kind::Generic:
	case Kind::Static:
		return ColorBackground;
	case Kind::MDIClient:
		return ColorWorkspace;
	case Kind::ListBox:
	case Kind::ListView:
	case Kind::TreeView:
	case Kind::RichEdit:
		return ColorWindow;
	case Kind::Edit:
		if(GetWindowStyle(hwnd) & (WS_VSCROLL | WS_HSCROLL))
			return ColorWindow;
		return std::nullopt;
	default:
		return std::nullopt;
	}
}


//////////////////////////////////////////////////////////////////////////
// Messages handled for all themed windows

LRESULT HandleCtlColor(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
	// Respect colours chosen by the application, but override the default colours and the tab page texture of property pages.
	if(!IsDefaultControlBrush(reinterpret_cast<HBRUSH>(result)) && !IsThemeDialogTextureEnabled(hwnd))
		return result;

	HDC dc = reinterpret_cast<HDC>(wParam);
	HWND control = reinterpret_cast<HWND>(lParam);
	const COLORREF background = (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) ? ColorWindow : ColorBackground;
	const bool enabled = !control || IsWindowEnabled(control);
	SetTextColor(dc, enabled ? ColorText : ColorDisabledText);
	SetBkColor(dc, background);
	return reinterpret_cast<LRESULT>(GetBrush(background));
}


// Erase the background with a dark brush if the window class uses a system colour brush for its background.
LRESULT HandleEraseBackground(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	const auto classBrush = reinterpret_cast<HBRUSH>(GetClassLongPtr(hwnd, GCLP_HBRBACKGROUND));
	const auto sysColorIndex = GetSysColorIndexOfBrush(classBrush);
	if(!sysColorIndex)
		return DefSubclassProc(hwnd, msg, wParam, lParam);
	// Temporarily swap the class brush, so that application handlers that do their own erasing are still respected.
	SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g_darkSysColorBrushes[*sysColorIndex]));
	const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
	SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(classBrush));
	return result;
}


LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
{
	const Kind kind = static_cast<Kind>(refData);
	if(msg == WM_NCDESTROY)
	{
		RemoveWindowSubclass(hwnd, SubclassProc, SubclassID);
		if(kind == Kind::Ignore)
			RemoveProp(hwnd, ExcludeProp());
		g_hotTabs.erase(hwnd);
		return DefSubclassProc(hwnd, msg, wParam, lParam);
	}
	if(kind == Kind::Ignore || !g_enabled)
		return DefSubclassProc(hwnd, msg, wParam, lParam);

	switch(msg)
	{
	case WM_CREATE:
		{
			const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
			if(result != -1 && kind != Kind::PendingDialog)
				ApplyControlColors(hwnd, kind, true);
			return result;
		}

	case WM_INITDIALOG:
		if(kind == Kind::PendingDialog)
		{
			const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
			ResolvePendingDialog(hwnd);
			return result;
		}
		break;

	case WM_THEMECHANGED:
		if(kind == Kind::ListView || kind == Kind::TreeView)
		{
			const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
			ApplyControlColors(hwnd, kind, true);
			return result;
		}
		break;

	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLORSCROLLBAR:
		if(kind != Kind::PendingDialog)
			return HandleCtlColor(hwnd, msg, wParam, lParam);
		break;

	case WM_NOTIFY:
		if(const auto *header = reinterpret_cast<const NMHDR *>(lParam); header && header->code == NM_CUSTOMDRAW)
		{
			const auto childKind = GetAttachedKind(header->hwndFrom);
			if(childKind == Kind::Toolbar)
				return ToolbarCustomDraw(*reinterpret_cast<NMTBCUSTOMDRAW *>(lParam));
			if(childKind == Kind::Header)
				return HeaderCustomDraw(*reinterpret_cast<NMCUSTOMDRAW *>(lParam));
		}
		break;

	case WM_ERASEBKGND:
		if(kind == Kind::StatusBar)
			return TRUE;
		if(kind == Kind::Toolbar && !(GetWindowStyle(hwnd) & TBSTYLE_TRANSPARENT))
		{
			RECT rect;
			GetClientRect(hwnd, &rect);
			FillSolid(reinterpret_cast<HDC>(wParam), rect, ColorBackground);
			return TRUE;
		}
		if(kind != Kind::Button && kind != Kind::Tab)
			return HandleEraseBackground(hwnd, msg, wParam, lParam);
		break;

	case WM_PAINT:
	case WM_PRINTCLIENT:
		if(kind == Kind::Generic && IsForeignDialog(hwnd))
		{
			// Message boxes paint a light two-tone background (content area and button footer) in WM_PAINT.
			// All their actual content consists of child controls, so we can paint the background ourselves.
			RECT rect;
			GetClientRect(hwnd, &rect);
			if(msg == WM_PRINTCLIENT)
			{
				FillSolid(reinterpret_cast<HDC>(wParam), rect, ColorBackground);
				return 0;
			}
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			FillSolid(dc, rect, ColorBackground);
			EndPaint(hwnd, &ps);
			return 0;
		}
		// MFC status bars must update their pane texts before painting, so they call DrawStatusBar themselves.
		if(kind == Kind::StatusBar && !CWnd::FromHandlePermanent(hwnd))
		{
			if(msg == WM_PRINTCLIENT)
			{
				PaintStatusBar(hwnd, reinterpret_cast<HDC>(wParam));
				return 0;
			}
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			PaintBuffered(hwnd, dc, [hwnd](HDC paintDC) { PaintStatusBar(hwnd, paintDC); });
			EndPaint(hwnd, &ps);
			return 0;
		}
		break;

	case WM_NCPAINT:
	case WM_NCACTIVATE:
		{
			const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
			if(!(GetWindowStyle(hwnd) & WS_CHILD))
				DrawMenuBarBottomLine(hwnd);
			else if(const auto innerColor = GetBorderInnerColor(hwnd, kind); innerColor && msg == WM_NCPAINT)
				PaintNonClientBorder(hwnd, *innerColor);
			return result;
		}

	case WM_UAH_DRAWMENU:
		if(GetMenu(hwnd))
		{
			FillMenuBar(hwnd, reinterpret_cast<UAHMENU *>(lParam)->hdc);
			return TRUE;
		}
		break;

	case WM_UAH_DRAWMENUITEM:
		if(GetMenu(hwnd))
		{
			DrawMenuBarItem(*reinterpret_cast<UAHDRAWMENUITEM *>(lParam));
			return TRUE;
		}
		break;

	default:
		break;
	}

	if(kind == Kind::Button)
		return HandleButtonMessage(hwnd, msg, wParam, lParam);
	if(kind == Kind::Tab)
		return HandleTabMessage(hwnd, msg, wParam, lParam);

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}


void SetEnabled(bool dark)
{
	if(dark == g_enabled)
		return;
	if(dark)
		InstallSysColorHooks();
	g_enabled = dark;
	SetAppMode(dark);

	if(dark && !g_cbtHook)
	{
		g_cbtHook = SetWindowsHookEx(WH_CBT, CbtProc, nullptr, GetCurrentThreadId());
	} else if(!dark && g_cbtHook)
	{
		UnhookWindowsHookEx(g_cbtHook);
		g_cbtHook = nullptr;
	}

	const DWORD threadID = GetCurrentThreadId();
	EnumThreadWindows(threadID, dark ? AttachTopLevelProc : DetachTopLevelProc, 0);

	// Let MFC and OpenMPT update everything that depends on system colours (MFC caches them in afxData).
	if(CWnd *mainWnd = AfxGetMainWnd(); mainWnd && mainWnd->m_hWnd)
		mainWnd->SendMessage(WM_SYSCOLORCHANGE);

	EnumThreadWindows(threadID, RedrawTopLevelProc, 0);
	// Views refresh their contents in response to posted messages, which can leave controls stale.
	// WM_TIMER is only dispatched once all posted messages have been processed, so repaint again then.
	SetTimer(nullptr, 0, 0, DeferredRedrawProc);
}

}  // namespace


void Initialize(DarkModeSetting setting)
{
	g_setting = setting;
	if(HMODULE user32 = GetModuleHandle(_T("user32.dll")))
	{
		g_realGetSysColor = GetProcAddressAs<GetSysColorFunc>(user32, "GetSysColor");
		g_realGetSysColorBrush = GetProcAddressAs<GetSysColorBrushFunc>(user32, "GetSysColorBrush");
		g_realDrawEdge = GetProcAddressAs<DrawEdgeFunc>(user32, "DrawEdge");
		g_realDrawFrameControl = GetProcAddressAs<DrawFrameControlFunc>(user32, "DrawFrameControl");
	}
	if(!g_realGetSysColor || !g_realGetSysColorBrush || !g_realDrawEdge || !g_realDrawFrameControl)
		return;
	if(!g_excludeAtom)
		g_excludeAtom = GlobalAddAtom(_T("OpenMPT.DarkMode.Exclude"));
	if(!g_excludeAtom)
		return;
	LoadAPI();
	SetEnabled(ShouldUseDarkMode());
}


void Shutdown()
{
	if(g_cbtHook)
	{
		UnhookWindowsHookEx(g_cbtHook);
		g_cbtHook = nullptr;
	}
}


bool IsSupported()
{
	return g_api.supported && g_excludeAtom;
}


bool IsEnabled()
{
	return g_enabled;
}


void SetSetting(DarkModeSetting setting)
{
	g_setting = setting;
	if(IsSupported())
		SetEnabled(ShouldUseDarkMode());
}


void OnSettingChange(UINT flags, LPCTSTR section)
{
	if(!IsSupported())
		return;
	if(flags == SPI_SETHIGHCONTRAST || (section && !_tcscmp(section, _T("ImmersiveColorSet"))))
		SetEnabled(ShouldUseDarkMode());
}


COLORREF GetRealSysColor(int index)
{
	return g_realGetSysColor ? g_realGetSysColor(index) : ::GetSysColor(index);
}


void ExcludeWindow(HWND hwnd)
{
	if(!hwnd || !g_excludeAtom)
		return;
	DetachFromWindow(hwnd);
	EnumChildWindows(hwnd, DetachChildProc, 0);
	MarkExcluded(hwnd);
}


void DrawStatusBar(HWND hwnd, HDC hdc)
{
	PaintBuffered(hwnd, hdc, [hwnd](HDC paintDC) { PaintStatusBar(hwnd, paintDC); });
}

}  // namespace DarkMode


#else  // !MPT_WINNT_AT_LEAST(MPT_WIN_VISTA)


namespace DarkMode
{
void Initialize(DarkModeSetting) {}
void Shutdown() {}
bool IsSupported() { return false; }
bool IsEnabled() { return false; }
void SetSetting(DarkModeSetting) {}
void OnSettingChange(UINT, LPCTSTR) {}
COLORREF GetRealSysColor(int index) { return ::GetSysColor(index); }
void ExcludeWindow(HWND) {}
void DrawStatusBar(HWND, HDC) {}
}  // namespace DarkMode


#endif  // MPT_WINNT_AT_LEAST(MPT_WIN_VISTA)


OPENMPT_NAMESPACE_END
