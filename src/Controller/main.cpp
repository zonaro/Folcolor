// Folcolor(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"
#include <versionhelpers.h>
#include <cwctype>
#include "resource.h"
#include "FolderColorize.h"
#include "GeneratedColorNames.h"

#define APP_URL "http://www.folcolor.com/"

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
#define URL_BACK_COLOR RGB(76, 76, 76)
#define URL_FONT_HEIGHT 18
#define URL_FONT_WIDTH FW_SEMIBOLD
static HFONT tweakedFont = NULL, undlineFont = NULL;
static BOOL isClicking = FALSE, isVisited = FALSE;
static COLORREF urlColor = RGB(245, 245, 245);
static HBRUSH buttonBrush = NULL;
static HCURSOR mouseOverCursor = NULL;

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

#define PICKER_SEARCH_DEBOUNCE_TIMER_ID 1
#define PICKER_SEARCH_DEBOUNCE_MS 500

/** Large icon displayed in the preview box. */
static HICON gPickerPreviewIcon      = NULL;
static BOOL  gPickerPreviewOwnsIcon  = FALSE;
static BOOL  gPickerPreviewIsFolder  = FALSE; // TRUE when showing the folder's current icon
static BOOL  gPickerPreviewIsDefault = FALSE; // TRUE when showing the OS default folder icon (no custom icon set)

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

			PickerVisibleItem visible = {};
			visible.categoryIndex = (int) c;
			visible.itemIndex = (int) i;
			visible.displayLabel = L"[" + cat.name + L"] " + item.label;
			gPickerVisibleItems.push_back(visible);
		}
	}
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

	std::wstring folderLabel = rel.empty() ? L"Custom ICO" : (L"Folder: " + rel);
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
				std::wstring dllLabel = rel.empty() ? (L"DLL: " + std::wstring(fd.cFileName)) : (L"DLL: " + rel + L"\\" + fd.cFileName);
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
 * Apply current search text to item list immediately.
 */
static void ApplyPickerSearchNow(HWND hWnd)
{
	UpdatePickerSearchQuery(hWnd);
	HWND hCat = GetDlgItem(hWnd, IDC_PICKER_CATEGORY);
	int sel = (int) SendMessageW(hCat, LB_GETCURSEL, 0, 0);
	PopulatePickerItems(hWnd, sel);
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


/**
 * Picker window procedure.
 */
static LRESULT CALLBACK PickerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CREATE:
		{
			CreateWindowW(L"STATIC", L"Category", WS_CHILD | WS_VISIBLE,
				12, 12, 250, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_BORDER,
				12, 32, 250, 360, hWnd, (HMENU) IDC_PICKER_CATEGORY, NULL, NULL);

			CreateWindowW(L"STATIC", L"Icons", WS_CHILD | WS_VISIBLE,
				274, 12, 80, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"STATIC", L"Search", WS_CHILD | WS_VISIBLE,
				356, 12, 48, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
				408, 10, 132, 22, hWnd, (HMENU) IDC_PICKER_SEARCH, NULL, NULL);
			CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_BORDER | LBS_OWNERDRAWFIXED,
				274, 32, 260, 360, hWnd, (HMENU) IDC_PICKER_ITEM, NULL, NULL);

			// --- Preview panel (right of icon list) ---
			CreateWindowW(L"STATIC", L"Preview", WS_CHILD | WS_VISIBLE,
				546, 12, 150, 18, hWnd, (HMENU) IDC_PICKER_PREVIEW_LABEL, NULL, NULL);
			CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY | WS_BORDER,
				546, 32, 150, 150, hWnd, (HMENU) IDC_PICKER_PREVIEW, NULL, NULL);

			CreateWindowW(L"BUTTON", L"Apply Icon", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				274, 404, 95, 28, hWnd, (HMENU) IDC_PICKER_APPLY, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Import Icon", WS_CHILD | WS_VISIBLE,
				375, 404, 95, 28, hWnd, (HMENU) IDC_PICKER_IMPORT, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Open Icons Folder", WS_CHILD | WS_VISIBLE,
				138, 404, 130, 28, hWnd, (HMENU) IDC_PICKER_OPEN_ICONS, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Restore Default", WS_CHILD | WS_VISIBLE,
				12, 404, 120, 28, hWnd, (HMENU) IDC_PICKER_RESTORE, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
				476, 404, 70, 28, hWnd, (HMENU) IDC_PICKER_CANCEL, NULL, NULL);

			PopulatePickerCategories(hWnd);

			// Show the folder's existing icon in the preview until user picks something.
			LoadFolderCurrentPreviewIcon();
			UpdatePickerPreviewLabel(hWnd);
		}
		return 0;

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
				FillRect(di->hDC, &di->rcItem, GetSysColorBrush(COLOR_WINDOW));
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
			COLORREF textColor = GetSysColor(COLOR_WINDOWTEXT);
			if (di->itemState & ODS_SELECTED)
			{
				bgBrush = GetSysColorBrush(COLOR_HIGHLIGHT);
				textColor = GetSysColor(COLOR_HIGHLIGHTTEXT);
			}
			else
				bgBrush = GetSysColorBrush(COLOR_WINDOW);

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

					char msg[1024];
					sprintf_s(msg, sizeof(msg),
						"Imported %u file(s), converted %u image(s), failed %u file(s).",
						copiedCount, convertedCount, failedCount);
					AppendFailedImportList(msg, sizeof(msg), failedFiles);
					MessageBoxA(hWnd, msg, "Completion:", (MB_OK | (failedCount ? MB_ICONWARNING : MB_ICONASTERISK)));
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
	wc.lpszClassName = L"FolcolorRuntimePicker";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconA((HINSTANCE) GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	HWND hWnd = CreateWindowExW(
		WS_EX_APPWINDOW,
		wc.lpszClassName,
		L"Color Folder",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 730, 480,
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

			return (INT_PTR) TRUE;
		}
		break;

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
			DWORD id = GetDlgCtrlID((HWND)lParam);
			if (id == IDC_HYPERLINK)
			{
				HDC hdcStatic = (HDC) wParam;
				SetTextColor(hdcStatic, urlColor);
				SetBkColor(hdcStatic, URL_BACK_COLOR);
				return (INT_PTR) GetStockObject(NULL_PEN);
			}
		}
		break;

		case WM_CTLCOLORBTN:
		{
			if (!buttonBrush)
				buttonBrush = CreateSolidBrush(RGB(128, 100, 100));
			return (LRESULT) buttonBrush;
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