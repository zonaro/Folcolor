// Foldrion(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"
#include <versionhelpers.h>
#include <cwctype>
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <wincodec.h>
#include "resource.h"
#include "FolderColorize.h"
#include "GeneratedColorNames.h"

#pragma comment(lib, "Comdlg32.lib")

#define APP_URL "http://www.foldrion.com/"

static BOOL isInstalled = FALSE;
static BOOL isRunningOutsideInstallFolder = FALSE;
WCHAR myPathGlobal[MAX_PATH] = {};
int iconOffsetGlobal = WIN7_ICON_OFFSET;
extern void Install();
extern int Uninstall();
extern BOOL HasInstallRegistry();

// ntdll.lib
typedef long NTSTATUS;
extern "C" NTSYSAPI NTSTATUS NTAPI RtlGetVersion(__out PRTL_OSVERSIONINFOEXW VersionInformation /*PRTL_OSVERSIONINFOW*/);


// If there is another instance of us running, pull it into focus and return TRUE
static BOOL FindDoppelganger()
{
	BOOL found = FALSE;

	UINT myPid = GetCurrentProcessId();
	char myName[_MAX_FNAME];
	if (!GetModuleBaseNameA(GetCurrentProcess(), NULL, myName, _countof(myName)))
		strcpy_s(myName, TARGET_NAME);

	PROCESSENTRY32 nfo;
	nfo.dwSize = sizeof(nfo);
	HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (handle != INVALID_HANDLE_VALUE)
	{
		if (Process32First(handle, &nfo))
		{
			do
			{
				if ((strcmp(myName, nfo.szExeFile) == 0) && (nfo.th32ProcessID != myPid))
				{
					HWND hwnd = GetHwndForPid(nfo.th32ProcessID);
					if(hwnd)
						ForceWindowFocus(hwnd);
					found = TRUE;
					break;
				}

			} while (Process32Next(handle, &nfo));
		}

		CloseHandle(handle);
	}

	return found;
}


// ----------------------------------------------------------------------------
// Custom hyperlink control
#define VISITED_COLOR RGB(160, 160, 260)
#define URL_FONT_HEIGHT 18
#define URL_FONT_WIDTH FW_SEMIBOLD
static HFONT tweakedFont = NULL, undlineFont = NULL;
static BOOL isClicking = FALSE, isVisited = FALSE;
static COLORREF urlColor = RGB(0, 102, 204);
static HBRUSH gDialogBackgroundBrush = NULL;
static HBRUSH gDialogButtonBrush = NULL;
static COLORREF gDialogBackgroundColor = RGB(255, 255, 255);
static COLORREF gDialogButtonColor = RGB(240, 240, 240);
static COLORREF gDialogTextColor = RGB(0, 0, 0);
static HCURSOR mouseOverCursor = NULL;

/**
 * Refresh installer dialog palette from the current system light/dark mode.
 */
static void UpdateInstallerTheme(HWND hWnd)
{
	BOOL darkMode = IsSystemDarkModeEnabled();
	gDialogBackgroundColor = darkMode ? RGB(32, 32, 32) : RGB(255, 255, 255);
	gDialogButtonColor = darkMode ? RGB(52, 52, 52) : RGB(240, 240, 240);
	gDialogTextColor = darkMode ? RGB(232, 232, 232) : RGB(0, 0, 0);

	if (gDialogBackgroundBrush)
	{
		DeleteObject(gDialogBackgroundBrush);
		gDialogBackgroundBrush = NULL;
	}

	if (gDialogButtonBrush)
	{
		DeleteObject(gDialogButtonBrush);
		gDialogButtonBrush = NULL;
	}

	gDialogBackgroundBrush = CreateSolidBrush(gDialogBackgroundColor);
	gDialogButtonBrush = CreateSolidBrush(gDialogButtonColor);

	if (!isVisited)
		urlColor = darkMode ? RGB(138, 180, 248) : RGB(0, 102, 204);

	ApplyThemeToWindowAndChildren(hWnd);
	InvalidateRect(hWnd, NULL, TRUE);
}

/**
 * Build absolute path to the installed executable expected in the install folder.
 */
static void BuildInstalledExePath(WCHAR* exePath, size_t exePathCount)
{
	if (!exePath || (exePathCount == 0))
		return;

	exePath[0] = L'\0';
	if (_snwprintf_s(exePath, exePathCount, (exePathCount - 1), L"%s%S", myPathGlobal, TARGET_NAME) < 1)
		CRITICAL("Path size limit error!");
}

/**
 * Compare two filesystem paths case-insensitively after expanding them.
 */
static BOOL AreSamePath(LPCWSTR leftPath, LPCWSTR rightPath)
{
	if (!leftPath || !leftPath[0] || !rightPath || !rightPath[0])
		return FALSE;

	WCHAR normalizedLeft[MAX_PATH] = {};
	WCHAR normalizedRight[MAX_PATH] = {};
	DWORD leftLen = GetFullPathNameW(leftPath, _countof(normalizedLeft), normalizedLeft, NULL);
	DWORD rightLen = GetFullPathNameW(rightPath, _countof(normalizedRight), normalizedRight, NULL);
	if ((leftLen > 0) && (leftLen < _countof(normalizedLeft)) && (rightLen > 0) && (rightLen < _countof(normalizedRight)))
		return (_wcsicmp(normalizedLeft, normalizedRight) == 0);

	return (_wcsicmp(leftPath, rightPath) == 0);
}

/**
 * Enable or disable controls that require a completed installation.
 */
static void UpdateInstallDependentControls(HWND hWnd)
{
	EnableWindow(GetDlgItem(hWnd, IDC_REINSTALL), (isInstalled && isRunningOutsideInstallFolder));
}

/**
 * Build a compact user message listing files that failed to import.
 */
static void AppendFailedImportList(char* buffer, size_t bufferSize, const std::vector<std::wstring>& failedFiles)
{
	if (!buffer || (bufferSize == 0) || failedFiles.empty())
		return;

	strcat_s(buffer, bufferSize, " Failed: ");
	for (size_t i = 0; i < failedFiles.size(); i++)
	{
		char nameBuffer[128] = {};
		WideCharToMultiByte(CP_ACP, 0, failedFiles[i].c_str(), -1, nameBuffer, (int) sizeof(nameBuffer), NULL, NULL);

		if (i > 0)
			strcat_s(buffer, bufferSize, ", ");
		strcat_s(buffer, bufferSize, nameBuffer[0] ? nameBuffer : "<unknown>");

		if (i == 4 && (failedFiles.size() > 5))
		{
			char extraBuffer[32];
			sprintf_s(extraBuffer, sizeof(extraBuffer), " and %u more", (UINT) (failedFiles.size() - 5));
			strcat_s(buffer, bufferSize, extraBuffer);
			break;
		}
	}
}

/**
 * Open a directory in Explorer.
 */
static BOOL OpenDirectoryInExplorer(LPCWSTR folderPath)
{
	if (!folderPath || !folderPath[0])
		return FALSE;

	HINSTANCE hRes = ShellExecuteW(NULL, L"open", folderPath, NULL, NULL, SW_SHOWNORMAL);
	return ((INT_PTR) hRes > 32);
}

/**
 * Ensure a directory exists by creating it when missing.
 */
static BOOL EnsureDirectoryExists(LPCWSTR folderPath)
{
	DWORD attr = GetFileAttributesW(folderPath);
	if ((attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY))
		return TRUE;

	if (CreateDirectoryW(folderPath, NULL))
		return TRUE;

	return (GetLastError() == ERROR_ALREADY_EXISTS);
}


/**
 * Runtime icon picker item.
 */
struct PickerItem
{
	BOOL isBuiltIn;
	int builtInIndex;
	int builtInOffset;
	std::wstring resourcePath;
	int resourceIndex;
	std::wstring label;
	HICON cachedIcon;
	BOOL ownsCachedIcon;
};


/**
 * Runtime icon picker category.
 */
struct PickerCategory
{
	std::wstring name;
	std::vector<PickerItem> items;
};


struct SystemCategoryEntry
{
	LPCWSTR envPath;
	LPCWSTR label;
};

static const SystemCategoryEntry kSystemDlls[] =
{
	{ L"%SystemRoot%\\System32\\imageres.dll", L"Windows Icon Library (imageres.dll)" },
	{ L"%SystemRoot%\\System32\\shell32.dll", L"Windows Shell Core (shell32.dll)" },
	{ L"%SystemRoot%\\System32\\pifmgr.dll", L"Legacy Program Icons (pifmgr.dll)" },
	{ L"%SystemRoot%\\System32\\ddores.dll", L"Device Category Resources (ddores.dll)" },
	{ L"%SystemRoot%\\System32\\accessibilitycpl.dll", L"Ease of Access (accessibilitycpl.dll)" },
	{ L"%SystemRoot%\\System32\\mmres.dll", L"Audio Resources (mmres.dll)" },
	{ L"%SystemRoot%\\System32\\netshell.dll", L"Network Connections (netshell.dll)" },
	{ L"%SystemRoot%\\explorer.exe", L"File Explorer Shell (explorer.exe)" },
	{ L"%SystemRoot%\\System32\\wmploc.dll", L"Media Player Resources (wmploc.dll)" },
};

static std::vector<PickerCategory> gPickerCategories;
struct PickerVisibleItem
{
	int categoryIndex;
	int itemIndex;
	std::wstring displayLabel;
};

static std::vector<PickerVisibleItem> gPickerVisibleItems;
static std::wstring gPickerSearchQuery;
static std::wstring gPickerTargetFolder;

enum PickerColorCategoryIndex
{
	PICKER_COLORS_WIN11 = 0,
	PICKER_COLORS_WIN10 = 1,
	PICKER_COLORS_WIN78 = 2,
};

#define IDC_PICKER_CATEGORY 2001
#define IDC_PICKER_ITEM 2002
#define IDC_PICKER_APPLY 2003
#define IDC_PICKER_RESTORE 2004
#define IDC_PICKER_CANCEL 2005
#define IDC_PICKER_IMPORT 2006
#define IDC_PICKER_SEARCH 2007
#define IDC_PICKER_OPEN_ICONS 2008
#define IDC_PICKER_PREVIEW 2009
#define IDC_PICKER_PREVIEW_LABEL 2010
#define IDC_PICKER_CREATE_DERIVED 2011
#define IDC_PICKER_DELETE 2012

#define PICKER_SEARCH_DEBOUNCE_TIMER_ID 1
#define PICKER_SEARCH_DEBOUNCE_MS 500
#define WM_PICKER_DELETE_REQUEST (WM_APP + 101)

/** Large icon displayed in the preview box. */
static HICON gPickerPreviewIcon      = NULL;
static BOOL  gPickerPreviewOwnsIcon  = FALSE;
static BOOL  gPickerPreviewIsFolder  = FALSE; // TRUE when showing the folder's current icon
static BOOL  gPickerPreviewIsDefault = FALSE; // TRUE when showing the OS default folder icon (no custom icon set)

// Derived icon editor controls.
#define IDC_DERIVED_PREVIEW 3001
#define IDC_DERIVED_LAYER_LIST 3002
#define IDC_DERIVED_ADD_LAYER 3003
#define IDC_DERIVED_REMOVE_LAYER 3004
#define IDC_DERIVED_MOVE_UP 3005
#define IDC_DERIVED_MOVE_DOWN 3006
#define IDC_DERIVED_HUE 3007
#define IDC_DERIVED_SATURATION 3008
#define IDC_DERIVED_OPACITY 3009
#define IDC_DERIVED_SCALE 3010
#define IDC_DERIVED_POSX 3011
#define IDC_DERIVED_POSY 3012
#define IDC_DERIVED_SAVE 3013
#define IDC_DERIVED_CANCEL 3014
#define IDC_DERIVED_LABEL_HUE 3015
#define IDC_DERIVED_LABEL_SATURATION 3016
#define IDC_DERIVED_LABEL_OPACITY 3017
#define IDC_DERIVED_LABEL_SCALE 3018
#define IDC_DERIVED_LABEL_POSX 3019
#define IDC_DERIVED_LABEL_POSY 3020
#define IDC_DERIVED_EDIT_HUE 3021
#define IDC_DERIVED_EDIT_SATURATION 3022
#define IDC_DERIVED_EDIT_OPACITY 3023
#define IDC_DERIVED_EDIT_SCALE 3024
#define IDC_DERIVED_EDIT_POSX 3025
#define IDC_DERIVED_EDIT_POSY 3026
#define IDC_DERIVED_COLORIZE 3027
#define IDC_DERIVED_SAVE_CATEGORY_LABEL 3028
#define IDC_DERIVED_SAVE_CATEGORY_EDIT 3029
#define IDC_DERIVED_SAVE_NAME_LABEL 3030
#define IDC_DERIVED_SAVE_NAME_EDIT 3031
#define IDC_DERIVED_SAVE_OK 3032
#define IDC_DERIVED_SAVE_CANCEL 3033
#define IDC_DERIVED_ROTATION 3034
#define IDC_DERIVED_LABEL_ROTATION 3035
#define IDC_DERIVED_EDIT_ROTATION 3036

/** A user-loaded overlay layer in the derived icon editor. */
struct DerivedLayer
{
	std::wstring sourcePath;
	std::wstring displayName;
	std::vector<BYTE> pixels;
	UINT width;
	UINT height;
	int hue;
	int saturation;
	int opacity;
	int scale;
	int posX;
	int posY;
	int rotation;
	BOOL colorize;
	BOOL isBase;
};

/** Runtime state for the derived icon editor window. */
struct DerivedEditorState
{
	HWND hWnd;
	HWND hParent;
	PickerItem baseItem;
	std::vector<BYTE> basePixels;
	std::vector<BYTE> composedPixels;
	std::vector<DerivedLayer> layers;
	std::wstring savedPath;
	BOOL syncingControls;
	BOOL draggingLayer;
	POINT dragStartPoint;
	int dragStartPosX;
	int dragStartPosY;
};

static DerivedEditorState gDerivedEditor = {};

/** Runtime state for the derived icon save dialog. */
struct DerivedSaveDialogState
{
	HWND hWnd;
	HWND hParent;
	std::wstring category;
	std::wstring name;
	BOOL confirmed;
};

static DerivedSaveDialogState gDerivedSaveDialog = {};

// Runtime picker/editor theme resources.
static HBRUSH gRuntimeBackgroundBrush = NULL;
static HBRUSH gRuntimePanelBrush = NULL;
static HBRUSH gRuntimeSelectionBrush = NULL;
static COLORREF gRuntimeBackgroundColor = RGB(250, 250, 250);
static COLORREF gRuntimePanelColor = RGB(255, 255, 255);
static COLORREF gRuntimeTextColor = RGB(24, 24, 24);
static COLORREF gRuntimeSelectionColor = RGB(0, 120, 215);
static COLORREF gRuntimeSelectionTextColor = RGB(255, 255, 255);
static BOOL gRuntimeThemeDark = FALSE;
static int gRuntimeThemeUsers = 0;

/**
 * Rebuild picker/editor brushes based on current light/dark preference.
 */
static void RefreshRuntimeThemeResources()
{
	BOOL darkMode = IsSystemDarkModeEnabled();
	if ((gRuntimeBackgroundBrush != NULL) && (gRuntimeThemeDark == darkMode))
		return;

	if (gRuntimeBackgroundBrush)
	{
		DeleteObject(gRuntimeBackgroundBrush);
		gRuntimeBackgroundBrush = NULL;
	}

	if (gRuntimePanelBrush)
	{
		DeleteObject(gRuntimePanelBrush);
		gRuntimePanelBrush = NULL;
	}

	if (gRuntimeSelectionBrush)
	{
		DeleteObject(gRuntimeSelectionBrush);
		gRuntimeSelectionBrush = NULL;
	}

	gRuntimeThemeDark = darkMode;
	if (darkMode)
	{
		gRuntimeBackgroundColor = RGB(32, 32, 32);
		gRuntimePanelColor = RGB(44, 44, 44);
		gRuntimeTextColor = RGB(232, 232, 232);
		gRuntimeSelectionColor = RGB(58, 91, 138);
		gRuntimeSelectionTextColor = RGB(255, 255, 255);
	}
	else
	{
		gRuntimeBackgroundColor = RGB(250, 250, 250);
		gRuntimePanelColor = RGB(255, 255, 255);
		gRuntimeTextColor = RGB(24, 24, 24);
		gRuntimeSelectionColor = RGB(0, 120, 215);
		gRuntimeSelectionTextColor = RGB(255, 255, 255);
	}

	gRuntimeBackgroundBrush = CreateSolidBrush(gRuntimeBackgroundColor);
	gRuntimePanelBrush = CreateSolidBrush(gRuntimePanelColor);
	gRuntimeSelectionBrush = CreateSolidBrush(gRuntimeSelectionColor);
}

/**
 * Apply runtime theme and repaint a picker/editor window.
 */
static void ApplyRuntimeTheme(HWND hWnd)
{
	RefreshRuntimeThemeResources();
	ApplyThemeToWindowAndChildren(hWnd);
	InvalidateRect(hWnd, NULL, TRUE);
	UpdateWindow(hWnd);
}

/**
 * Release runtime theme brushes once no picker/editor windows are open.
 */
static void ReleaseRuntimeThemeResourcesIfUnused()
{
	if (gRuntimeThemeUsers > 0)
		return;

	if (gRuntimeBackgroundBrush)
	{
		DeleteObject(gRuntimeBackgroundBrush);
		gRuntimeBackgroundBrush = NULL;
	}

	if (gRuntimePanelBrush)
	{
		DeleteObject(gRuntimePanelBrush);
		gRuntimePanelBrush = NULL;
	}

	if (gRuntimeSelectionBrush)
	{
		DeleteObject(gRuntimeSelectionBrush);
		gRuntimeSelectionBrush = NULL;
	}
}

static LRESULT CALLBACK DerivedPreviewSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);
	UNREFERENCED_PARAMETER(dwRefData);
	HWND hParent = GetParent(hWnd);
	if (!hParent)
		return DefSubclassProc(hWnd, uMsg, wParam, lParam);

	switch (uMsg)
	{
		case WM_MOUSEWHEEL:
			SendMessageW(hParent, WM_MOUSEWHEEL, wParam, lParam);
			return 0;

		case WM_LBUTTONDOWN:
		case WM_MOUSEMOVE:
		case WM_LBUTTONUP:
		{
			POINT pt = { (short) LOWORD(lParam), (short) HIWORD(lParam) };
			ClientToScreen(hWnd, &pt);
			ScreenToClient(hParent, &pt);
			SendMessageW(hParent, uMsg, wParam, MAKELPARAM(pt.x, pt.y));
			return 0;
		}

		case WM_NCDESTROY:
			RemoveWindowSubclass(hWnd, DerivedPreviewSubclassProc, 1);
			break;
	}

	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

/**
 * Return lowercase copy for case-insensitive matching.
 */
static std::wstring ToLowerCopy(const std::wstring& value)
{
	std::wstring out = value;
	for (size_t i = 0; i < out.size(); i++)
		out[i] = (WCHAR) towlower(out[i]);
	return out;
}


/**
 * Return a copy without whitespace characters.
 */
static std::wstring RemoveWhitespaceCopy(const std::wstring& value)
{
	std::wstring out;
	out.reserve(value.size());
	for (size_t i = 0; i < value.size(); i++)
	{
		if (!iswspace(value[i]))
			out.push_back(value[i]);
	}
	return out;
}


/**
 * Compute Levenshtein edit distance between two lowercase wide strings.
 * Uses the standard iterative two-row DP approach.
 */
static int LevenshteinDistance(const std::wstring& a, const std::wstring& b)
{
	size_t m = a.size();
	size_t n = b.size();
	if (m == 0) return (int) n;
	if (n == 0) return (int) m;

	std::vector<int> prev(n + 1, 0);
	std::vector<int> curr(n + 1, 0);
	for (size_t j = 0; j <= n; j++)
		prev[j] = (int) j;

	for (size_t i = 1; i <= m; i++)
	{
		curr[0] = (int) i;
		for (size_t j = 1; j <= n; j++)
		{
			int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			curr[j] = min(min(prev[j] + 1, curr[j - 1] + 1), prev[j - 1] + cost);
		}
		std::swap(prev, curr);
	}
	return prev[n];
}


/**
 * Return TRUE if lowerQuery fuzzy-matches the lowerTarget string.
 * First tries a fast substring check; then tokenises on spaces and checks
 * each token with Levenshtein distance <= maxDist.
 */
static BOOL FuzzyMatchQuery(const std::wstring& lowerQuery, const std::wstring& lowerTarget, int maxDist)
{
	std::wstring compactQuery = RemoveWhitespaceCopy(lowerQuery);
	std::wstring compactTarget = RemoveWhitespaceCopy(lowerTarget);

	if (compactQuery.empty())
		return TRUE;

	if (compactTarget.find(compactQuery) != std::wstring::npos)
		return TRUE;

	size_t qLen = compactQuery.size();
	size_t tLen = compactTarget.size();
	if ((int) qLen <= (int) tLen + maxDist &&
		(int) tLen <= (int) qLen + maxDist)
	{
		if (LevenshteinDistance(compactQuery, compactTarget) <= maxDist)
			return TRUE;
	}

	tLen = lowerTarget.size();
	size_t start = 0;
	while (start <= tLen)
	{
		size_t end = lowerTarget.find(L' ', start);
		if (end == std::wstring::npos)
			end = tLen;

		size_t tokenLen = end - start;
		if (tokenLen > 0)
		{
			std::wstring token = RemoveWhitespaceCopy(lowerTarget.substr(start, tokenLen));
			tokenLen = token.size();
			if ((int) qLen <= (int) tokenLen + maxDist &&
				(int) tokenLen <= (int) qLen + maxDist)
			{
				if (LevenshteinDistance(compactQuery, token) <= maxDist)
					return TRUE;
			}
		}

		if (end == tLen)
			break;
		start = end + 1;
	}
	return FALSE;
}


/**
 * Return the best Levenshtein distance between query and target forms.
 * Includes compact full-string compare and compact token compares.
 */
static int BestLevenshteinDistance(const std::wstring& lowerQuery, const std::wstring& lowerTarget)
{
	std::wstring compactQuery = RemoveWhitespaceCopy(lowerQuery);
	std::wstring compactTarget = RemoveWhitespaceCopy(lowerTarget);
	if (compactQuery.empty())
		return 0;

	if (compactTarget.find(compactQuery) != std::wstring::npos)
		return 0;

	int best = LevenshteinDistance(compactQuery, compactTarget);
	size_t tLen = lowerTarget.size();
	size_t start = 0;
	while (start <= tLen)
	{
		size_t end = lowerTarget.find(L' ', start);
		if (end == std::wstring::npos)
			end = tLen;

		size_t tokenLen = end - start;
		if (tokenLen > 0)
		{
			std::wstring token = RemoveWhitespaceCopy(lowerTarget.substr(start, tokenLen));
			if (!token.empty())
			{
				int tokenDist = LevenshteinDistance(compactQuery, token);
				if (tokenDist < best)
					best = tokenDist;
			}
		}

		if (end == tLen)
			break;
		start = end + 1;
	}

	return best;
}


/**
 * Refresh current search query from the search box.
 */
static void UpdatePickerSearchQuery(HWND hWnd)
{
	HWND hSearch = GetDlgItem(hWnd, IDC_PICKER_SEARCH);
	if (!hSearch)
	{
		gPickerSearchQuery.clear();
		return;
	}

	int len = GetWindowTextLengthW(hSearch);
	if (len <= 0)
	{
		gPickerSearchQuery.clear();
		return;
	}

	std::vector<WCHAR> buffer((size_t) len + 1, 0);
	GetWindowTextW(hSearch, &buffer[0], len + 1);
	gPickerSearchQuery = ToLowerCopy(std::wstring(&buffer[0]));
}


/**
 * Rebuild visible items based on selected category or global search.
 */
static void RebuildPickerVisibleItems(int categoryIndex)
{
	gPickerVisibleItems.clear();

	if (gPickerSearchQuery.empty())
	{
		if ((categoryIndex < 0) || (categoryIndex >= (int) gPickerCategories.size()))
			return;

		const PickerCategory& cat = gPickerCategories[categoryIndex];
		for (size_t i = 0; i < cat.items.size(); i++)
		{
			PickerVisibleItem visible = {};
			visible.categoryIndex = categoryIndex;
			visible.itemIndex = (int) i;
			visible.displayLabel = cat.items[i].label;
			gPickerVisibleItems.push_back(visible);
		}
		return;
	}

	struct SearchCandidate
	{
		PickerVisibleItem visible;
		int distance;
		std::wstring sortKey;
	};

	std::vector<SearchCandidate> candidates;

	for (size_t c = 0; c < gPickerCategories.size(); c++)
	{
		const PickerCategory& cat = gPickerCategories[c];
		std::wstring lowerCategory = ToLowerCopy(cat.name);

		for (size_t i = 0; i < cat.items.size(); i++)
		{
			const PickerItem& item = cat.items[i];
			std::wstring lowerLabel = ToLowerCopy(item.label);
			if (!FuzzyMatchQuery(gPickerSearchQuery, lowerLabel, 3) &&
				!FuzzyMatchQuery(gPickerSearchQuery, lowerCategory, 3))
			{
				continue;
			}

			int labelDistance = BestLevenshteinDistance(gPickerSearchQuery, lowerLabel);
			int categoryDistance = BestLevenshteinDistance(gPickerSearchQuery, lowerCategory);

			PickerVisibleItem visible = {};
			visible.categoryIndex = (int) c;
			visible.itemIndex = (int) i;
			visible.displayLabel = L"[" + cat.name + L"] " + item.label;

			SearchCandidate candidate = {};
			candidate.visible = visible;
			candidate.distance = min(labelDistance, categoryDistance);
			candidate.sortKey = ToLowerCopy(visible.displayLabel);
			candidates.push_back(candidate);
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const SearchCandidate& left, const SearchCandidate& right)
	{
		if (left.distance != right.distance)
			return (left.distance < right.distance);

		if (left.sortKey != right.sortKey)
			return (left.sortKey < right.sortKey);

		if (left.visible.categoryIndex != right.visible.categoryIndex)
			return (left.visible.categoryIndex < right.visible.categoryIndex);

		return (left.visible.itemIndex < right.visible.itemIndex);
	});

	for (size_t i = 0; i < candidates.size(); i++)
		gPickerVisibleItems.push_back(candidates[i].visible);
}


/**
 * Expand environment variables in a path.
 */
static std::wstring ExpandEnvPath(LPCWSTR envPath)
{
	WCHAR expanded[MAX_PATH * 2] = {};
	DWORD len = ExpandEnvironmentStringsW(envPath, expanded, _countof(expanded));
	if ((len == 0) || (len > _countof(expanded)))
		return std::wstring();
	return std::wstring(expanded);
}


/**
 * Return absolute path of the current executable.
 */
static std::wstring GetCurrentExePath()
{
	WCHAR exePath[MAX_PATH] = {};
	if (!GetModuleFileNameW(NULL, exePath, _countof(exePath)))
		return std::wstring();
	return std::wstring(exePath);
}


/**
 * Return default Colors category index for the current OS.
 */
static int GetDefaultColorCategoryIndex()
{
	if (iconOffsetGlobal == WIN11_ICON_OFFSET)
		return PICKER_COLORS_WIN11;

	if (iconOffsetGlobal == WIN10_ICON_OFFSET)
		return PICKER_COLORS_WIN10;

	return PICKER_COLORS_WIN78;
}


/**
 * Add one built-in color category for a specific resource set.
 */
static void AddColorsCategory(std::vector<PickerCategory>& out, const WCHAR* categoryName, int builtInOffset)
{
	PickerCategory cat;
	cat.name = categoryName;
	for (int i = 0; i < COLOR_ICON_COUNT; i++)
	{
		PickerItem item = {};
		item.isBuiltIn = TRUE;
		item.builtInIndex = i;
		item.builtInOffset = builtInOffset;
		item.resourceIndex = 0;
		item.label = kColorNames[i];
		item.cachedIcon = NULL;
		item.ownsCachedIcon = FALSE;
		cat.items.push_back(item);
	}
	out.push_back(cat);
}


/**
 * Add one DLL/EXE category by enumerating all icon resources.
 */
static void AddDllCategory(std::vector<PickerCategory>& out, const std::wstring& categoryName, const std::wstring& filePath)
{
	if (filePath.empty())
		return;

	if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
		return;

	UINT iconCount = ExtractIconExW(filePath.c_str(), -1, NULL, NULL, 0);
	if ((iconCount == UINT_MAX) || (iconCount == 0))
		return;

	PickerCategory cat;
	cat.name = categoryName;
	for (UINT i = 0; i < iconCount; i++)
	{
		WCHAR label[64];
		swprintf_s(label, _countof(label), L"Icon %03u", i);

		PickerItem item = {};
		item.isBuiltIn = FALSE;
		item.builtInIndex = -1;
		item.builtInOffset = 0;
		item.resourcePath = filePath;
		item.resourceIndex = (int) i;
		item.label = label;
		item.cachedIcon = NULL;
		item.ownsCachedIcon = FALSE;
		cat.items.push_back(item);
	}

	out.push_back(cat);
}


/**
 * Add one ICO folder category (.ico files only).
 */
static void AddIcoFolderCategory(std::vector<PickerCategory>& out, const std::wstring& categoryName, const std::wstring& folderPath)
{
	std::vector<std::wstring> icoFiles;
	std::wstring pattern = folderPath + L"\\*.ico";
	WIN32_FIND_DATAW fd = {};
	HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			icoFiles.push_back(folderPath + L"\\" + fd.cFileName);
		}
		while (FindNextFileW(hFind, &fd));
		FindClose(hFind);
	}

	if (icoFiles.empty())
		return;

	PickerCategory cat;
	cat.name = categoryName;
	for (size_t i = 0; i < icoFiles.size(); i++)
	{
		PickerItem item = {};
		item.isBuiltIn = FALSE;
		item.builtInIndex = -1;
		item.builtInOffset = 0;
		item.resourcePath = icoFiles[i];
		item.resourceIndex = 0;
		item.label = PathFindFileNameW(icoFiles[i].c_str());
		item.cachedIcon = NULL;
		item.ownsCachedIcon = FALSE;
		cat.items.push_back(item);
	}

	out.push_back(cat);
}


/**
 * Format a relative icons path as a picker category label.
 */
static std::wstring FormatCustomCategoryPath(const std::wstring& relPath)
{
	if (relPath.empty())
		return L"Custom ICO";

	std::wstring label = relPath;
	for (size_t pos = 0; (pos = label.find(L'\\', pos)) != std::wstring::npos; pos += 3)
		label.replace(pos, 1, L" - ");

	return label;
}


/**
 * Recursively collect custom categories from install/icons.
 */
static void CollectCustomCategories(std::vector<PickerCategory>& out, const std::wstring& rootIconsPath, const std::wstring& dirPath)
{
	WCHAR relPath[MAX_PATH] = {};
	PathRelativePathToW(relPath, rootIconsPath.c_str(), FILE_ATTRIBUTE_DIRECTORY, dirPath.c_str(), FILE_ATTRIBUTE_DIRECTORY);
	std::wstring rel = relPath;
	if (!rel.empty() && (rel.size() >= 2) && (rel[0] == L'.') && (rel[1] == L'\\'))
		rel = rel.substr(2);
	if (rel == L".")
		rel.clear();

	std::wstring folderLabel = FormatCustomCategoryPath(rel);
	AddIcoFolderCategory(out, folderLabel, dirPath);

	std::wstring pattern = dirPath + L"\\*";
	WIN32_FIND_DATAW fd = {};
	HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if ((wcscmp(fd.cFileName, L".") == 0) || (wcscmp(fd.cFileName, L"..") == 0))
			continue;

		std::wstring full = dirPath + L"\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				CollectCustomCategories(out, rootIconsPath, full);
		}
		else
		{
			LPCWSTR ext = PathFindExtensionW(full.c_str());
			if (ext && (_wcsicmp(ext, L".dll") == 0))
			{
				std::wstring dllLabel = rel.empty()
					? std::wstring(fd.cFileName)
					: (FormatCustomCategoryPath(rel) + L" - " + fd.cFileName);
				AddDllCategory(out, dllLabel, full);
			}
		}
	}
	while (FindNextFileW(hFind, &fd));

	FindClose(hFind);
}


/**
 * Build all picker categories dynamically.
 */
static void BuildPickerCategories(std::vector<PickerCategory>& out)
{
	out.clear();
	AddColorsCategory(out, L"Windows 11 Colored", WIN11_ICON_OFFSET);
	AddColorsCategory(out, L"Windows 10 Colored", WIN10_ICON_OFFSET);
	AddColorsCategory(out, L"Windows 7/8 Colored", WIN7_ICON_OFFSET);

	for (UINT i = 0; i < _countof(kSystemDlls); i++)
	{
		std::wstring path = ExpandEnvPath(kSystemDlls[i].envPath);
		if (!path.empty())
			AddDllCategory(out, kSystemDlls[i].label, path);
	}

	std::wstring iconsRoot = std::wstring(myPathGlobal) + L"icons";
	DWORD attr = GetFileAttributesW(iconsRoot.c_str());
	if ((attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY))
		CollectCustomCategories(out, iconsRoot, iconsRoot);
}


/**
 * Populate the right-side icon list for the selected category.
 */
static void PopulatePickerItems(HWND hWnd, int categoryIndex)
{
	HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
	SendMessageW(hItem, LB_RESETCONTENT, 0, 0);

	RebuildPickerVisibleItems(categoryIndex);
	for (size_t i = 0; i < gPickerVisibleItems.size(); i++)
	{
		LRESULT idx = SendMessageW(hItem, LB_ADDSTRING, 0, (LPARAM) gPickerVisibleItems[i].displayLabel.c_str());
		if (idx != LB_ERR)
			SendMessageW(hItem, LB_SETITEMDATA, (WPARAM) idx, (LPARAM) i);
	}

	if (!gPickerVisibleItems.empty())
		SendMessageW(hItem, LB_SETCURSEL, 0, 0);
}


/**
 * Enable or disable picker delete button from current selection.
 */
static void UpdatePickerDeleteButtonState(HWND hWnd);


/**
 * Apply current search text to item list immediately.
 */
static void ApplyPickerSearchNow(HWND hWnd)
{
	UpdatePickerSearchQuery(hWnd);
	HWND hCat = GetDlgItem(hWnd, IDC_PICKER_CATEGORY);
	int sel = (int) SendMessageW(hCat, LB_GETCURSEL, 0, 0);
	PopulatePickerItems(hWnd, sel);
	UpdatePickerDeleteButtonState(hWnd);
}


/**
 * Populate the left-side categories list.
 */
static void PopulatePickerCategories(HWND hWnd)
{
	HWND hCat = GetDlgItem(hWnd, IDC_PICKER_CATEGORY);
	SendMessageW(hCat, LB_RESETCONTENT, 0, 0);

	for (size_t i = 0; i < gPickerCategories.size(); i++)
		SendMessageW(hCat, LB_ADDSTRING, 0, (LPARAM) gPickerCategories[i].name.c_str());

	if (!gPickerCategories.empty())
	{
		int defaultCategory = GetDefaultColorCategoryIndex();
		if ((defaultCategory < 0) || (defaultCategory >= (int) gPickerCategories.size()))
			defaultCategory = 0;

		SendMessageW(hCat, LB_SETCURSEL, defaultCategory, 0);
		UpdatePickerSearchQuery(hWnd);
		PopulatePickerItems(hWnd, defaultCategory);
	}
	else
	{
		HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
		SendMessageW(hItem, LB_RESETCONTENT, 0, 0);
		gPickerVisibleItems.clear();
	}
}


/**
 * Resolve item icon (lazy cache).
 */
static HICON ResolvePickerItemIcon(PickerItem& item)
{
	if (item.cachedIcon)
		return item.cachedIcon;

	if (item.isBuiltIn)
	{
		WCHAR exePath[MAX_PATH] = {};
		if (!GetModuleFileNameW(NULL, exePath, _countof(exePath)))
			return NULL;

		HICON largeIcon = NULL;
		HICON smallIcon = NULL;
		if (ExtractIconExW(exePath, item.builtInOffset + item.builtInIndex, &largeIcon, &smallIcon, 1) > 0)
		{
			if (largeIcon)
				DestroyIcon(largeIcon);
			item.cachedIcon = smallIcon;
			item.ownsCachedIcon = TRUE;
		}
		return item.cachedIcon;
	}

	LPCWSTR ext = PathFindExtensionW(item.resourcePath.c_str());
	if (ext && (_wcsicmp(ext, L".ico") == 0))
	{
		item.cachedIcon = (HICON) LoadImageW(NULL, item.resourcePath.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
		item.ownsCachedIcon = (item.cachedIcon != NULL);
		return item.cachedIcon;
	}

	HICON largeIcon = NULL;
	HICON smallIcon = NULL;
	if (ExtractIconExW(item.resourcePath.c_str(), item.resourceIndex, &largeIcon, &smallIcon, 1) > 0)
	{
		if (largeIcon)
			DestroyIcon(largeIcon);
		item.cachedIcon = smallIcon;
		item.ownsCachedIcon = TRUE;
	}

	return item.cachedIcon;
}


/**
 * Release the current preview icon, if owned.
 */
static void ReleasePickerPreviewIcon()
{
	if (gPickerPreviewOwnsIcon && gPickerPreviewIcon)
		DestroyIcon(gPickerPreviewIcon);
	gPickerPreviewIcon      = NULL;
	gPickerPreviewOwnsIcon  = FALSE;
	gPickerPreviewIsFolder  = FALSE;
	gPickerPreviewIsDefault = FALSE;
}

/**
 * Update the preview label text to reflect what is being shown.
 */
static void UpdatePickerPreviewLabel(HWND hWnd)
{
	HWND hLabel = GetDlgItem(hWnd, IDC_PICKER_PREVIEW_LABEL);
	if (!hLabel) return;
	if (gPickerPreviewIsDefault)
		SetWindowTextW(hLabel, L"Default OS icon  (click for current)");
	else if (gPickerPreviewIsFolder)
		SetWindowTextW(hLabel, L"Current icon  (click to restore)");
	else if (gPickerPreviewIcon)
		SetWindowTextW(hLabel, L"Preview  (click for current icon)");
	else
		SetWindowTextW(hLabel, L"Preview");
}

/**
 * Load the current icon of gPickerTargetFolder into the preview box.
 * Reads desktop.ini via SHGetSetFolderCustomSettings and then extracts
 * the largest available frame using PrivateExtractIconsW.
 * If the folder has no custom icon the preview is cleared.
 */
static void LoadFolderCurrentPreviewIcon()
{
	ReleasePickerPreviewIcon();

	if (gPickerTargetFolder.empty())
		return;

	// Read current icon assignment from folder's desktop.ini.
	SHFOLDERCUSTOMSETTINGS pfcs = {};
	pfcs.dwSize  = sizeof(SHFOLDERCUSTOMSETTINGS);
	pfcs.dwMask  = FCSM_ICONFILE;
	WCHAR iconPath[MAX_PATH] = {};
	pfcs.pszIconFile  = iconPath;
	pfcs.cchIconFile  = MAX_PATH;

	BOOL hasCustomIcon = SUCCEEDED(SHGetSetFolderCustomSettings(&pfcs, gPickerTargetFolder.c_str(), FCS_READ))
	                     && iconPath[0];

	if (hasCustomIcon)
	{
		HICON hIco  = NULL;
		UINT  iconId = 0;
		if (PrivateExtractIconsW(iconPath, pfcs.iIconIndex, 256, 256, &hIco, &iconId, 1, 0) > 0 && hIco)
		{
			gPickerPreviewIcon     = hIco;
			gPickerPreviewOwnsIcon = TRUE;
			gPickerPreviewIsFolder = TRUE;
		}
		return;
	}

	// No custom icon — show the default OS folder icon so the user can see
	// what the folder currently looks like before picking a color.
	SHSTOCKICONINFO sii = {};
	sii.cbSize = sizeof(sii);
	if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &sii)))
	{
		HICON hIco  = NULL;
		UINT  iconId = 0;
		if (PrivateExtractIconsW(sii.szPath, sii.iIcon, 256, 256, &hIco, &iconId, 1, 0) > 0 && hIco)
		{
			gPickerPreviewIcon      = hIco;
			gPickerPreviewOwnsIcon  = TRUE;
			gPickerPreviewIsFolder  = TRUE;
			gPickerPreviewIsDefault = TRUE;
			return;
		}
	}

	// Fallback path: ask shell directly for the folder icon handle.
	SHFILEINFOW sfi = {};
	if (SHGetFileInfoW(gPickerTargetFolder.c_str(), FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
		SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES) && sfi.hIcon)
	{
		gPickerPreviewIcon      = sfi.hIcon;
		gPickerPreviewOwnsIcon  = TRUE;
		gPickerPreviewIsFolder  = TRUE;
		gPickerPreviewIsDefault = TRUE;
	}
}

/**
 * Load the largest available icon frame (up to 256x256) for the given PickerItem
 * directly from the source resource, without upscaling the small listbox icon.
 * Uses PrivateExtractIconsW so the same file-offset index convention as
 * ExtractIconExW applies for both built-in (EXE) and external (DLL/ICO) sources.
 */
static void LoadPickerPreviewIcon(PickerItem& item)
{
	ReleasePickerPreviewIcon();

	WCHAR resourcePath[MAX_PATH] = {};
	int   resourceIndex = 0;

	if (item.isBuiltIn)
	{
		if (!GetModuleFileNameW(NULL, resourcePath, _countof(resourcePath)))
			return;
		resourceIndex = item.builtInOffset + item.builtInIndex;
	}
	else
	{
		wcsncpy_s(resourcePath, item.resourcePath.c_str(), _TRUNCATE);
		resourceIndex = item.resourceIndex;
	}

	HICON hLarge = NULL;
	UINT  iconId  = 0;
	// Request 256x256; Windows picks the best available frame from the icon data.
	if (PrivateExtractIconsW(resourcePath, resourceIndex, 256, 256, &hLarge, &iconId, 1, 0) > 0 && hLarge)
	{
		gPickerPreviewIcon     = hLarge;
		gPickerPreviewOwnsIcon = TRUE;
	}
}

/**
 * Release cached picker icons.
 */
static void ReleasePickerIcons()
{
	for (size_t c = 0; c < gPickerCategories.size(); c++)
	{
		for (size_t i = 0; i < gPickerCategories[c].items.size(); i++)
		{
			PickerItem& item = gPickerCategories[c].items[i];
			if (item.ownsCachedIcon && item.cachedIcon)
				DestroyIcon(item.cachedIcon);
			item.cachedIcon = NULL;
			item.ownsCachedIcon = FALSE;
		}
	}
}


/** Release COM pointers safely. */
template<typename T>
static void SafeReleaseCom(T*& ptr)
{
	if (ptr)
	{
		ptr->Release();
		ptr = NULL;
	}
}


#pragma pack(push, 1)
struct DerivedIcoHeader
{
	WORD idReserved;
	WORD idType;
	WORD idCount;
};

struct DerivedIcoEntry
{
	BYTE bWidth;
	BYTE bHeight;
	BYTE bColorCount;
	BYTE bReserved;
	WORD wPlanes;
	WORD wBitCount;
	DWORD dwBytesInRes;
	DWORD dwImageOffset;
};
#pragma pack(pop)


/** Build and ensure the derived icons folder path exists. */
static BOOL EnsureDerivedIconsFolder(std::wstring& outFolder)
{
	outFolder = std::wstring(myPathGlobal) + L"icons\\Derived Icons";
	int shres = SHCreateDirectoryExW(NULL, outFolder.c_str(), NULL);
	return (shres == ERROR_SUCCESS) || (shres == ERROR_ALREADY_EXISTS) || (shres == ERROR_FILE_EXISTS);
}


/** Build a destination path that does not collide with existing files. */
static std::wstring MakeUniqueDestinationPathMain(const std::wstring& dirPath, const std::wstring& fileName)
{
	WCHAR baseName[MAX_PATH] = {};
	WCHAR extension[MAX_PATH] = {};
	_wsplitpath_s(fileName.c_str(), NULL, 0, NULL, 0, baseName, _countof(baseName), extension, _countof(extension));

	std::wstring candidate = dirPath + L"\\" + fileName;
	if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
		return candidate;

	for (UINT suffix = 1; suffix < 10000; suffix++)
	{
		WCHAR uniqueName[MAX_PATH] = {};
		if (_snwprintf_s(uniqueName, _countof(uniqueName), (_countof(uniqueName) - 1), L"%s (%u)%s", baseName, suffix, extension) < 1)
			continue;

		candidate = dirPath + L"\\" + uniqueName;
		if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
			return candidate;
	}

	return std::wstring();
}


/** Return a copy trimmed from both ends by spaces, tabs, CR and LF. */
static std::wstring TrimWhitespaceCopy(const std::wstring& value)
{
	if (value.empty())
		return std::wstring();

	size_t start = 0;
	while ((start < value.size()) && iswspace(value[start]))
		start++;

	size_t end = value.size();
	while ((end > start) && iswspace(value[end - 1]))
		end--;

	return value.substr(start, end - start);
}


/** Remove an extension from one name-like value. */
static std::wstring RemoveExtensionCopy(const std::wstring& value)
{
	if (value.empty())
		return std::wstring();

	std::wstring name = value;
	LPCWSTR filePart = PathFindFileNameW(name.c_str());
	if (filePart && filePart[0])
		name = filePart;

	LPCWSTR ext = PathFindExtensionW(name.c_str());
	if (ext && ext[0])
		name.resize((size_t) (ext - name.c_str()));

	return name;
}


/** Return TRUE when one character is not allowed in a Windows file/folder name. */
static BOOL IsInvalidWindowsNameChar(WCHAR ch)
{
	if (ch < 32)
		return TRUE;

	return (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' ||
		ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*');
}


/** Sanitize one path segment for directory usage. */
static std::wstring SanitizeCategorySegment(const std::wstring& rawSegment)
{
	std::wstring trimmed = TrimWhitespaceCopy(rawSegment);
	if (trimmed.empty())
		return std::wstring();

	std::wstring sanitized;
	sanitized.reserve(trimmed.size());
	for (size_t i = 0; i < trimmed.size(); i++)
	{
		if (IsInvalidWindowsNameChar(trimmed[i]))
			continue;
		sanitized.push_back(trimmed[i]);
	}

	sanitized = TrimWhitespaceCopy(sanitized);
	while (!sanitized.empty() && ((sanitized.back() == L'.') || (sanitized.back() == L' ')))
		sanitized.pop_back();

	return sanitized;
}


/** Sanitize icon file stem (without extension) by removing invalid filename characters. */
static std::wstring SanitizeIconFileStem(const std::wstring& rawName)
{
	std::wstring trimmed = TrimWhitespaceCopy(rawName);
	std::wstring sanitized;
	sanitized.reserve(trimmed.size());
	for (size_t i = 0; i < trimmed.size(); i++)
	{
		if (IsInvalidWindowsNameChar(trimmed[i]))
			continue;
		sanitized.push_back(trimmed[i]);
	}

	sanitized = TrimWhitespaceCopy(sanitized);
	while (!sanitized.empty() && ((sanitized.back() == L'.') || (sanitized.back() == L' ')))
		sanitized.pop_back();

	return sanitized;
}


/** Build default icon name from base icon plus first user layer. */
static std::wstring BuildDefaultDerivedIconName()
{
	std::wstring baseName = RemoveExtensionCopy(gDerivedEditor.baseItem.label);
	baseName = SanitizeIconFileStem(baseName);
	if (baseName.empty())
		baseName = L"base";

	std::wstring firstOverlayName;
	if (gDerivedEditor.layers.size() > 1)
		firstOverlayName = RemoveExtensionCopy(gDerivedEditor.layers[1].displayName);

	firstOverlayName = SanitizeIconFileStem(firstOverlayName);
	if (firstOverlayName.empty())
		return baseName;

	std::wstring combined = baseName + L" " + firstOverlayName;
	combined = SanitizeIconFileStem(combined);
	if (combined.empty())
		combined = L"derived-icon";

	return combined;
}


/** Parse category input supporting both '/' and '\\' as nested folder separators. */
static std::vector<std::wstring> ParseCategorySegments(const std::wstring& categoryInput)
{
	std::vector<std::wstring> segments;
	std::wstring current;

	for (size_t i = 0; i < categoryInput.size(); i++)
	{
		WCHAR ch = categoryInput[i];
		if ((ch == L'/') || (ch == L'\\'))
		{
			std::wstring segment = SanitizeCategorySegment(current);
			if (!segment.empty())
				segments.push_back(segment);
			current.clear();
			continue;
		}

		current.push_back(ch);
	}

	std::wstring tail = SanitizeCategorySegment(current);
	if (!tail.empty())
		segments.push_back(tail);

	return segments;
}


/** Resolve and ensure icons\{Category} path exists from raw category input. */
static BOOL BuildDerivedCategoryFolderPath(const std::wstring& categoryInput, std::wstring& outFolder)
{
	outFolder.clear();

	std::vector<std::wstring> segments = ParseCategorySegments(categoryInput);
	if (segments.empty())
		return FALSE;

	std::wstring folderPath = std::wstring(myPathGlobal) + L"icons";
	int rootResult = SHCreateDirectoryExW(NULL, folderPath.c_str(), NULL);
	if (!((rootResult == ERROR_SUCCESS) || (rootResult == ERROR_ALREADY_EXISTS) || (rootResult == ERROR_FILE_EXISTS)))
		return FALSE;

	for (size_t i = 0; i < segments.size(); i++)
	{
		folderPath += L"\\";
		folderPath += segments[i];
	}

	int result = SHCreateDirectoryExW(NULL, folderPath.c_str(), NULL);
	if (!((result == ERROR_SUCCESS) || (result == ERROR_ALREADY_EXISTS) || (result == ERROR_FILE_EXISTS)))
		return FALSE;

	outFolder = folderPath;
	return TRUE;
}


/** Save dialog window procedure for Category/Name input. */
static LRESULT CALLBACK DerivedSaveDialogWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);

	switch (msg)
	{
		case WM_CREATE:
		{
			CreateWindowW(L"STATIC", L"Category", WS_CHILD | WS_VISIBLE,
				12, 12, 336, 18, hWnd, (HMENU) IDC_DERIVED_SAVE_CATEGORY_LABEL, NULL, NULL);
			CreateWindowW(L"EDIT", gDerivedSaveDialog.category.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				12, 32, 336, 24, hWnd, (HMENU) IDC_DERIVED_SAVE_CATEGORY_EDIT, NULL, NULL);

			CreateWindowW(L"STATIC", L"Name", WS_CHILD | WS_VISIBLE,
				12, 64, 336, 18, hWnd, (HMENU) IDC_DERIVED_SAVE_NAME_LABEL, NULL, NULL);
			CreateWindowW(L"EDIT", gDerivedSaveDialog.name.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				12, 84, 336, 24, hWnd, (HMENU) IDC_DERIVED_SAVE_NAME_EDIT, NULL, NULL);

			CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				176, 122, 82, 28, hWnd, (HMENU) IDC_DERIVED_SAVE_OK, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
				266, 122, 82, 28, hWnd, (HMENU) IDC_DERIVED_SAVE_CANCEL, NULL, NULL);

			SetFocus(GetDlgItem(hWnd, IDC_DERIVED_SAVE_NAME_EDIT));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SAVE_NAME_EDIT), EM_SETSEL, 0, -1);
			ApplyRuntimeTheme(hWnd);
			return 0;
		}

		case WM_THEMECHANGED:
		case WM_SYSCOLORCHANGE:
		case WM_SETTINGCHANGE:
			ApplyRuntimeTheme(hWnd);
			return 0;

		case WM_ERASEBKGND:
		{
			RECT rc = {};
			GetClientRect(hWnd, &rc);
			FillRect((HDC) wParam, &rc, gRuntimeBackgroundBrush ? gRuntimeBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			return 1;
		}

		case WM_CTLCOLORSTATIC:
		{
			HDC hdc = (HDC) wParam;
			SetTextColor(hdc, gRuntimeTextColor);
			SetBkColor(hdc, gRuntimeBackgroundColor);
			return (LRESULT) (gRuntimeBackgroundBrush ? gRuntimeBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		case WM_CTLCOLOREDIT:
		{
			HDC hdc = (HDC) wParam;
			SetTextColor(hdc, gRuntimeTextColor);
			SetBkColor(hdc, gRuntimePanelColor);
			return (LRESULT) (gRuntimePanelBrush ? gRuntimePanelBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDC_DERIVED_SAVE_OK:
				{
					WCHAR categoryBuf[512] = {};
					WCHAR nameBuf[512] = {};
					GetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_SAVE_CATEGORY_EDIT), categoryBuf, _countof(categoryBuf));
					GetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_SAVE_NAME_EDIT), nameBuf, _countof(nameBuf));

					std::wstring category = TrimWhitespaceCopy(categoryBuf);
					std::wstring name = TrimWhitespaceCopy(nameBuf);

					if (ParseCategorySegments(category).empty())
					{
						MessageBoxA(hWnd, "Category is required.", "Info:", MB_OK | MB_ICONINFORMATION);
						SetFocus(GetDlgItem(hWnd, IDC_DERIVED_SAVE_CATEGORY_EDIT));
						break;
					}

					name = SanitizeIconFileStem(name);
					if (name.empty())
					{
						MessageBoxA(hWnd, "Name is required.", "Info:", MB_OK | MB_ICONINFORMATION);
						SetFocus(GetDlgItem(hWnd, IDC_DERIVED_SAVE_NAME_EDIT));
						break;
					}

					gDerivedSaveDialog.category = category;
					gDerivedSaveDialog.name = name;
					gDerivedSaveDialog.confirmed = TRUE;
					DestroyWindow(hWnd);
				}
				break;

				case IDC_DERIVED_SAVE_CANCEL:
					DestroyWindow(hWnd);
					break;
			}
			return 0;

		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE)
			{
				DestroyWindow(hWnd);
				return 0;
			}
			break;

		case WM_CLOSE:
			DestroyWindow(hWnd);
			return 0;

		case WM_DESTROY:
			return 0;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}


/** Show modal save dialog and return selected Category and Name. */
static BOOL PromptDerivedSaveValues(HWND hParent, const std::wstring& initialCategory, const std::wstring& initialName,
	std::wstring& outCategory, std::wstring& outName)
{
	outCategory.clear();
	outName.clear();

	gDerivedSaveDialog = {};
	gDerivedSaveDialog.hParent = hParent;
	gDerivedSaveDialog.category = initialCategory;
	gDerivedSaveDialog.name = initialName;
	gDerivedSaveDialog.confirmed = FALSE;

	WNDCLASSW wc = {};
	wc.lpfnWndProc = DerivedSaveDialogWndProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"FoldrionDerivedSaveDialog";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconA((HINSTANCE) GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	HWND hWnd = CreateWindowExW(
		WS_EX_DLGMODALFRAME,
		wc.lpszClassName,
		L"Save Derived Icon",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 376, 200,
		hParent, NULL, wc.hInstance, NULL);

	if (!hWnd)
		return FALSE;

	gRuntimeThemeUsers++;
	gDerivedSaveDialog.hWnd = hWnd;
	EnableWindow(hParent, FALSE);
	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	MSG msg = {};
	while (IsWindow(hWnd) && (GetMessage(&msg, NULL, 0, 0) > 0))
	{
		if (!IsDialogMessage(hWnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	EnableWindow(hParent, TRUE);
	SetActiveWindow(hParent);

	if (gRuntimeThemeUsers > 0)
		gRuntimeThemeUsers--;
	ReleaseRuntimeThemeResourcesIfUnused();

	if (!gDerivedSaveDialog.confirmed)
		return FALSE;

	outCategory = gDerivedSaveDialog.category;
	outName = gDerivedSaveDialog.name;
	return TRUE;
}


/** Convert any supported image file to 32bpp BGRA pixels. */
static HRESULT LoadImageFileAsBgra(LPCWSTR sourcePath, std::vector<BYTE>& pixels, UINT& width, UINT& height)
{
	width = 0;
	height = 0;
	pixels.clear();

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL shouldUninitialize = SUCCEEDED(hr);
	if ((hr != S_OK) && (hr != S_FALSE) && (hr != RPC_E_CHANGED_MODE))
		return hr;

	IWICImagingFactory* factory = NULL;
	IWICBitmapDecoder* decoder = NULL;
	IWICBitmapFrameDecode* frame = NULL;
	IWICFormatConverter* converter = NULL;

	do
	{
		hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (FAILED(hr))
			break;

		hr = factory->CreateDecoderFromFilename(sourcePath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
		if (FAILED(hr))
			break;

		hr = decoder->GetFrame(0, &frame);
		if (FAILED(hr))
			break;

		hr = frame->GetSize(&width, &height);
		if (FAILED(hr) || (width == 0) || (height == 0))
		{
			if (SUCCEEDED(hr))
				hr = E_FAIL;
			break;
		}

		hr = factory->CreateFormatConverter(&converter);
		if (FAILED(hr))
			break;

		hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
		if (FAILED(hr))
			break;

		UINT stride = width * 4;
		pixels.resize((size_t) stride * height);
		hr = converter->CopyPixels(NULL, stride, (UINT) pixels.size(), pixels.data());
	}
	while (FALSE);

	SafeReleaseCom(converter);
	SafeReleaseCom(frame);
	SafeReleaseCom(decoder);
	SafeReleaseCom(factory);

	if (shouldUninitialize)
		CoUninitialize();

	if (FAILED(hr))
	{
		width = 0;
		height = 0;
		pixels.clear();
	}

	return hr;
}


/** Render one extracted icon frame as a square BGRA bitmap. */
static BOOL RenderIconToSquareBgra(HICON hIcon, UINT size, std::vector<BYTE>& pixels)
{
	pixels.assign((size_t) size * size * 4, 0);

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG) size;
	bmi.bmiHeader.biHeight = -(LONG) size;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* dibBits = NULL;
	HDC screenDc = GetDC(NULL);
	if (!screenDc)
		return FALSE;

	HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, NULL, 0);
	if (!dib)
	{
		ReleaseDC(NULL, screenDc);
		return FALSE;
	}

	HDC memDc = CreateCompatibleDC(screenDc);
	if (!memDc)
	{
		DeleteObject(dib);
		ReleaseDC(NULL, screenDc);
		return FALSE;
	}

	HGDIOBJ oldBitmap = SelectObject(memDc, dib);
	PatBlt(memDc, 0, 0, size, size, BLACKNESS);
	DrawIconEx(memDc, 0, 0, hIcon, size, size, 0, NULL, DI_NORMAL);
	memcpy(pixels.data(), dibBits, pixels.size());

	SelectObject(memDc, oldBitmap);
	DeleteDC(memDc);
	DeleteObject(dib);
	ReleaseDC(NULL, screenDc);
	return TRUE;
}


/** Load a picker item as a square 256x256 BGRA base image. */
static BOOL LoadPickerItemAsBaseBgra(const PickerItem& item, std::vector<BYTE>& pixels)
{
	WCHAR resourcePath[MAX_PATH] = {};
	int resourceIndex = 0;

	if (item.isBuiltIn)
	{
		if (!GetModuleFileNameW(NULL, resourcePath, _countof(resourcePath)))
			return FALSE;
		resourceIndex = item.builtInOffset + item.builtInIndex;
	}
	else
	{
		wcsncpy_s(resourcePath, item.resourcePath.c_str(), _TRUNCATE);
		resourceIndex = item.resourceIndex;
	}

	HICON hLarge = NULL;
	UINT iconId = 0;
	if (PrivateExtractIconsW(resourcePath, resourceIndex, 256, 256, &hLarge, &iconId, 1, 0) <= 0 || !hLarge)
		return FALSE;

	BOOL ok = RenderIconToSquareBgra(hLarge, 256, pixels);
	DestroyIcon(hLarge);
	return ok;
}


/** Resize a square BGRA image to another square size using nearest-neighbor. */
static std::vector<BYTE> ResizeSquareNearest(const std::vector<BYTE>& src, UINT srcSize, UINT dstSize)
{
	std::vector<BYTE> dst((size_t) dstSize * dstSize * 4, 0);
	for (UINT y = 0; y < dstSize; y++)
	{
		UINT sy = (UINT) (((UINT64) y * srcSize) / dstSize);
		if (sy >= srcSize) sy = srcSize - 1;
		for (UINT x = 0; x < dstSize; x++)
		{
			UINT sx = (UINT) (((UINT64) x * srcSize) / dstSize);
			if (sx >= srcSize) sx = srcSize - 1;
			const BYTE* s = &src[((size_t) sy * srcSize + sx) * 4];
			BYTE* d = &dst[((size_t) y * dstSize + x) * 4];
			d[0] = s[0];
			d[1] = s[1];
			d[2] = s[2];
			d[3] = s[3];
		}
	}
	return dst;
}


/** Rotate hue and scale saturation for one RGB pixel. */
static void AdjustHueSaturation(BYTE& b, BYTE& g, BYTE& r, int hueDegrees, int saturationPercent)
{
	double rf = (double) r / 255.0;
	double gf = (double) g / 255.0;
	double bf = (double) b / 255.0;

	double maxc = max(rf, max(gf, bf));
	double minc = min(rf, min(gf, bf));
	double delta = maxc - minc;

	double h = 0.0;
	if (delta > 0.000001)
	{
		if (maxc == rf)
			h = 60.0 * fmod(((gf - bf) / delta), 6.0);
		else if (maxc == gf)
			h = 60.0 * (((bf - rf) / delta) + 2.0);
		else
			h = 60.0 * (((rf - gf) / delta) + 4.0);
	}
	if (h < 0.0)
		h += 360.0;

	double s = (maxc <= 0.0) ? 0.0 : (delta / maxc);
	double v = maxc;

	h += (double) hueDegrees;
	while (h < 0.0) h += 360.0;
	while (h >= 360.0) h -= 360.0;

	s *= ((double) saturationPercent / 100.0);
	if (s < 0.0) s = 0.0;
	if (s > 1.0) s = 1.0;

	double c = v * s;
	double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
	double m = v - c;
	double rr = 0.0, gg = 0.0, bb = 0.0;

	if (h < 60.0) { rr = c; gg = x; bb = 0.0; }
	else if (h < 120.0) { rr = x; gg = c; bb = 0.0; }
	else if (h < 180.0) { rr = 0.0; gg = c; bb = x; }
	else if (h < 240.0) { rr = 0.0; gg = x; bb = c; }
	else if (h < 300.0) { rr = x; gg = 0.0; bb = c; }
	else { rr = c; gg = 0.0; bb = x; }

	r = (BYTE) min(255.0, max(0.0, (rr + m) * 255.0));
	g = (BYTE) min(255.0, max(0.0, (gg + m) * 255.0));
	b = (BYTE) min(255.0, max(0.0, (bb + m) * 255.0));
}


/** Colorize grayscale-like pixels using the selected hue and saturation amount. */
static void ColorizePixelIfGray(BYTE& b, BYTE& g, BYTE& r, int hueDegrees, int saturationPercent)
{
	int rb = abs((int) r - (int) b);
	int rg = abs((int) r - (int) g);
	int gb = abs((int) g - (int) b);
	if ((rb > 6) || (rg > 6) || (gb > 6))
		return;

	double hue = (double) hueDegrees;
	while (hue < 0.0) hue += 360.0;
	while (hue >= 360.0) hue -= 360.0;
	double sat = (double) max(0, min(200, saturationPercent)) / 100.0;
	if (sat > 1.0) sat = 1.0;

	double value = max((double) r, max((double) g, (double) b)) / 255.0;
	double c = value * sat;
	double x = c * (1.0 - fabs(fmod(hue / 60.0, 2.0) - 1.0));
	double m = value - c;
	double rr = 0.0, gg = 0.0, bb = 0.0;

	if (hue < 60.0) { rr = c; gg = x; bb = 0.0; }
	else if (hue < 120.0) { rr = x; gg = c; bb = 0.0; }
	else if (hue < 180.0) { rr = 0.0; gg = c; bb = x; }
	else if (hue < 240.0) { rr = 0.0; gg = x; bb = c; }
	else if (hue < 300.0) { rr = x; gg = 0.0; bb = c; }
	else { rr = c; gg = 0.0; bb = x; }

	r = (BYTE) min(255.0, max(0.0, (rr + m) * 255.0));
	g = (BYTE) min(255.0, max(0.0, (gg + m) * 255.0));
	b = (BYTE) min(255.0, max(0.0, (bb + m) * 255.0));
}


/** Alpha-composite one source pixel over one destination pixel. */
static void BlendPixelOver(BYTE* dst, BYTE sb, BYTE sg, BYTE sr, BYTE sa)
{
	double srcA = (double) sa / 255.0;
	double dstA = (double) dst[3] / 255.0;
	double outA = srcA + (dstA * (1.0 - srcA));
	if (outA <= 0.000001)
	{
		dst[0] = dst[1] = dst[2] = dst[3] = 0;
		return;
	}

	double outB = (((double) sb * srcA) + ((double) dst[0] * dstA * (1.0 - srcA))) / outA;
	double outG = (((double) sg * srcA) + ((double) dst[1] * dstA * (1.0 - srcA))) / outA;
	double outR = (((double) sr * srcA) + ((double) dst[2] * dstA * (1.0 - srcA))) / outA;

	dst[0] = (BYTE) min(255.0, max(0.0, outB));
	dst[1] = (BYTE) min(255.0, max(0.0, outG));
	dst[2] = (BYTE) min(255.0, max(0.0, outR));
	dst[3] = (BYTE) min(255.0, max(0.0, outA * 255.0));
}


/** Rebuild composed preview pixels from base icon plus all overlay layers. */
static void RebuildDerivedComposite()
{
	gDerivedEditor.composedPixels.assign((size_t) 256 * 256 * 4, 0);

	for (size_t layerIndex = 0; layerIndex < gDerivedEditor.layers.size(); layerIndex++)
	{
		const DerivedLayer& layer = gDerivedEditor.layers[layerIndex];
		if (layer.pixels.empty() || (layer.width == 0) || (layer.height == 0))
			continue;

		double fit = min(256.0 / (double) layer.width, 256.0 / (double) layer.height);
		double scale = fit * ((double) layer.scale / 100.0);
		int drawW = max(1, (int) ((double) layer.width * scale + 0.5));
		int drawH = max(1, (int) ((double) layer.height * scale + 0.5));
		int startX = ((256 - drawW) / 2) + layer.posX;
		int startY = ((256 - drawH) / 2) + layer.posY;
		double centerX = (double) startX + ((double) drawW * 0.5);
		double centerY = (double) startY + ((double) drawH * 0.5);
		double halfW = (double) drawW * 0.5;
		double halfH = (double) drawH * 0.5;
		double angleRad = ((double) layer.rotation * 3.14159265358979323846) / 180.0;
		double cosT = cos(angleRad);
		double sinT = sin(angleRad);

		for (int y = 0; y < drawH; y++)
		{
			int dstY = startY + y;
			if ((dstY < 0) || (dstY >= 256))
				continue;

			for (int x = 0; x < drawW; x++)
			{
				int dstX = startX + x;
				if ((dstX < 0) || (dstX >= 256))
					continue;

				double localX = ((double) dstX + 0.5) - centerX;
				double localY = ((double) dstY + 0.5) - centerY;
				double srcLocalX = (localX * cosT) + (localY * sinT);
				double srcLocalY = (-localX * sinT) + (localY * cosT);
				double srcNormX = srcLocalX / halfW;
				double srcNormY = srcLocalY / halfH;

				if ((srcNormX < -1.0) || (srcNormX > 1.0) || (srcNormY < -1.0) || (srcNormY > 1.0))
					continue;

				double srcFx = ((srcNormX + 1.0) * 0.5) * (double) (layer.width - 1);
				double srcFy = ((srcNormY + 1.0) * 0.5) * (double) (layer.height - 1);
				UINT srcX = (UINT) min((double) (layer.width - 1), max(0.0, srcFx + 0.5));
				UINT srcY = (UINT) min((double) (layer.height - 1), max(0.0, srcFy + 0.5));

				const BYTE* srcPx = &layer.pixels[((size_t) srcY * layer.width + srcX) * 4];
				BYTE b = srcPx[0];
				BYTE g = srcPx[1];
				BYTE r = srcPx[2];
				BYTE a = srcPx[3];

				AdjustHueSaturation(b, g, r, layer.hue, layer.saturation);
				if (layer.colorize)
					ColorizePixelIfGray(b, g, r, layer.hue, layer.saturation);

				if (!layer.isBase)
					a = (BYTE) (((UINT) a * (UINT) layer.opacity) / 100U);

				BYTE* dstPx = &gDerivedEditor.composedPixels[((size_t) dstY * 256 + dstX) * 4];
				BlendPixelOver(dstPx, b, g, r, a);
			}
		}
	}
}


/** Encode one square BGRA bitmap to a PNG byte buffer through WIC. */
static HRESULT EncodeSquarePngMain(const std::vector<BYTE>& pixels, UINT canvasSize, std::vector<BYTE>& pngData)
{
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL shouldUninitialize = SUCCEEDED(hr);
	if ((hr != S_OK) && (hr != S_FALSE) && (hr != RPC_E_CHANGED_MODE))
		return hr;

	IWICImagingFactory* factory = NULL;
	IWICBitmap* bitmap = NULL;
	IWICBitmapEncoder* encoder = NULL;
	IWICBitmapFrameEncode* frame = NULL;
	IPropertyBag2* props = NULL;
	IStream* stream = NULL;

	do
	{
		hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (FAILED(hr))
			break;

		UINT stride = canvasSize * 4;
		hr = factory->CreateBitmapFromMemory(canvasSize, canvasSize, GUID_WICPixelFormat32bppBGRA, stride, (UINT) pixels.size(), const_cast<BYTE*>(pixels.data()), &bitmap);
		if (FAILED(hr))
			break;

		hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
		if (FAILED(hr))
			break;

		hr = factory->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder);
		if (FAILED(hr))
			break;

		hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		if (FAILED(hr))
			break;

		hr = encoder->CreateNewFrame(&frame, &props);
		if (FAILED(hr))
			break;

		hr = frame->Initialize(props);
		if (FAILED(hr))
			break;

		hr = frame->SetSize(canvasSize, canvasSize);
		if (FAILED(hr))
			break;

		WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
		hr = frame->SetPixelFormat(&pixelFormat);
		if (FAILED(hr))
			break;

		hr = frame->WriteSource(bitmap, NULL);
		if (FAILED(hr))
			break;

		hr = frame->Commit();
		if (FAILED(hr))
			break;

		hr = encoder->Commit();
		if (FAILED(hr))
			break;

		STATSTG stat = {};
		hr = stream->Stat(&stat, STATFLAG_NONAME);
		if (FAILED(hr))
			break;

		pngData.resize((size_t) stat.cbSize.QuadPart);
		LARGE_INTEGER zero = {};
		hr = stream->Seek(zero, STREAM_SEEK_SET, NULL);
		if (FAILED(hr))
			break;

		ULONG bytesRead = 0;
		hr = stream->Read(pngData.data(), (ULONG) pngData.size(), &bytesRead);
		if (FAILED(hr))
			break;

		if (bytesRead != pngData.size())
		{
			hr = E_FAIL;
			break;
		}
	}
	while (FALSE);

	SafeReleaseCom(props);
	SafeReleaseCom(frame);
	SafeReleaseCom(encoder);
	SafeReleaseCom(stream);
	SafeReleaseCom(bitmap);
	SafeReleaseCom(factory);

	if (shouldUninitialize)
		CoUninitialize();

	return hr;
}


/** Write a multi-size PNG-backed ICO file. */
static HRESULT WriteMultiSizePngIcoFile(LPCWSTR targetPath, const std::vector<BYTE>& src256)
{
	static const UINT kSizes[] = { 16, 24, 32, 48, 64, 128, 256 };
	std::vector<std::vector<BYTE> > pngPayloads;
	pngPayloads.reserve(_countof(kSizes));

	for (UINT i = 0; i < _countof(kSizes); i++)
	{
		std::vector<BYTE> resized = (kSizes[i] == 256) ? src256 : ResizeSquareNearest(src256, 256, kSizes[i]);
		std::vector<BYTE> pngData;
		HRESULT hr = EncodeSquarePngMain(resized, kSizes[i], pngData);
		if (FAILED(hr))
			return hr;
		pngPayloads.push_back(pngData);
	}

	DerivedIcoHeader header = { 0, 1, (WORD) _countof(kSizes) };
	std::vector<DerivedIcoEntry> entries(_countof(kSizes));
	DWORD dataOffset = (DWORD) (sizeof(DerivedIcoHeader) + (sizeof(DerivedIcoEntry) * _countof(kSizes)));

	for (UINT i = 0; i < _countof(kSizes); i++)
	{
		DerivedIcoEntry entry = {};
		entry.bWidth = (kSizes[i] >= 256) ? 0 : (BYTE) kSizes[i];
		entry.bHeight = (kSizes[i] >= 256) ? 0 : (BYTE) kSizes[i];
		entry.bColorCount = 0;
		entry.bReserved = 0;
		entry.wPlanes = 1;
		entry.wBitCount = 32;
		entry.dwBytesInRes = (DWORD) pngPayloads[i].size();
		entry.dwImageOffset = dataOffset;
		entries[i] = entry;
		dataOffset += entry.dwBytesInRes;
	}

	HANDLE file = CreateFileW(targetPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

	HRESULT hr = S_OK;
	DWORD written = 0;
	if (!WriteFile(file, &header, sizeof(header), &written, NULL) || (written != sizeof(header)))
		hr = HRESULT_FROM_WIN32(GetLastError());

	for (UINT i = 0; SUCCEEDED(hr) && (i < entries.size()); i++)
	{
		if (!WriteFile(file, &entries[i], sizeof(DerivedIcoEntry), &written, NULL) || (written != sizeof(DerivedIcoEntry)))
			hr = HRESULT_FROM_WIN32(GetLastError());
	}

	for (UINT i = 0; SUCCEEDED(hr) && (i < pngPayloads.size()); i++)
	{
		if (!WriteFile(file, pngPayloads[i].data(), (DWORD) pngPayloads[i].size(), &written, NULL) || (written != pngPayloads[i].size()))
			hr = HRESULT_FROM_WIN32(GetLastError());
	}

	CloseHandle(file);
	if (FAILED(hr))
		DeleteFileW(targetPath);

	return hr;
}


/** Build one-line text for a layer list entry. */
static std::wstring BuildLayerListLabel(const DerivedLayer& layer)
{
	WCHAR text[512] = {};
	if (_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"%s%s  [H:%d S:%d%% O:%d%% C:%s Sc:%d%% X:%d Y:%d R:%d]",
		layer.displayName.c_str(), layer.isBase ? L" (Fixed)" : L"", layer.hue, layer.saturation, layer.opacity,
		layer.colorize ? L"On" : L"Off", layer.scale, layer.posX, layer.posY, layer.rotation) < 1)
		return layer.displayName;
	return text;
}


/** Write an integer into an edit control. */
static void SetEditIntValue(HWND hWnd, int controlId, int value)
{
	WCHAR text[32] = {};
	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"%d", value);
	HWND hEdit = GetDlgItem(hWnd, controlId);
	if (!hEdit)
		return;
	WCHAR current[32] = {};
	GetWindowTextW(hEdit, current, _countof(current));
	if (wcscmp(current, text) != 0)
		SetWindowTextW(hEdit, text);
}


/** Parse integer from an edit control, clamped to range. */
static int GetEditIntValue(HWND hWnd, int controlId, int minValue, int maxValue, int fallback)
{
	WCHAR text[64] = {};
	GetWindowTextW(GetDlgItem(hWnd, controlId), text, _countof(text));
	if (!text[0])
		return fallback;
	int value = _wtoi(text);
	if (value < minValue) value = minValue;
	if (value > maxValue) value = maxValue;
	return value;
}


/** Return preview image rect (drawn icon area) in parent client coordinates. */
static BOOL GetDerivedPreviewImageRect(HWND hWnd, RECT* outRect)
{
	if (!outRect)
		return FALSE;
	HWND hPreview = GetDlgItem(hWnd, IDC_DERIVED_PREVIEW);
	if (!hPreview)
		return FALSE;

	RECT rc = {};
	if (!GetWindowRect(hPreview, &rc))
		return FALSE;
	POINT tl = { rc.left, rc.top };
	POINT br = { rc.right, rc.bottom };
	ScreenToClient(hWnd, &tl);
	ScreenToClient(hWnd, &br);

	int boxW = br.x - tl.x;
	int boxH = br.y - tl.y;
	int draw = min(boxW, boxH) - 8;
	if (draw < 1)
		draw = 1;

	outRect->left = tl.x + (boxW - draw) / 2;
	outRect->top = tl.y + (boxH - draw) / 2;
	outRect->right = outRect->left + draw;
	outRect->bottom = outRect->top + draw;
	return TRUE;
}


static int GetSelectedDerivedLayerIndex(HWND hWnd);
static void UpdateDerivedSliderLabels(HWND hWnd, int layerIndex);
static void RefreshDerivedLayerList(HWND hWnd);


/** Apply slider + edit values into selected layer and refresh preview/list. */
static void CommitDerivedLayerValues(HWND hWnd, BOOL fromEdits)
{
	int idx = GetSelectedDerivedLayerIndex(hWnd);
	if (idx < 0)
		return;

	DerivedLayer& layer = gDerivedEditor.layers[idx];
	if (fromEdits)
	{
		layer.hue = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_HUE, -180, 180, layer.hue);
		layer.saturation = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_SATURATION, 0, 200, layer.saturation);
		layer.opacity = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_OPACITY, 0, 100, layer.opacity);
		layer.scale = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_SCALE, 10, 300, layer.scale);
		layer.posX = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_POSX, -128, 128, layer.posX);
		layer.posY = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_POSY, -128, 128, layer.posY);
		layer.rotation = GetEditIntValue(hWnd, IDC_DERIVED_EDIT_ROTATION, 0, 360, layer.rotation);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_HUE), TBM_SETPOS, TRUE, layer.hue);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SATURATION), TBM_SETPOS, TRUE, layer.saturation);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_OPACITY), TBM_SETPOS, TRUE, layer.opacity);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SCALE), TBM_SETPOS, TRUE, layer.scale);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSX), TBM_SETPOS, TRUE, layer.posX);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSY), TBM_SETPOS, TRUE, layer.posY);
		SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_ROTATION), TBM_SETPOS, TRUE, layer.rotation);
	}
	else
	{
		layer.hue = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_HUE), TBM_GETPOS, 0, 0);
		layer.saturation = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SATURATION), TBM_GETPOS, 0, 0);
		layer.opacity = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_OPACITY), TBM_GETPOS, 0, 0);
		layer.scale = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SCALE), TBM_GETPOS, 0, 0);
		layer.posX = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSX), TBM_GETPOS, 0, 0);
		layer.posY = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSY), TBM_GETPOS, 0, 0);
		layer.rotation = (int) SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_ROTATION), TBM_GETPOS, 0, 0);
		gDerivedEditor.syncingControls = TRUE;
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_HUE, layer.hue);
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_SATURATION, layer.saturation);
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_OPACITY, layer.opacity);
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_SCALE, layer.scale);
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_POSX, layer.posX);
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_POSY, layer.posY);
		SetEditIntValue(hWnd, IDC_DERIVED_EDIT_ROTATION, layer.rotation);
		gDerivedEditor.syncingControls = FALSE;
	}

	layer.colorize = (IsDlgButtonChecked(hWnd, IDC_DERIVED_COLORIZE) == BST_CHECKED);
	if (layer.isBase)
	{
		layer.opacity = 100;
		layer.rotation = 0;
	}

	UpdateDerivedSliderLabels(hWnd, idx);
	RefreshDerivedLayerList(hWnd);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_LAYER_LIST), LB_SETCURSEL, idx, 0);
	RebuildDerivedComposite();
	InvalidateRect(GetDlgItem(hWnd, IDC_DERIVED_PREVIEW), NULL, TRUE);
}


/** Refresh layer listbox from the editor state. */
static void RefreshDerivedLayerList(HWND hWnd)
{
	HWND hList = GetDlgItem(hWnd, IDC_DERIVED_LAYER_LIST);
	if (!hList)
		return;

	int oldSel = (int) SendMessageW(hList, LB_GETCURSEL, 0, 0);
	SendMessageW(hList, LB_RESETCONTENT, 0, 0);

	for (size_t i = 0; i < gDerivedEditor.layers.size(); i++)
		SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM) BuildLayerListLabel(gDerivedEditor.layers[i]).c_str());

	if (!gDerivedEditor.layers.empty())
	{
		if ((oldSel < 0) || (oldSel >= (int) gDerivedEditor.layers.size()))
			oldSel = (int) gDerivedEditor.layers.size() - 1;
		SendMessageW(hList, LB_SETCURSEL, (WPARAM) oldSel, 0);
	}
}


/** Return currently selected layer index or -1 when no layer is selected. */
static int GetSelectedDerivedLayerIndex(HWND hWnd)
{
	HWND hList = GetDlgItem(hWnd, IDC_DERIVED_LAYER_LIST);
	if (!hList)
		return -1;
	int idx = (int) SendMessageW(hList, LB_GETCURSEL, 0, 0);
	if ((idx < 0) || (idx >= (int) gDerivedEditor.layers.size()))
		return -1;
	return idx;
}


/** Update text labels attached to editor sliders. */
static void UpdateDerivedSliderLabels(HWND hWnd, int layerIndex)
{
	if ((layerIndex < 0) || (layerIndex >= (int) gDerivedEditor.layers.size()))
		return;

	const DerivedLayer& layer = gDerivedEditor.layers[layerIndex];
	WCHAR text[96] = {};

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Hue: %d", layer.hue);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_HUE), text);

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Saturation: %d%%", layer.saturation);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_SATURATION), text);

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Opacity: %d%%", layer.opacity);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_OPACITY), text);

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Scale: %d%%", layer.scale);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_SCALE), text);

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Position X: %d", layer.posX);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_POSX), text);

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Position Y: %d", layer.posY);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_POSY), text);

	_snwprintf_s(text, _countof(text), (_countof(text) - 1), L"Rotation: %d\xB0", layer.rotation);
	SetWindowTextW(GetDlgItem(hWnd, IDC_DERIVED_LABEL_ROTATION), text);

	gDerivedEditor.syncingControls = TRUE;
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_HUE, layer.hue);
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_SATURATION, layer.saturation);
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_OPACITY, layer.opacity);
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_SCALE, layer.scale);
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_POSX, layer.posX);
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_POSY, layer.posY);
	SetEditIntValue(hWnd, IDC_DERIVED_EDIT_ROTATION, layer.rotation);
	gDerivedEditor.syncingControls = FALSE;
}


/** Push selected layer values into slider controls. */
static void SyncDerivedControlsFromSelection(HWND hWnd)
{
	int idx = GetSelectedDerivedLayerIndex(hWnd);
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_REMOVE_LAYER), (idx >= 0));
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_MOVE_UP), (idx > 1));
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_MOVE_DOWN), (idx > 0) && (idx < ((int) gDerivedEditor.layers.size() - 1)));

	if (idx < 0)
		return;

	const DerivedLayer& layer = gDerivedEditor.layers[idx];
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_HUE), TBM_SETPOS, TRUE, layer.hue);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SATURATION), TBM_SETPOS, TRUE, layer.saturation);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_OPACITY), TBM_SETPOS, TRUE, layer.opacity);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SCALE), TBM_SETPOS, TRUE, layer.scale);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSX), TBM_SETPOS, TRUE, layer.posX);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSY), TBM_SETPOS, TRUE, layer.posY);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_ROTATION), TBM_SETPOS, TRUE, layer.rotation);
	CheckDlgButton(hWnd, IDC_DERIVED_COLORIZE, layer.colorize ? BST_CHECKED : BST_UNCHECKED);
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_REMOVE_LAYER), (idx > 0));
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_OPACITY), !layer.isBase);
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_EDIT_OPACITY), !layer.isBase);
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_ROTATION), !layer.isBase);
	EnableWindow(GetDlgItem(hWnd, IDC_DERIVED_EDIT_ROTATION), !layer.isBase);
	UpdateDerivedSliderLabels(hWnd, idx);
}


/** Read current slider values and write them back into selected layer. */
static void ApplyDerivedControlsToSelection(HWND hWnd)
{
	CommitDerivedLayerValues(hWnd, FALSE);
}


/** Ask user to choose one PNG/ICO layer file and append it to the editor. */
static void AddDerivedLayerFromFile(HWND hWnd)
{
	WCHAR buffer[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	static const WCHAR filter[] =
		L"Layer files (*.png;*.ico)\0*.png;*.ico\0"
		L"PNG files (*.png)\0*.png\0"
		L"Icon files (*.ico)\0*.ico\0\0";

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = buffer;
	ofn.nMaxFile = _countof(buffer);
	ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
	ofn.lpstrTitle = L"Add Layer";

	if (!GetOpenFileNameW(&ofn))
		return;

	std::vector<BYTE> pixels;
	UINT width = 0;
	UINT height = 0;
	if (FAILED(LoadImageFileAsBgra(buffer, pixels, width, height)))
	{
		MessageBoxA(hWnd, "Unable to load layer file.", "Error:", MB_OK | MB_ICONERROR);
		return;
	}

	DerivedLayer layer = {};
	layer.sourcePath = buffer;
	layer.displayName = PathFindFileNameW(buffer);
	layer.pixels.swap(pixels);
	layer.width = width;
	layer.height = height;
	layer.hue = 0;
	layer.saturation = 100;
	layer.opacity = 100;
	layer.scale = 100;
	layer.posX = 0;
	layer.posY = 0;
	layer.rotation = 0;
	layer.colorize = FALSE;
	layer.isBase = FALSE;

	gDerivedEditor.layers.push_back(layer);
	RefreshDerivedLayerList(hWnd);
	SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_LAYER_LIST), LB_SETCURSEL, (WPARAM) (gDerivedEditor.layers.size() - 1), 0);
	SyncDerivedControlsFromSelection(hWnd);
	RebuildDerivedComposite();
	InvalidateRect(GetDlgItem(hWnd, IDC_DERIVED_PREVIEW), NULL, TRUE);
}


/** Save composed icon into icons\\Derived Icons with required name and unique suffix on conflict. */
static BOOL SaveDerivedIconFromEditor(HWND hWnd)
{
	std::wstring defaultCategory = L"Derived Icons";
	std::wstring defaultName = BuildDefaultDerivedIconName();
	if (defaultName.empty())
		defaultName = L"derived-icon";

	std::wstring categoryValue;
	std::wstring nameValue;
	if (!PromptDerivedSaveValues(hWnd, defaultCategory, defaultName, categoryValue, nameValue))
		return FALSE;

	std::wstring targetFolder;
	if (!BuildDerivedCategoryFolderPath(categoryValue, targetFolder))
	{
		MessageBoxA(hWnd, "Unable to create the requested category path under icons.", "Error:", MB_OK | MB_ICONERROR);
		return FALSE;
	}

	std::wstring fileStem = SanitizeIconFileStem(nameValue);
	if (fileStem.empty())
		fileStem = defaultName;

	std::wstring targetPath = MakeUniqueDestinationPathMain(targetFolder, fileStem + L".ico");
	if (targetPath.empty())
	{
		MessageBoxA(hWnd, "Unable to create unique target file name.", "Error:", MB_OK | MB_ICONERROR);
		return FALSE;
	}

	RebuildDerivedComposite();
	HRESULT hr = WriteMultiSizePngIcoFile(targetPath.c_str(), gDerivedEditor.composedPixels);
	if (FAILED(hr))
	{
		MessageBoxA(hWnd, "Unable to save icon file.", "Error:", MB_OK | MB_ICONERROR);
		return FALSE;
	}

	gDerivedEditor.savedPath = targetPath;
	return TRUE;
}


/** Derived icon editor window procedure. */
static LRESULT CALLBACK DerivedIconEditorWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CREATE:
		{
			gRuntimeThemeUsers++;
			INITCOMMONCONTROLSEX icc = {};
			icc.dwSize = sizeof(icc);
			icc.dwICC = ICC_BAR_CLASSES;
			InitCommonControlsEx(&icc);

			CreateWindowW(L"STATIC", L"Composed Preview", WS_CHILD | WS_VISIBLE,
				12, 12, 220, 20, hWnd, NULL, NULL, NULL);
			HWND hPreview = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY | WS_BORDER,
				12, 36, 220, 220, hWnd, (HMENU) IDC_DERIVED_PREVIEW, NULL, NULL);
			if (hPreview)
				SetWindowSubclass(hPreview, DerivedPreviewSubclassProc, 1, 0);

			CreateWindowW(L"STATIC", L"Layers", WS_CHILD | WS_VISIBLE,
				248, 12, 220, 20, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
				248, 36, 440, 220, hWnd, (HMENU) IDC_DERIVED_LAYER_LIST, NULL, NULL);

			CreateWindowW(L"BUTTON", L"Add Layer", WS_CHILD | WS_VISIBLE,
				700, 36, 120, 28, hWnd, (HMENU) IDC_DERIVED_ADD_LAYER, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE,
				700, 70, 120, 28, hWnd, (HMENU) IDC_DERIVED_REMOVE_LAYER, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Move Up", WS_CHILD | WS_VISIBLE,
				700, 104, 120, 28, hWnd, (HMENU) IDC_DERIVED_MOVE_UP, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Move Down", WS_CHILD | WS_VISIBLE,
				700, 138, 120, 28, hWnd, (HMENU) IDC_DERIVED_MOVE_DOWN, NULL, NULL);

			CreateWindowW(L"STATIC", L"Hue: 0", WS_CHILD | WS_VISIBLE,
				248, 272, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_HUE, NULL, NULL);
			CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				500, 300, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_HUE, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				248, 292, 246, 34, hWnd, (HMENU) IDC_DERIVED_HUE, NULL, NULL);

			CreateWindowW(L"STATIC", L"Saturation: 100%", WS_CHILD | WS_VISIBLE,
				248, 328, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_SATURATION, NULL, NULL);
			CreateWindowW(L"EDIT", L"100", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				500, 356, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_SATURATION, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				248, 348, 246, 34, hWnd, (HMENU) IDC_DERIVED_SATURATION, NULL, NULL);

			CreateWindowW(L"STATIC", L"Opacity: 100%", WS_CHILD | WS_VISIBLE,
				248, 384, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_OPACITY, NULL, NULL);
			CreateWindowW(L"EDIT", L"100", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				500, 412, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_OPACITY, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				248, 404, 246, 34, hWnd, (HMENU) IDC_DERIVED_OPACITY, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Colorize grayscale", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
				248, 442, 180, 20, hWnd, (HMENU) IDC_DERIVED_COLORIZE, NULL, NULL);

			CreateWindowW(L"STATIC", L"Scale: 100%", WS_CHILD | WS_VISIBLE,
				560, 272, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_SCALE, NULL, NULL);
			CreateWindowW(L"EDIT", L"100", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				770, 300, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_SCALE, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				560, 292, 204, 34, hWnd, (HMENU) IDC_DERIVED_SCALE, NULL, NULL);

			CreateWindowW(L"STATIC", L"Position X: 0", WS_CHILD | WS_VISIBLE,
				560, 328, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_POSX, NULL, NULL);
			CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				770, 356, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_POSX, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				560, 348, 204, 34, hWnd, (HMENU) IDC_DERIVED_POSX, NULL, NULL);

			CreateWindowW(L"STATIC", L"Position Y: 0", WS_CHILD | WS_VISIBLE,
				560, 384, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_POSY, NULL, NULL);
			CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				770, 412, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_POSY, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				560, 404, 204, 34, hWnd, (HMENU) IDC_DERIVED_POSY, NULL, NULL);

			CreateWindowW(L"STATIC", L"Rotation: 0\xB0", WS_CHILD | WS_VISIBLE,
				560, 440, 220, 20, hWnd, (HMENU) IDC_DERIVED_LABEL_ROTATION, NULL, NULL);
			CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
				770, 468, 50, 20, hWnd, (HMENU) IDC_DERIVED_EDIT_ROTATION, NULL, NULL);
			CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
				560, 460, 204, 34, hWnd, (HMENU) IDC_DERIVED_ROTATION, NULL, NULL);

			CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				640, 504, 90, 30, hWnd, (HMENU) IDC_DERIVED_SAVE, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
				736, 504, 90, 30, hWnd, (HMENU) IDC_DERIVED_CANCEL, NULL, NULL);

			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_HUE), TBM_SETRANGE, TRUE, MAKELONG(-180, 180));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SATURATION), TBM_SETRANGE, TRUE, MAKELONG(0, 200));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_OPACITY), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SCALE), TBM_SETRANGE, TRUE, MAKELONG(10, 300));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSX), TBM_SETRANGE, TRUE, MAKELONG(-128, 128));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSY), TBM_SETRANGE, TRUE, MAKELONG(-128, 128));
			SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_ROTATION), TBM_SETRANGE, TRUE, MAKELONG(0, 360));

			RefreshDerivedLayerList(hWnd);
			SyncDerivedControlsFromSelection(hWnd);
			RebuildDerivedComposite();
			ApplyRuntimeTheme(hWnd);
		}
		return 0;

		case WM_THEMECHANGED:
		case WM_SYSCOLORCHANGE:
		case WM_SETTINGCHANGE:
			ApplyRuntimeTheme(hWnd);
			return 0;

		case WM_ERASEBKGND:
		{
			RECT rc = {};
			GetClientRect(hWnd, &rc);
			FillRect((HDC) wParam, &rc, gRuntimeBackgroundBrush ? gRuntimeBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			return 1;
		}

		case WM_CTLCOLORSTATIC:
		{
			HDC hdc = (HDC) wParam;
			SetTextColor(hdc, gRuntimeTextColor);
			SetBkColor(hdc, gRuntimeBackgroundColor);
			return (LRESULT) (gRuntimeBackgroundBrush ? gRuntimeBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORLISTBOX:
		{
			HDC hdc = (HDC) wParam;
			SetTextColor(hdc, gRuntimeTextColor);
			SetBkColor(hdc, gRuntimePanelColor);
			return (LRESULT) (gRuntimePanelBrush ? gRuntimePanelBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT) lParam;
			if (!di || (di->CtlID != IDC_DERIVED_PREVIEW))
				break;

			FillRect(di->hDC, &di->rcItem, gRuntimePanelBrush ? gRuntimePanelBrush : GetSysColorBrush(COLOR_WINDOW));
			if (gDerivedEditor.composedPixels.size() == ((size_t) 256 * 256 * 4))
			{
				BITMAPINFO bmi = {};
				bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bmi.bmiHeader.biWidth = 256;
				bmi.bmiHeader.biHeight = -256;
				bmi.bmiHeader.biPlanes = 1;
				bmi.bmiHeader.biBitCount = 32;
				bmi.bmiHeader.biCompression = BI_RGB;

				int boxW = di->rcItem.right - di->rcItem.left;
				int boxH = di->rcItem.bottom - di->rcItem.top;
				int draw = min(boxW, boxH) - 8;
				if (draw < 1) draw = 1;
				int dx = di->rcItem.left + (boxW - draw) / 2;
				int dy = di->rcItem.top + (boxH - draw) / 2;

				StretchDIBits(di->hDC,
					dx,
					dy,
					draw,
					draw,
					0,
					0,
					256,
					256,
					gDerivedEditor.composedPixels.data(),
					&bmi,
					DIB_RGB_COLORS,
					SRCCOPY);
			}
			return TRUE;
		}

		case WM_HSCROLL:
			ApplyDerivedControlsToSelection(hWnd);
			return 0;

		case WM_MOUSEWHEEL:
		{
			RECT imageRect = {};
			if (!GetDerivedPreviewImageRect(hWnd, &imageRect))
				break;
			POINT pt = { (short) LOWORD(lParam), (short) HIWORD(lParam) };
			ScreenToClient(hWnd, &pt);
			if (!PtInRect(&imageRect, pt))
				break;

			int idx = GetSelectedDerivedLayerIndex(hWnd);
			if (idx < 0)
				break;

			int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			int step = (delta > 0) ? 5 : -5;
			BOOL ctrlDown = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
			BOOL shiftDown = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);

			if (ctrlDown && !shiftDown)
			{
				if (!gDerivedEditor.layers[idx].isBase)
				{
					gDerivedEditor.layers[idx].opacity = min(100, max(0, gDerivedEditor.layers[idx].opacity + step));
					SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_OPACITY), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].opacity);
				}
			}
			else if (ctrlDown && shiftDown)
			{
				if (!gDerivedEditor.layers[idx].isBase)
				{
					int newHue = gDerivedEditor.layers[idx].hue + step;
					int newSat = gDerivedEditor.layers[idx].saturation + step;
					gDerivedEditor.layers[idx].hue = min(180, max(-180, newHue));
					gDerivedEditor.layers[idx].saturation = min(200, max(0, newSat));
					SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_HUE), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].hue);
					SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SATURATION), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].saturation);
				}
			}
			else if (!ctrlDown && shiftDown)
			{
				if (!gDerivedEditor.layers[idx].isBase)
				{
					int rotation = gDerivedEditor.layers[idx].rotation + step;
					while (rotation < 0)
						rotation += 360;
					while (rotation > 360)
						rotation -= 360;
					gDerivedEditor.layers[idx].rotation = rotation;
					SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_ROTATION), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].rotation);
				}
			}
			else
			{
				gDerivedEditor.layers[idx].scale = min(300, max(10, gDerivedEditor.layers[idx].scale + step));
				SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_SCALE), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].scale);
			}

			CommitDerivedLayerValues(hWnd, FALSE);
			return 0;
		}

		case WM_LBUTTONDOWN:
		{
			RECT imageRect = {};
			if (!GetDerivedPreviewImageRect(hWnd, &imageRect))
				break;
			POINT pt = { (short) LOWORD(lParam), (short) HIWORD(lParam) };
			if (!PtInRect(&imageRect, pt))
				break;
			int idx = GetSelectedDerivedLayerIndex(hWnd);
			if (idx < 0)
				break;

			gDerivedEditor.draggingLayer = TRUE;
			gDerivedEditor.dragStartPoint = pt;
			gDerivedEditor.dragStartPosX = gDerivedEditor.layers[idx].posX;
			gDerivedEditor.dragStartPosY = gDerivedEditor.layers[idx].posY;
			SetCapture(hWnd);
			return 0;
		}

		case WM_MOUSEMOVE:
			if (gDerivedEditor.draggingLayer)
			{
				int idx = GetSelectedDerivedLayerIndex(hWnd);
				if (idx >= 0)
				{
					POINT pt = { (short) LOWORD(lParam), (short) HIWORD(lParam) };
					int dx = pt.x - gDerivedEditor.dragStartPoint.x;
					int dy = pt.y - gDerivedEditor.dragStartPoint.y;
					gDerivedEditor.layers[idx].posX = min(128, max(-128, gDerivedEditor.dragStartPosX + dx));
					gDerivedEditor.layers[idx].posY = min(128, max(-128, gDerivedEditor.dragStartPosY + dy));
					SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSX), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].posX);
					SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_POSY), TBM_SETPOS, TRUE, gDerivedEditor.layers[idx].posY);
					CommitDerivedLayerValues(hWnd, FALSE);
				}
				return 0;
			}
			break;

		case WM_LBUTTONUP:
			if (gDerivedEditor.draggingLayer)
			{
				gDerivedEditor.draggingLayer = FALSE;
				ReleaseCapture();
				return 0;
			}
			break;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDC_DERIVED_LAYER_LIST:
				if (HIWORD(wParam) == LBN_SELCHANGE)
					SyncDerivedControlsFromSelection(hWnd);
				break;

				case IDC_DERIVED_EDIT_HUE:
				case IDC_DERIVED_EDIT_SATURATION:
				case IDC_DERIVED_EDIT_OPACITY:
				case IDC_DERIVED_EDIT_SCALE:
				case IDC_DERIVED_EDIT_POSX:
				case IDC_DERIVED_EDIT_POSY:
				case IDC_DERIVED_EDIT_ROTATION:
					if ((HIWORD(wParam) == EN_CHANGE) && !gDerivedEditor.syncingControls)
						CommitDerivedLayerValues(hWnd, TRUE);
					break;

				case IDC_DERIVED_COLORIZE:
					if (HIWORD(wParam) == BN_CLICKED)
						CommitDerivedLayerValues(hWnd, FALSE);
					break;

				case IDC_DERIVED_ADD_LAYER:
					AddDerivedLayerFromFile(hWnd);
					break;

				case IDC_DERIVED_REMOVE_LAYER:
				{
					int idx = GetSelectedDerivedLayerIndex(hWnd);
					if (idx > 0)
					{
						gDerivedEditor.layers.erase(gDerivedEditor.layers.begin() + idx);
						RefreshDerivedLayerList(hWnd);
						SyncDerivedControlsFromSelection(hWnd);
						RebuildDerivedComposite();
						InvalidateRect(GetDlgItem(hWnd, IDC_DERIVED_PREVIEW), NULL, TRUE);
					}
				}
				break;

				case IDC_DERIVED_MOVE_UP:
				{
					int idx = GetSelectedDerivedLayerIndex(hWnd);
					if (idx > 1)
					{
						std::swap(gDerivedEditor.layers[idx], gDerivedEditor.layers[idx - 1]);
						RefreshDerivedLayerList(hWnd);
						SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_LAYER_LIST), LB_SETCURSEL, idx - 1, 0);
						SyncDerivedControlsFromSelection(hWnd);
						RebuildDerivedComposite();
						InvalidateRect(GetDlgItem(hWnd, IDC_DERIVED_PREVIEW), NULL, TRUE);
					}
				}
				break;

				case IDC_DERIVED_MOVE_DOWN:
				{
					int idx = GetSelectedDerivedLayerIndex(hWnd);
					if ((idx > 0) && (idx < ((int) gDerivedEditor.layers.size() - 1)))
					{
						std::swap(gDerivedEditor.layers[idx], gDerivedEditor.layers[idx + 1]);
						RefreshDerivedLayerList(hWnd);
						SendMessageW(GetDlgItem(hWnd, IDC_DERIVED_LAYER_LIST), LB_SETCURSEL, idx + 1, 0);
						SyncDerivedControlsFromSelection(hWnd);
						RebuildDerivedComposite();
						InvalidateRect(GetDlgItem(hWnd, IDC_DERIVED_PREVIEW), NULL, TRUE);
					}
				}
				break;

				case IDC_DERIVED_SAVE:
					if (SaveDerivedIconFromEditor(hWnd))
						DestroyWindow(hWnd);
					break;

				case IDC_DERIVED_CANCEL:
					DestroyWindow(hWnd);
					break;
			}
			return 0;

		case WM_CLOSE:
			DestroyWindow(hWnd);
			return 0;

		case WM_DESTROY:
			gDerivedEditor.draggingLayer = FALSE;
			if (gRuntimeThemeUsers > 0)
				gRuntimeThemeUsers--;
			ReleaseRuntimeThemeResourcesIfUnused();
			return 0;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}


/** Open the derived icon editor as a modal child and return saved file path when successful. */
static BOOL ShowDerivedIconEditor(HWND hParent, const PickerItem& baseItem, std::wstring& savedPath)
{
	savedPath.clear();
	gDerivedEditor = {};
	gDerivedEditor.hParent = hParent;
	gDerivedEditor.baseItem = baseItem;

	if (!LoadPickerItemAsBaseBgra(baseItem, gDerivedEditor.basePixels))
	{
		MessageBoxA(hParent, "Unable to load the selected base icon.", "Error:", MB_OK | MB_ICONERROR);
		return FALSE;
	}

	DerivedLayer baseLayer = {};
	baseLayer.sourcePath = L"<base>";
	baseLayer.displayName = L"Base Icon";
	baseLayer.pixels = gDerivedEditor.basePixels;
	baseLayer.width = 256;
	baseLayer.height = 256;
	baseLayer.hue = 0;
	baseLayer.saturation = 100;
	baseLayer.opacity = 100;
	baseLayer.scale = 100;
	baseLayer.posX = 0;
	baseLayer.posY = 0;
	baseLayer.rotation = 0;
	baseLayer.colorize = FALSE;
	baseLayer.isBase = TRUE;
	gDerivedEditor.layers.push_back(baseLayer);

	RebuildDerivedComposite();

	WNDCLASSW wc = {};
	wc.lpfnWndProc = DerivedIconEditorWndProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"FoldrionDerivedIconEditor";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconA((HINSTANCE) GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	HWND hWnd = CreateWindowExW(
		WS_EX_DLGMODALFRAME,
		wc.lpszClassName,
		L"Create Derived Icon",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 850, 540,
		hParent, NULL, wc.hInstance, NULL);

	if (!hWnd)
		return FALSE;

	gDerivedEditor.hWnd = hWnd;
	EnableWindow(hParent, FALSE);
	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	MSG msg = {};
	while (IsWindow(hWnd) && (GetMessage(&msg, NULL, 0, 0) > 0))
	{
		if (!IsDialogMessage(hWnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	EnableWindow(hParent, TRUE);
	SetActiveWindow(hParent);
	savedPath = gDerivedEditor.savedPath;

	gDerivedEditor.layers.clear();
	gDerivedEditor.basePixels.clear();
	gDerivedEditor.composedPixels.clear();
	return !savedPath.empty();
}


/** Locate one picker item by resource path and select it when available. */
static void SelectPickerItemByResourcePath(HWND hWnd, const std::wstring& path)
{
	if (path.empty())
		return;

	for (size_t c = 0; c < gPickerCategories.size(); c++)
	{
		for (size_t i = 0; i < gPickerCategories[c].items.size(); i++)
		{
			const PickerItem& item = gPickerCategories[c].items[i];
			if (!item.isBuiltIn && (_wcsicmp(item.resourcePath.c_str(), path.c_str()) == 0))
			{
				HWND hCat = GetDlgItem(hWnd, IDC_PICKER_CATEGORY);
				SendMessageW(hCat, LB_SETCURSEL, (WPARAM) c, 0);
				PopulatePickerItems(hWnd, (int) c);

				HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
				for (int row = 0; row < (int) gPickerVisibleItems.size(); row++)
				{
					if ((gPickerVisibleItems[row].categoryIndex == (int) c) && (gPickerVisibleItems[row].itemIndex == (int) i))
					{
						SendMessageW(hItem, LB_SETCURSEL, row, 0);
						LoadPickerPreviewIcon(gPickerCategories[c].items[i]);
						InvalidateRect(GetDlgItem(hWnd, IDC_PICKER_PREVIEW), NULL, TRUE);
						UpdatePickerPreviewLabel(hWnd);
						return;
					}
				}
			}
		}
	}
}


/**
 * Return TRUE when one path lives under another directory path.
 */
static BOOL IsPathUnderFolder(const std::wstring& filePath, const std::wstring& folderPath)
{
	if (filePath.empty() || folderPath.empty())
		return FALSE;

	WCHAR fullFile[MAX_PATH] = {};
	WCHAR fullFolder[MAX_PATH] = {};
	DWORD fileLen = GetFullPathNameW(filePath.c_str(), _countof(fullFile), fullFile, NULL);
	DWORD folderLen = GetFullPathNameW(folderPath.c_str(), _countof(fullFolder), fullFolder, NULL);
	if ((fileLen == 0) || (fileLen >= _countof(fullFile)) || (folderLen == 0) || (folderLen >= _countof(fullFolder)))
		return FALSE;

	std::wstring normalizedFile = ToLowerCopy(fullFile);
	std::wstring normalizedFolder = ToLowerCopy(fullFolder);
	if (normalizedFolder.back() != L'\\')
		normalizedFolder.push_back(L'\\');

	if (normalizedFile.size() <= normalizedFolder.size())
		return FALSE;

	return (normalizedFile.compare(0, normalizedFolder.size(), normalizedFolder) == 0);
}


/**
 * Return TRUE when a picker item is a removable custom icon file.
 */
static BOOL IsRemovableCustomPickerItem(const PickerItem& item)
{
	if (item.isBuiltIn || item.resourcePath.empty())
		return FALSE;

	if (_wcsicmp(PathFindExtensionW(item.resourcePath.c_str()), L".ico") != 0)
		return FALSE;

	std::wstring iconsRoot = std::wstring(myPathGlobal) + L"icons";
	return IsPathUnderFolder(item.resourcePath, iconsRoot);
}


/**
 * Enable or disable picker delete button from current selection.
 */
static void UpdatePickerDeleteButtonState(HWND hWnd)
{
	HWND hDeleteButton = GetDlgItem(hWnd, IDC_PICKER_DELETE);
	HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
	if (!hDeleteButton || !hItem)
		return;

	BOOL canDelete = FALSE;
	int itemSel = (int) SendMessageW(hItem, LB_GETCURSEL, 0, 0);
	if (itemSel != LB_ERR)
	{
		int visIdx = (int) SendMessageW(hItem, LB_GETITEMDATA, (WPARAM) itemSel, 0);
		if ((visIdx >= 0) && (visIdx < (int) gPickerVisibleItems.size()))
		{
			const PickerVisibleItem& vi = gPickerVisibleItems[visIdx];
			if ((vi.categoryIndex >= 0) && (vi.categoryIndex < (int) gPickerCategories.size()) &&
				(vi.itemIndex >= 0) && (vi.itemIndex < (int) gPickerCategories[vi.categoryIndex].items.size()))
			{
				canDelete = IsRemovableCustomPickerItem(gPickerCategories[vi.categoryIndex].items[vi.itemIndex]);
			}
		}
	}

	EnableWindow(hDeleteButton, canDelete);
}


/**
 * Delete selected custom icon file from disk with user confirmation.
 */
static void DeleteSelectedCustomIcon(HWND hWnd)
{
	HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
	if (!hItem)
		return;

	int itemSel = (int) SendMessageW(hItem, LB_GETCURSEL, 0, 0);
	if (itemSel == LB_ERR)
	{
		MessageBoxA(hWnd, "Select an icon.", "Info:", MB_OK | MB_ICONINFORMATION);
		UpdatePickerDeleteButtonState(hWnd);
		return;
	}

	int visIdx = (int) SendMessageW(hItem, LB_GETITEMDATA, (WPARAM) itemSel, 0);
	if ((visIdx < 0) || (visIdx >= (int) gPickerVisibleItems.size()))
	{
		UpdatePickerDeleteButtonState(hWnd);
		return;
	}

	const PickerVisibleItem& vi = gPickerVisibleItems[visIdx];
	if ((vi.categoryIndex < 0) || (vi.categoryIndex >= (int) gPickerCategories.size()) ||
		(vi.itemIndex < 0) || (vi.itemIndex >= (int) gPickerCategories[vi.categoryIndex].items.size()))
	{
		UpdatePickerDeleteButtonState(hWnd);
		return;
	}

	const PickerItem& item = gPickerCategories[vi.categoryIndex].items[vi.itemIndex];
	if (!IsRemovableCustomPickerItem(item))
	{
		MessageBoxA(hWnd, "Only custom ICO icons can be deleted.", "Info:", MB_OK | MB_ICONINFORMATION);
		UpdatePickerDeleteButtonState(hWnd);
		return;
	}

	std::wstring fileName = PathFindFileNameW(item.resourcePath.c_str());
	WCHAR question[512] = {};
	_snwprintf_s(question, _countof(question), (_countof(question) - 1),
		L"Delete this custom icon file?\n\n%s",
		fileName.c_str());

	if (MessageBoxW(hWnd, question, L"Confirm Delete", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
	{
		UpdatePickerDeleteButtonState(hWnd);
		return;
	}

	if (!DeleteFileW(item.resourcePath.c_str()))
	{
		MessageBoxA(hWnd, "Unable to delete icon file.", "Error:", MB_OK | MB_ICONERROR);
		UpdatePickerDeleteButtonState(hWnd);
		return;
	}

	if (isInstalled)
		RefreshInstalledShellMenu();

	ReleasePickerPreviewIcon();
	ReleasePickerIcons();
	BuildPickerCategories(gPickerCategories);
	PopulatePickerCategories(hWnd);

	HWND hPreview = GetDlgItem(hWnd, IDC_PICKER_PREVIEW);
	if (hPreview)
		InvalidateRect(hPreview, NULL, TRUE);
	UpdatePickerPreviewLabel(hWnd);
	UpdatePickerDeleteButtonState(hWnd);
}


/**
 * Subclass to support Delete key and middle-click deletion in picker list.
 */
static LRESULT CALLBACK PickerItemListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);
	UNREFERENCED_PARAMETER(dwRefData);

	HWND hParent = GetParent(hWnd);
	if (!hParent)
		return DefSubclassProc(hWnd, uMsg, wParam, lParam);

	switch (uMsg)
	{
		case WM_KEYDOWN:
			if (wParam == VK_DELETE)
			{
				SendMessageW(hParent, WM_PICKER_DELETE_REQUEST, 0, 0);
				return 0;
			}
			break;

		case WM_MBUTTONUP:
		{
			POINT pt = { (short) LOWORD(lParam), (short) HIWORD(lParam) };
			DWORD rowResult = (DWORD) SendMessageW(hWnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
			int clickedRow = LOWORD(rowResult);
			BOOL outsideListItem = HIWORD(rowResult);
			if (!outsideListItem && (clickedRow != LB_ERR))
			{
				SendMessageW(hWnd, LB_SETCURSEL, (WPARAM) clickedRow, 0);
				SendMessageW(hParent, WM_PICKER_DELETE_REQUEST, (WPARAM) clickedRow, 0);
				return 0;
			}
		}
		break;

		case WM_NCDESTROY:
			RemoveWindowSubclass(hWnd, PickerItemListSubclassProc, 1);
			break;
	}

	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}


/**
 * Picker window procedure.
 */
static LRESULT CALLBACK PickerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CREATE:
		{
			gRuntimeThemeUsers++;
			CreateWindowW(L"STATIC", L"Category", WS_CHILD | WS_VISIBLE,
				12, 12, 500, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_BORDER,
				12, 32, 500, 360, hWnd, (HMENU) IDC_PICKER_CATEGORY, NULL, NULL);

			CreateWindowW(L"STATIC", L"Icons", WS_CHILD | WS_VISIBLE,
				524, 12, 80, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"STATIC", L"Search", WS_CHILD | WS_VISIBLE,
				606, 12, 48, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
				658, 10, 386, 22, hWnd, (HMENU) IDC_PICKER_SEARCH, NULL, NULL);
			CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_BORDER | LBS_OWNERDRAWFIXED,
				524, 32, 520, 360, hWnd, (HMENU) IDC_PICKER_ITEM, NULL, NULL);

			// --- Preview panel (right of icon list) ---
			CreateWindowW(L"STATIC", L"Preview", WS_CHILD | WS_VISIBLE,
				1056, 12, 150, 18, hWnd, (HMENU) IDC_PICKER_PREVIEW_LABEL, NULL, NULL);
			CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY | WS_BORDER,
				1056, 32, 150, 150, hWnd, (HMENU) IDC_PICKER_PREVIEW, NULL, NULL);

			CreateWindowW(L"BUTTON", L"Apply Icon", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				433, 404, 95, 28, hWnd, (HMENU) IDC_PICKER_APPLY, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Import Icons", WS_CHILD | WS_VISIBLE,
				148, 404, 95, 28, hWnd, (HMENU) IDC_PICKER_IMPORT, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Delete Icon", WS_CHILD | WS_VISIBLE,
				687, 404, 95, 28, hWnd, (HMENU) IDC_PICKER_DELETE, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Create Derived Icon", WS_CHILD | WS_VISIBLE,
				538, 404, 139, 28, hWnd, (HMENU) IDC_PICKER_CREATE_DERIVED, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Open Icons Folder", WS_CHILD | WS_VISIBLE,
				12, 404, 130, 28, hWnd, (HMENU) IDC_PICKER_OPEN_ICONS, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Restore Default", WS_CHILD | WS_VISIBLE,
				1002, 404, 120, 28, hWnd, (HMENU) IDC_PICKER_RESTORE, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
				1132, 404, 74, 28, hWnd, (HMENU) IDC_PICKER_CANCEL, NULL, NULL);

			PopulatePickerCategories(hWnd);

			// Show the folder's existing icon in the preview until user picks something.
			LoadFolderCurrentPreviewIcon();
			UpdatePickerPreviewLabel(hWnd);
			SetWindowSubclass(GetDlgItem(hWnd, IDC_PICKER_ITEM), PickerItemListSubclassProc, 1, 0);
			UpdatePickerDeleteButtonState(hWnd);
			ApplyRuntimeTheme(hWnd);
		}
		return 0;

		case WM_THEMECHANGED:
		case WM_SYSCOLORCHANGE:
		case WM_SETTINGCHANGE:
			ApplyRuntimeTheme(hWnd);
			return 0;

		case WM_ERASEBKGND:
		{
			RECT rc = {};
			GetClientRect(hWnd, &rc);
			FillRect((HDC) wParam, &rc, gRuntimeBackgroundBrush ? gRuntimeBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			return 1;
		}

		case WM_CTLCOLORSTATIC:
		{
			HDC hdc = (HDC) wParam;
			SetTextColor(hdc, gRuntimeTextColor);
			SetBkColor(hdc, gRuntimeBackgroundColor);
			return (LRESULT) (gRuntimeBackgroundBrush ? gRuntimeBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORLISTBOX:
		{
			HDC hdc = (HDC) wParam;
			SetTextColor(hdc, gRuntimeTextColor);
			SetBkColor(hdc, gRuntimePanelColor);
			return (LRESULT) (gRuntimePanelBrush ? gRuntimePanelBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		case WM_MEASUREITEM:
		{
			LPMEASUREITEMSTRUCT mi = (LPMEASUREITEMSTRUCT) lParam;
			if (mi && (mi->CtlID == IDC_PICKER_ITEM))
			{
				mi->itemHeight = 22;
				return TRUE;
			}
		}
		break;

		case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT) lParam;
			if (!di) break;

			// --- Preview box ---
			if (di->CtlID == IDC_PICKER_PREVIEW)
			{
				FillRect(di->hDC, &di->rcItem, gRuntimePanelBrush ? gRuntimePanelBrush : GetSysColorBrush(COLOR_WINDOW));
				if (gPickerPreviewIcon)
				{
					int w = di->rcItem.right  - di->rcItem.left;
					int h = di->rcItem.bottom - di->rcItem.top;
					int sz = min(w, h) - 8;
					if (sz < 1) sz = 1;
					int ox = di->rcItem.left + (w - sz) / 2;
					int oy = di->rcItem.top  + (h - sz) / 2;
					DrawIconEx(di->hDC, ox, oy, gPickerPreviewIcon, sz, sz, 0, NULL, DI_NORMAL);
				}
				return TRUE;
			}

			if ((di->CtlID != IDC_PICKER_ITEM) || (di->itemID == UINT_MAX))
				break;

			HBRUSH bgBrush = NULL;
			COLORREF textColor = gRuntimeTextColor;
			if (di->itemState & ODS_SELECTED)
			{
				bgBrush = gRuntimeSelectionBrush ? gRuntimeSelectionBrush : GetSysColorBrush(COLOR_HIGHLIGHT);
				textColor = gRuntimeSelectionTextColor;
			}
			else
				bgBrush = gRuntimePanelBrush ? gRuntimePanelBrush : GetSysColorBrush(COLOR_WINDOW);

			FillRect(di->hDC, &di->rcItem, bgBrush);

			int visibleIndex = (int) SendMessageW(di->hwndItem, LB_GETITEMDATA, di->itemID, 0);
			if ((visibleIndex < 0) || (visibleIndex >= (int) gPickerVisibleItems.size()))
				return TRUE;

			const PickerVisibleItem& visibleItem = gPickerVisibleItems[visibleIndex];
			if ((visibleItem.categoryIndex < 0) || (visibleItem.categoryIndex >= (int) gPickerCategories.size()))
				return TRUE;

			if ((visibleItem.itemIndex < 0) || (visibleItem.itemIndex >= (int) gPickerCategories[visibleItem.categoryIndex].items.size()))
				return TRUE;

			PickerItem& item = gPickerCategories[visibleItem.categoryIndex].items[visibleItem.itemIndex];
			HICON hIcon = ResolvePickerItemIcon(item);

			int iconX = di->rcItem.left + 4;
			int iconY = di->rcItem.top + ((di->rcItem.bottom - di->rcItem.top - 16) / 2);
			if (hIcon)
				DrawIconEx(di->hDC, iconX, iconY, hIcon, 16, 16, 0, NULL, DI_NORMAL);

			RECT textRc = di->rcItem;
			textRc.left += 26;
			SetBkMode(di->hDC, TRANSPARENT);
			SetTextColor(di->hDC, textColor);
			DrawTextW(di->hDC, gPickerVisibleItems[visibleIndex].displayLabel.c_str(), -1, &textRc, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			if (di->itemState & ODS_FOCUS)
				DrawFocusRect(di->hDC, &di->rcItem);

			return TRUE;
		}
		break;

		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
				case IDC_PICKER_DELETE:
				DeleteSelectedCustomIcon(hWnd);
				break;

				case IDC_PICKER_PREVIEW:
				if (HIWORD(wParam) == STN_CLICKED)
				{
					// Reload and display the folder's current (existing) icon.
					LoadFolderCurrentPreviewIcon();
					HWND hPreview = GetDlgItem(hWnd, IDC_PICKER_PREVIEW);
					if (hPreview) InvalidateRect(hPreview, NULL, TRUE);
					UpdatePickerPreviewLabel(hWnd);
				}
				break;

				case IDC_PICKER_SEARCH:
				if (HIWORD(wParam) == EN_CHANGE)
				{
					// Debounce heavy filtering work while user is typing.
					SetTimer(hWnd, PICKER_SEARCH_DEBOUNCE_TIMER_ID, PICKER_SEARCH_DEBOUNCE_MS, NULL);
				}
				break;

				case IDC_PICKER_CATEGORY:
				if (HIWORD(wParam) == LBN_SELCHANGE)
				{
					// Clear search and preview when switching category.
					HWND hSearch = GetDlgItem(hWnd, IDC_PICKER_SEARCH);
					if (hSearch)
					{
						KillTimer(hWnd, PICKER_SEARCH_DEBOUNCE_TIMER_ID);
						SetWindowTextW(hSearch, L"");
						gPickerSearchQuery.clear();
					}
					ReleasePickerPreviewIcon();
					HWND hPreview = GetDlgItem(hWnd, IDC_PICKER_PREVIEW);
					if (hPreview) InvalidateRect(hPreview, NULL, TRUE);
					UpdatePickerPreviewLabel(hWnd);
					ApplyPickerSearchNow(hWnd);
					UpdatePickerDeleteButtonState(hWnd);
				}
				break;

				case IDC_PICKER_ITEM:
				if (HIWORD(wParam) == LBN_SELCHANGE)
				{
					HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
					int itemSel = (int) SendMessageW(hItem, LB_GETCURSEL, 0, 0);
					if (itemSel != LB_ERR)
					{
						int visIdx = (int) SendMessageW(hItem, LB_GETITEMDATA, (WPARAM) itemSel, 0);
						if ((visIdx >= 0) && (visIdx < (int) gPickerVisibleItems.size()))
						{
							const PickerVisibleItem& vi = gPickerVisibleItems[visIdx];
							if ((vi.categoryIndex >= 0) && (vi.categoryIndex < (int) gPickerCategories.size()) &&
								(vi.itemIndex >= 0) && (vi.itemIndex < (int) gPickerCategories[vi.categoryIndex].items.size()))
							{
								LoadPickerPreviewIcon(gPickerCategories[vi.categoryIndex].items[vi.itemIndex]);
							}
						}
					}
					HWND hPreview = GetDlgItem(hWnd, IDC_PICKER_PREVIEW);
					if (hPreview) InvalidateRect(hPreview, NULL, TRUE);
					UpdatePickerPreviewLabel(hWnd);
					UpdatePickerDeleteButtonState(hWnd);
				}
				break;

				case IDC_PICKER_APPLY:
				{
					HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
					int itemSel = (int) SendMessageW(hItem, LB_GETCURSEL, 0, 0);
					if (itemSel == LB_ERR)
					{
						MessageBoxA(hWnd, "Select an icon.", "Info:", MB_OK | MB_ICONINFORMATION);
						break;
					}

					int visibleIndex = (int) SendMessageW(hItem, LB_GETITEMDATA, (WPARAM) itemSel, 0);
					if ((visibleIndex < 0) || (visibleIndex >= (int) gPickerVisibleItems.size()))
						break;

					const PickerVisibleItem& visibleItem = gPickerVisibleItems[visibleIndex];
					if ((visibleItem.categoryIndex < 0) || (visibleItem.categoryIndex >= (int) gPickerCategories.size()))
						break;

					if ((visibleItem.itemIndex < 0) || (visibleItem.itemIndex >= (int) gPickerCategories[visibleItem.categoryIndex].items.size()))
						break;

					const PickerItem& item = gPickerCategories[visibleItem.categoryIndex].items[visibleItem.itemIndex];
					if (item.isBuiltIn)
					{
						// Apply built-in colors using the currently running EXE resources.
						std::wstring exePath = GetCurrentExePath();
						if (!exePath.empty())
							SetFolderIconResource(exePath.c_str(), item.builtInOffset + item.builtInIndex, (LPWSTR) gPickerTargetFolder.c_str());
						else
							SetFolderColor(item.builtInIndex, (LPWSTR) gPickerTargetFolder.c_str());
					}
					else
						SetFolderIconResource(item.resourcePath.c_str(), item.resourceIndex, (LPWSTR) gPickerTargetFolder.c_str());

					ResetWindowsIconCache();
					DestroyWindow(hWnd);
				}
				break;

				case IDC_PICKER_IMPORT:
				{
					UINT copiedCount = 0;
					UINT convertedCount = 0;
					UINT failedCount = 0;
					std::vector<std::wstring> failedFiles;

					if (!ImportCustomIconFiles(hWnd, &copiedCount, &convertedCount, &failedCount, &failedFiles))
						break;

					if (isInstalled)
						RefreshInstalledShellMenu();

					ReleasePickerIcons();
					BuildPickerCategories(gPickerCategories);
					PopulatePickerCategories(hWnd);
					UpdatePickerDeleteButtonState(hWnd);

					char msg[1024];
					sprintf_s(msg, sizeof(msg),
						"Imported %u file(s), converted %u image(s), failed %u file(s).",
						copiedCount, convertedCount, failedCount);
					AppendFailedImportList(msg, sizeof(msg), failedFiles);
					MessageBoxA(hWnd, msg, "Completion:", (MB_OK | (failedCount ? MB_ICONWARNING : MB_ICONASTERISK)));
				}
				break;

				case IDC_PICKER_CREATE_DERIVED:
				{
					HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
					int itemSel = (int) SendMessageW(hItem, LB_GETCURSEL, 0, 0);
					if (itemSel == LB_ERR)
					{
						MessageBoxA(hWnd, "Select an icon first.", "Info:", MB_OK | MB_ICONINFORMATION);
						break;
					}

					int visibleIndex = (int) SendMessageW(hItem, LB_GETITEMDATA, (WPARAM) itemSel, 0);
					if ((visibleIndex < 0) || (visibleIndex >= (int) gPickerVisibleItems.size()))
						break;

					const PickerVisibleItem& visibleItem = gPickerVisibleItems[visibleIndex];
					if ((visibleItem.categoryIndex < 0) || (visibleItem.categoryIndex >= (int) gPickerCategories.size()))
						break;

					if ((visibleItem.itemIndex < 0) || (visibleItem.itemIndex >= (int) gPickerCategories[visibleItem.categoryIndex].items.size()))
						break;

					std::wstring savedPath;
					const PickerItem& selectedItem = gPickerCategories[visibleItem.categoryIndex].items[visibleItem.itemIndex];
					if (ShowDerivedIconEditor(hWnd, selectedItem, savedPath))
					{
						if (isInstalled)
							RefreshInstalledShellMenu();

						ReleasePickerIcons();
						BuildPickerCategories(gPickerCategories);
						PopulatePickerCategories(hWnd);
						SelectPickerItemByResourcePath(hWnd, savedPath);
						UpdatePickerDeleteButtonState(hWnd);
					}
				}
				break;

				case IDC_PICKER_OPEN_ICONS:
				{
					WCHAR iconsPath[MAX_PATH];
					if (_snwprintf_s(iconsPath, _countof(iconsPath), (_countof(iconsPath) - 1), L"%sicons", myPathGlobal) < 1)
					{
						MessageBoxA(hWnd, "Unable to build icons folder path.", "Error:", (MB_OK | MB_ICONERROR));
						break;
					}

					if (!EnsureDirectoryExists(iconsPath))
					{
						MessageBoxA(hWnd, "Unable to create icons folder.", "Error:", (MB_OK | MB_ICONERROR));
						break;
					}

					if (!OpenDirectoryInExplorer(iconsPath))
						MessageBoxA(hWnd, "Unable to open icons folder.", "Error:", (MB_OK | MB_ICONERROR));
				}
				break;

				case IDC_PICKER_RESTORE:
				SetFolderColor(COLOR_ICON_COUNT, (LPWSTR) gPickerTargetFolder.c_str());
				ResetWindowsIconCache();
				DestroyWindow(hWnd);
				break;

				case IDC_PICKER_CANCEL:
				DestroyWindow(hWnd);
				break;
			}
		}
		return 0;

		case WM_PICKER_DELETE_REQUEST:
			DeleteSelectedCustomIcon(hWnd);
			return 0;

		case WM_TIMER:
		if (wParam == PICKER_SEARCH_DEBOUNCE_TIMER_ID)
		{
			KillTimer(hWnd, PICKER_SEARCH_DEBOUNCE_TIMER_ID);
			ApplyPickerSearchNow(hWnd);
			return 0;
		}
		break;

		case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;

		case WM_DESTROY:
		KillTimer(hWnd, PICKER_SEARCH_DEBOUNCE_TIMER_ID);
		ReleasePickerIcons();
		ReleasePickerPreviewIcon();
		gPickerVisibleItems.clear();
		gPickerSearchQuery.clear();
		if (gRuntimeThemeUsers > 0)
			gRuntimeThemeUsers--;
		ReleaseRuntimeThemeResourcesIfUnused();
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}


/**
 * Show the runtime icon picker window and block until closed.
 */
static int ShowFolderIconPicker(LPCWSTR folderPath)
{
	if (!folderPath || !folderPath[0])
		return EXIT_FAILURE;

	gPickerTargetFolder = folderPath;
	gPickerSearchQuery.clear();
	gPickerVisibleItems.clear();
	BuildPickerCategories(gPickerCategories);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = PickerWndProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"FoldrionRuntimePicker";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconA((HINSTANCE) GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	HWND hWnd = CreateWindowExW(
		WS_EX_APPWINDOW,
		wc.lpszClassName,
		L"Customize Folder",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 1260, 480,
		NULL, NULL, wc.hInstance, NULL);

	if (!hWnd)
		return EXIT_FAILURE;

	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	MSG msg = {};
	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
		// Handle search shortcuts globally before dispatching to child controls.
		if (msg.message == WM_KEYDOWN)
		{
			BOOL handled = FALSE;
			HWND hSearch = GetDlgItem(hWnd, IDC_PICKER_SEARCH);

			if ((msg.wParam == VK_F2) ||
				(msg.wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)))
			{
				// Focus the search bar and select all existing text.
				if (hSearch)
				{
					SetFocus(hSearch);
					SendMessageW(hSearch, EM_SETSEL, 0, -1);
				}
				handled = TRUE;
			}
			else if (msg.wParam == VK_ESCAPE)
			{
				if (hSearch && (GetFocus() == hSearch))
				{
					// Clear search and apply immediately.
					SetWindowTextW(hSearch, L"");
					KillTimer(hWnd, PICKER_SEARCH_DEBOUNCE_TIMER_ID);
					ApplyPickerSearchNow(hWnd);
				}
				else
				{
					DestroyWindow(hWnd);
				}
				handled = TRUE;
			}

			if (handled)
				continue;
		}

		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return EXIT_SUCCESS;
}

static void OpenLinkUrl()
{
	SHELLEXECUTEINFOA nfo;
	ZeroMemory(&nfo, sizeof(SHELLEXECUTEINFOA));
	nfo.cbSize = sizeof(SHELLEXECUTEINFOA);
	nfo.lpVerb = "open";
	nfo.lpFile = APP_URL;
	nfo.fMask  = SEE_MASK_ASYNCOK;
	nfo.nShow  = SW_SHOWNORMAL;
	ShellExecuteExA(&nfo);
}

static LRESULT CALLBACK HypLinkSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	switch(uMsg)
	{
		case WM_NCDESTROY:
		RemoveWindowSubclass(hWnd, HypLinkSubclass, uIdSubclass);
		break;

		case WM_NCHITTEST:
		return HTCLIENT;
		break;

		// Switch font when use hovers over us
		case WM_MOUSEMOVE:
		{
			if(GetCapture() != hWnd)
			{
				SendMessage(hWnd, WM_SETFONT, (WPARAM) undlineFont, FALSE);
				//UpdateWinDatForegroundColor(hWnd, pcHyperlink->m_HighlightColor);
				InvalidateRect(hWnd, NULL, FALSE);
				SetCapture(hWnd);
			}
			else
			{
				RECT rect;
				GetWindowRect(hWnd, &rect);
				POINT pt = { LOWORD(lParam), HIWORD(lParam)};
				ClientToScreen(hWnd, &pt);

				if(!PtInRect(&rect, pt))
				{
					SendMessage(hWnd, WM_SETFONT, (WPARAM) tweakedFont, FALSE);
					//UpdateWinDatForegroundColor(hWnd, pcHyperlink->m_CurrentColor);
					InvalidateRect(hWnd, NULL, FALSE);
					ReleaseCapture();
				}
			}
		}
		break;

		// Finger point cursor when over the control
		case WM_SETCURSOR:
		if(mouseOverCursor)
		{
			SetCursor(mouseOverCursor);
			return 1;
		}
		break;

		case WM_LBUTTONDOWN:
		{
			SetFocus(hWnd);
			SetCapture(hWnd);
			isClicking = TRUE;
		}
		break;

		case WM_LBUTTONUP:
		{
			ReleaseCapture();

			if(isClicking)
			{
				isClicking = FALSE;
				POINT pt;
				pt.x = (short) LOWORD(lParam);
				pt.y = (short) HIWORD(lParam);
				ClientToScreen(hWnd, &pt);
				RECT rc;
				GetWindowRect(hWnd, &rc);

				if(PtInRect(&rc,pt))
				{
					if(!isVisited)
					{
						isVisited = TRUE;
						//UpdateWinDatForegroundColor(hWnd, pcHyperlink->m_CurrentColor = pcHyperlink->m_VisitedColor);
						urlColor = VISITED_COLOR;
						InvalidateRect(hWnd, NULL, TRUE);
					}

					OpenLinkUrl();
				}
			}
		}
		break;

		case WM_SETFOCUS:
		{
			InvalidateRect(hWnd, NULL, TRUE);
			UpdateWindow(hWnd);
		}
		break;

		case WM_KILLFOCUS:
		{
			SendMessage(hWnd, WM_SETFONT, (WPARAM) tweakedFont, FALSE);
			//UpdateWinDatForegroundColor(hWnd, pcHyperlink->m_CurrentColor);
			InvalidateRect(hWnd, NULL, TRUE);
			UpdateWindow(hWnd);
		}
		break;

		case WM_GETDLGCODE:
		return DLGC_WANTCHARS;
		break;

		// Space key activate?
		case WM_KEYDOWN:
		if(wParam == VK_SPACE)
		{
			if(!isVisited)
			{
				isVisited = TRUE;
				//UpdateWinDatForegroundColor(hWnd, pcHyperlink->m_CurrentColor = pcHyperlink->m_VisitedColor);
				urlColor = VISITED_COLOR;
				InvalidateRect(hWnd, NULL, TRUE);
			}

			OpenLinkUrl();
			return 0;
		}
		break;

		case WM_KEYUP:
		if(wParam == VK_SPACE)
			return 0;
		break;
	};

	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ----------------------------------------------------------------------------


// Our dialog Window message handler
static INT_PTR CALLBACK DlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			// Add info to caption
			SetWindowTextA(hWnd, PROJECT_NAME " " APP_VERSION " Built: " __DATE__);

			// Set dialog icon
			HICON hIcon = LoadIconA((HINSTANCE) GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
			SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM) hIcon);
			SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM) hIcon);

			// Setup customized hyperlink control
			HWND hWndCtrl = GetDlgItem(hWnd, IDC_HYPERLINK);
			if (hWndCtrl)
			{
				// Font options
				HFONT fontHandle = (HFONT) SendMessage(hWndCtrl, WM_GETFONT, 0, 0);
				if (!fontHandle)
					fontHandle = (HFONT) GetStockObject(DEFAULT_GUI_FONT);

				if (fontHandle)
				{
					LOGFONT lf;
					if (GetObject(fontHandle, sizeof(LOGFONT), &lf) != 0)
					{
						// Emphasized font
						lf.lfHeight = URL_FONT_HEIGHT;
						lf.lfWeight = URL_FONT_WIDTH;
						tweakedFont = CreateFontIndirect(&lf);
						if (tweakedFont)
							SendMessageA(hWndCtrl, WM_SETFONT, WPARAM(tweakedFont), TRUE);

						// Underlined version
						lf.lfUnderline = TRUE;
						undlineFont = CreateFontIndirect(&lf);
					}
				}

				// Hyperlink finger cursor
				static const BYTE curAND[128] =
				{
					0xF9,0xFF,0xFF,0xFF, 0xF0,0xFF,0xFF,0xFF, 0xF0,0xFF,0xFF,0xFF, 0xF0,0xFF,0xFF,0xFF,
					0xF0,0xFF,0xFF,0xFF, 0xF0,0x3F,0xFF,0xFF, 0xF0,0x07,0xFF,0xFF, 0xF0,0x01,0xFF,0xFF,
					0xF0,0x00,0xFF,0xFF, 0x10,0x00,0x7F,0xFF, 0x00,0x00,0x7F,0xFF, 0x00,0x00,0x7F,0xFF,
					0x80,0x00,0x7F,0xFF, 0xC0,0x00,0x7F,0xFF, 0xC0,0x00,0x7F,0xFF, 0xE0,0x00,0x7F,0xFF,
					0xE0,0x00,0xFF,0xFF, 0xF0,0x00,0xFF,0xFF, 0xF0,0x00,0xFF,0xFF, 0xF8,0x01,0xFF,0xFF,
					0xF8,0x01,0xFF,0xFF, 0xF8,0x01,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,
					0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,
					0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF
				};
				static const BYTE curXOR[128] =
				{
					0x00,0x00,0x00,0x00, 0x06,0x00,0x00,0x00, 0x06,0x00,0x00,0x00, 0x06,0x00,0x00,0x00,
					0x06,0x00,0x00,0x00, 0x06,0x00,0x00,0x00, 0x06,0xC0,0x00,0x00, 0x06,0xD8,0x00,0x00,
					0x06,0xDA,0x00,0x00, 0x06,0xDB,0x00,0x00, 0x67,0xFB,0x00,0x00, 0x77,0xFF,0x00,0x00,
					0x37,0xFF,0x00,0x00, 0x17,0xFF,0x00,0x00, 0x1F,0xFF,0x00,0x00, 0x0F,0xFF,0x00,0x00,
					0x0F,0xFE,0x00,0x00, 0x07,0xFE,0x00,0x00, 0x07,0xFE,0x00,0x00, 0x03,0xFC,0x00,0x00,
					0x03,0xFC,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
					0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
					0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00
				};
				mouseOverCursor = CreateCursor(GetModuleHandle(NULL), 5, 0, 32, 32, curAND, curXOR);

				// Subclasses the control
				SetWindowSubclass(hWndCtrl, HypLinkSubclass, 1138, 0);
			}

			// Tweak the button font too
			HFONT fontHandle = (HFONT) SendMessage(GetDlgItem(hWnd, IDC_INSTALL_UNINSTALL), WM_GETFONT, 0, 0);
			if (!fontHandle)
				fontHandle = (HFONT) GetStockObject(DEFAULT_GUI_FONT);
			if (fontHandle)
			{
				LOGFONT lf;
				if (GetObject(fontHandle, sizeof(LOGFONT), &lf) != 0)
				{
					// Emphasized font
					lf.lfHeight = 14;
					lf.lfWeight = FW_MEDIUM;
					HFONT buttonFont = CreateFontIndirect(&lf);
					SendMessageA(GetDlgItem(hWnd, IDC_INSTALL_UNINSTALL), WM_SETFONT, WPARAM(buttonFont), TRUE);
					SendMessageA(GetDlgItem(hWnd, IDC_REFRESH), WM_SETFONT, WPARAM(buttonFont), TRUE);
						SendMessageA(GetDlgItem(hWnd, IDC_REINSTALL), WM_SETFONT, WPARAM(buttonFont), TRUE);
					SendMessageA(GetDlgItem(hWnd, IDC_OPEN_INSTALL_FOLDER), WM_SETFONT, WPARAM(buttonFont), TRUE);
				}
			}

			if (isInstalled)
				SetDlgItemTextA(hWnd, IDC_INSTALL_UNINSTALL, "Uninstall");

			UpdateInstallDependentControls(hWnd);
			UpdateInstallerTheme(hWnd);

			return (INT_PTR) TRUE;
		}
		break;

		case WM_THEMECHANGED:
		case WM_SYSCOLORCHANGE:
		case WM_SETTINGCHANGE:
			UpdateInstallerTheme(hWnd);
			return (INT_PTR) TRUE;

		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
				// Install/Uninstall
				case IDC_INSTALL_UNINSTALL:
				{
					if (!isInstalled)
					{
						Install();
						isInstalled = TRUE;
						SetDlgItemTextA(hWnd, IDC_INSTALL_UNINSTALL, "Uninstall");
						UpdateInstallDependentControls(hWnd);
						MessageBoxA(hWnd, "Installation complete.", "Completion:", (MB_OK | MB_ICONASTERISK));
					}
					else
					{
						if (MessageBoxA(hWnd, "Uninstall " PROJECT_NAME"?", "Confirmation:", (MB_OKCANCEL | MB_ICONQUESTION)) == IDOK)
						{
							int ur = Uninstall();
							isInstalled = FALSE;
							SetDlgItemTextA(hWnd, IDC_INSTALL_UNINSTALL, "Install");
							UpdateInstallDependentControls(hWnd);

							if (ur == 0)
							{
								char msg[512];
								sprintf_s(msg, sizeof(msg), PROJECT_NAME " registry uninstalled, but to complete the uninstallation manually delete the\n\"%S\"\nfolder after this dialog closes.", myPathGlobal);
								MessageBoxA(hWnd, msg, "Completion:", (MB_OK | MB_ICONASTERISK));
							}
							else
								MessageBoxA(hWnd, PROJECT_NAME " uninstalled.", "Completion:", (MB_OK | MB_ICONASTERISK));
						}
					}

					return (INT_PTR) TRUE;
				}
				break;

				case IDC_REINSTALL:
				{
					Install();
					isInstalled = TRUE;
					isRunningOutsideInstallFolder = FALSE;
					SetDlgItemTextA(hWnd, IDC_INSTALL_UNINSTALL, "Uninstall");
					UpdateInstallDependentControls(hWnd);
					MessageBoxA(hWnd, "Re-installation complete.", "Completion:", (MB_OK | MB_ICONASTERISK));
					return (INT_PTR) TRUE;
				}
				break;

				// Refresh Windows icon cache DB
				case IDC_REFRESH:
				{
					ResetWindowsIconCache();
					return (INT_PTR) TRUE;
				}
				break;

				case IDC_OPEN_INSTALL_FOLDER:
				{
					DWORD attr = GetFileAttributesW(myPathGlobal);
					if ((attr == INVALID_FILE_ATTRIBUTES) || !(attr & FILE_ATTRIBUTE_DIRECTORY))
					{
						MessageBoxA(hWnd, "Install folder does not exist yet.", "Info:", (MB_OK | MB_ICONINFORMATION));
						return (INT_PTR) TRUE;
					}

					if (!OpenDirectoryInExplorer(myPathGlobal))
						MessageBoxA(hWnd, "Unable to open install folder.", "Error:", (MB_OK | MB_ICONERROR));

					return (INT_PTR) TRUE;
				}
				break;

			};
		}
		break;

		case WM_CTLCOLORSTATIC:
		{
			HDC hdcStatic = (HDC) wParam;
			SetTextColor(hdcStatic, gDialogTextColor);
			SetBkColor(hdcStatic, gDialogBackgroundColor);

			DWORD id = GetDlgCtrlID((HWND)lParam);
			if (id == IDC_HYPERLINK)
			{
				SetTextColor(hdcStatic, urlColor);
				SetBkColor(hdcStatic, gDialogBackgroundColor);
			}

			if (gDialogBackgroundBrush)
				return (INT_PTR) gDialogBackgroundBrush;
		}
		break;

		case WM_CTLCOLORBTN:
		{
			HDC hdcButton = (HDC) wParam;
			SetTextColor(hdcButton, gDialogTextColor);
			SetBkColor(hdcButton, gDialogButtonColor);
			if (gDialogButtonBrush)
				return (LRESULT) gDialogButtonBrush;
		}
		break;

		case WM_DESTROY:
			if (gDialogBackgroundBrush)
			{
				DeleteObject(gDialogBackgroundBrush);
				gDialogBackgroundBrush = NULL;
			}
			if (gDialogButtonBrush)
			{
				DeleteObject(gDialogButtonBrush);
				gDialogButtonBrush = NULL;
			}
			break;

		case WM_CLOSE:		
		EndDialog(hWnd, 0);		
		break;
	}

	return (INT_PTR) FALSE;
}


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
	BOOL shellInvocation = (pCmdLine && ((wcsstr(pCmdLine, L"--folder") != NULL) || (wcsstr(pCmdLine, L"" COMMAND_FOLDER) != NULL)));

	// Should only be one instance running
	if(!shellInvocation && FindDoppelganger())
		return EXIT_FAILURE;

	// Our path from "%ProgramFiles(x86)%" base
	C_ASSERT(_countof(myPathGlobal) >= MAX_PATH);
	HRESULT hr = SHGetSpecialFolderPathW(0, myPathGlobal, CSIDL_PROGRAM_FILES, FALSE);
	if (FAILED(hr))
		CRITICAL_API_FAIL(SHGetSpecialFolderPathW, HRESULT_CODE(hr));
	if (wcscat_s(myPathGlobal, _countof(myPathGlobal), L"\\" INSTALL_FOLDER L"\\") != 0)
		CRITICAL("Path size limit error!");

	// We're installed?
	// Have registry?
	isInstalled = HasInstallRegistry();
	// Or has install folder?
	DWORD attr = GetFileAttributesW(myPathGlobal);
	isInstalled |= ((attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY));

	WCHAR currentExePath[MAX_PATH] = {};
	WCHAR installedExePath[MAX_PATH] = {};
	if (!GetModuleFileNameW(NULL, currentExePath, _countof(currentExePath)))
		CRITICAL_API_FAIL(GetModuleFileNameW, GetLastError());
	BuildInstalledExePath(installedExePath, _countof(installedExePath));
	isRunningOutsideInstallFolder = !AreSamePath(currentExePath, installedExePath);
	
	// Icon resource set index per OS version
	OSVERSIONINFOA nfo = { sizeof(OSVERSIONINFOA), 0,0,0};
	RtlGetVersion((PRTL_OSVERSIONINFOEXW) &nfo);
	if (nfo.dwMajorVersion >= 10)
	{
		if(nfo.dwBuildNumber >= 22000)
			iconOffsetGlobal = WIN11_ICON_OFFSET;
		else
			iconOffsetGlobal = WIN10_ICON_OFFSET;
	}
	
	// Silent registry reinstall (used by build-and-deploy.bat)
	if (pCmdLine && (wcsstr(pCmdLine, L"--reinstall-registry") != NULL))
	{
		Install();
		return EXIT_SUCCESS;
	}

	// We're passed an icon index argument?
	if (isInstalled)
	{
		if(pCmdLine && (wcslen(pCmdLine) > 0))
		{
			int argc = 0;
			LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
			if (argv && (argc > 1))
			{
				int builtInIndex = -1;
				int resourceIndex = 0;
				LPCWSTR folderArg = NULL;
				LPCWSTR resourceArg = NULL;

				for (int i = 1; i < argc; i++)
				{
					if (wcsncmp(argv[i], L"" COMMAND_ICON, SIZESTR(COMMAND_ICON)) == 0)
						builtInIndex = _wtoi(argv[i] + SIZESTR(COMMAND_ICON));
					else if (wcscmp(argv[i], L"--index") == 0)
					{
						if ((i + 1) < argc)
							builtInIndex = _wtoi(argv[++i]);
					}
					else if (wcsncmp(argv[i], L"" COMMAND_RESOURCE, SIZESTR(COMMAND_RESOURCE)) == 0)
						resourceArg = argv[i] + SIZESTR(COMMAND_RESOURCE);
					else if (wcscmp(argv[i], L"--resource") == 0)
					{
						if ((i + 1) < argc)
							resourceArg = argv[++i];
					}
					else if (wcsncmp(argv[i], L"" COMMAND_RESOURCE_INDEX, SIZESTR(COMMAND_RESOURCE_INDEX)) == 0)
						resourceIndex = _wtoi(argv[i] + SIZESTR(COMMAND_RESOURCE_INDEX));
					else if (wcscmp(argv[i], L"--rindex") == 0)
					{
						if ((i + 1) < argc)
							resourceIndex = _wtoi(argv[++i]);
					}
					else if (wcsncmp(argv[i], L"" COMMAND_FOLDER, SIZESTR(COMMAND_FOLDER)) == 0)
						folderArg = argv[i] + SIZESTR(COMMAND_FOLDER);
					else if (wcscmp(argv[i], L"--folder") == 0)
					{
						if ((i + 1) < argc)
							folderArg = argv[++i];
					}
				}

				if (folderArg)
				{
					MigrateLegacyFolderIconIndex((LPWSTR) folderArg);

					if (builtInIndex >= 0)
						SetFolderColor(builtInIndex, (LPWSTR) folderArg);
					else if (resourceArg && resourceArg[0])
						SetFolderIconResource(resourceArg, resourceIndex, (LPWSTR) folderArg);
					else
						return ShowFolderIconPicker(folderArg);
				}

				LocalFree(argv);
			}

			return EXIT_SUCCESS;
		}
	}

	return (int) DialogBoxParamA(hInstance, (LPCSTR) IDD_MAIN, 0, &DlgProc, 0);
}