
// Folcolor(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"
#include <versionhelpers.h>
#include "resource.h"
#include "FolderColorize.h"

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
	EnableWindow(GetDlgItem(hWnd, IDC_IMPORT_ICON), isInstalled);
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


static const LPCWSTR kSystemDlls[] =
{
	L"%SystemRoot%\\System32\\imageres.dll",
	L"%SystemRoot%\\System32\\shell32.dll",
	L"%SystemRoot%\\System32\\pifmgr.dll",
	L"%SystemRoot%\\System32\\ddores.dll",
	L"%SystemRoot%\\System32\\accessibilitycpl.dll",
	L"%SystemRoot%\\System32\\mmres.dll",
	L"%SystemRoot%\\System32\\netshell.dll",
	L"%SystemRoot%\\explorer.exe",
	L"%SystemRoot%\\System32\\wmploc.dll",
};

static const LPCWSTR kColorNames[COLOR_ICON_COUNT] =
{
	L"Red", L"Pink", L"Purple", L"Blue", L"Cyan", L"Teal", L"Green",
	L"Lime", L"Yellow", L"Orange", L"Brown", L"Grey", L"Blue Grey", L"Black"
};

static std::vector<PickerCategory> gPickerCategories;
static std::wstring gPickerTargetFolder;

#define IDC_PICKER_CATEGORY 2001
#define IDC_PICKER_ITEM 2002
#define IDC_PICKER_APPLY 2003
#define IDC_PICKER_RESTORE 2004
#define IDC_PICKER_CANCEL 2005
#define IDC_PICKER_IMPORT 2006


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
 * Add built-in color icons as a category.
 */
static void AddColorsCategory(std::vector<PickerCategory>& out)
{
	PickerCategory cat;
	cat.name = L"Colors";
	for (int i = 0; i < COLOR_ICON_COUNT; i++)
	{
		PickerItem item = {};
		item.isBuiltIn = TRUE;
		item.builtInIndex = i;
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
	AddColorsCategory(out);

	for (UINT i = 0; i < _countof(kSystemDlls); i++)
	{
		std::wstring path = ExpandEnvPath(kSystemDlls[i]);
		if (!path.empty())
		{
			std::wstring label = L"System DLL: ";
			label += PathFindFileNameW(path.c_str());
			AddDllCategory(out, label, path);
		}
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

	if ((categoryIndex < 0) || (categoryIndex >= (int) gPickerCategories.size()))
		return;

	const PickerCategory& cat = gPickerCategories[categoryIndex];
	for (size_t i = 0; i < cat.items.size(); i++)
	{
		LRESULT idx = SendMessageW(hItem, LB_ADDSTRING, 0, (LPARAM) cat.items[i].label.c_str());
		if (idx != LB_ERR)
			SendMessageW(hItem, LB_SETITEMDATA, (WPARAM) idx, (LPARAM) i);
	}

	if (!cat.items.empty())
		SendMessageW(hItem, LB_SETCURSEL, 0, 0);
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
		SendMessageW(hCat, LB_SETCURSEL, 0, 0);
		PopulatePickerItems(hWnd, 0);
	}
	else
	{
		HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
		SendMessageW(hItem, LB_RESETCONTENT, 0, 0);
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
		if (ExtractIconExW(exePath, iconOffsetGlobal + item.builtInIndex, &largeIcon, &smallIcon, 1) > 0)
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
				274, 12, 300, 18, hWnd, NULL, NULL, NULL);
			CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_BORDER | LBS_OWNERDRAWFIXED,
				274, 32, 380, 360, hWnd, (HMENU) IDC_PICKER_ITEM, NULL, NULL);

			CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				274, 404, 80, 28, hWnd, (HMENU) IDC_PICKER_APPLY, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Import Icon", WS_CHILD | WS_VISIBLE,
				360, 404, 95, 28, hWnd, (HMENU) IDC_PICKER_IMPORT, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Restore Default", WS_CHILD | WS_VISIBLE,
				461, 404, 120, 28, hWnd, (HMENU) IDC_PICKER_RESTORE, NULL, NULL);
			CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
				587, 404, 70, 28, hWnd, (HMENU) IDC_PICKER_CANCEL, NULL, NULL);

			PopulatePickerCategories(hWnd);
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
			if (!di || (di->CtlID != IDC_PICKER_ITEM) || (di->itemID == UINT_MAX))
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

			HWND hCat = GetDlgItem(hWnd, IDC_PICKER_CATEGORY);
			int catSel = (int) SendMessageW(hCat, LB_GETCURSEL, 0, 0);
			if ((catSel < 0) || (catSel >= (int) gPickerCategories.size()))
				return TRUE;

			int itemIndex = (int) SendMessageW(di->hwndItem, LB_GETITEMDATA, di->itemID, 0);
			if ((itemIndex < 0) || (itemIndex >= (int) gPickerCategories[catSel].items.size()))
				return TRUE;

			PickerItem& item = gPickerCategories[catSel].items[itemIndex];
			HICON hIcon = ResolvePickerItemIcon(item);

			int iconX = di->rcItem.left + 4;
			int iconY = di->rcItem.top + ((di->rcItem.bottom - di->rcItem.top - 16) / 2);
			if (hIcon)
				DrawIconEx(di->hDC, iconX, iconY, hIcon, 16, 16, 0, NULL, DI_NORMAL);

			RECT textRc = di->rcItem;
			textRc.left += 26;
			SetBkMode(di->hDC, TRANSPARENT);
			SetTextColor(di->hDC, textColor);
			DrawTextW(di->hDC, item.label.c_str(), -1, &textRc, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			if (di->itemState & ODS_FOCUS)
				DrawFocusRect(di->hDC, &di->rcItem);

			return TRUE;
		}
		break;

		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
				case IDC_PICKER_CATEGORY:
				if (HIWORD(wParam) == LBN_SELCHANGE)
				{
					int sel = (int) SendMessageW((HWND) lParam, LB_GETCURSEL, 0, 0);
					PopulatePickerItems(hWnd, sel);
				}
				break;

				case IDC_PICKER_APPLY:
				{
					HWND hCat = GetDlgItem(hWnd, IDC_PICKER_CATEGORY);
					HWND hItem = GetDlgItem(hWnd, IDC_PICKER_ITEM);
					int catSel = (int) SendMessageW(hCat, LB_GETCURSEL, 0, 0);
					int itemSel = (int) SendMessageW(hItem, LB_GETCURSEL, 0, 0);
					if ((catSel == LB_ERR) || (itemSel == LB_ERR) || (catSel < 0) || (catSel >= (int) gPickerCategories.size()))
					{
						MessageBoxA(hWnd, "Select a category and an icon.", "Info:", MB_OK | MB_ICONINFORMATION);
						break;
					}

					int itemIndex = (int) SendMessageW(hItem, LB_GETITEMDATA, (WPARAM) itemSel, 0);
					if ((itemIndex < 0) || (itemIndex >= (int) gPickerCategories[catSel].items.size()))
						break;

					const PickerItem& item = gPickerCategories[catSel].items[itemIndex];
					if (item.isBuiltIn)
					{
						// Apply built-in colors using the currently running EXE resources.
						std::wstring exePath = GetCurrentExePath();
						if (!exePath.empty())
							SetFolderIconResource(exePath.c_str(), item.builtInIndex + iconOffsetGlobal, (LPWSTR) gPickerTargetFolder.c_str());
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

		case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;

		case WM_DESTROY:
		ReleasePickerIcons();
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
		CW_USEDEFAULT, CW_USEDEFAULT, 680, 480,
		NULL, NULL, wc.hInstance, NULL);

	if (!hWnd)
		return EXIT_FAILURE;

	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	MSG msg = {};
	while (GetMessage(&msg, NULL, 0, 0) > 0)
	{
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
					SendMessageA(GetDlgItem(hWnd, IDC_IMPORT_ICON), WM_SETFONT, WPARAM(buttonFont), TRUE);
					SendMessageA(GetDlgItem(hWnd, IDC_OPEN_ICONS_FOLDER), WM_SETFONT, WPARAM(buttonFont), TRUE);
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
						EndDialog(hWnd, 0);
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

							EndDialog(hWnd, 0);
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
					EndDialog(hWnd, 0);
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

				case IDC_OPEN_ICONS_FOLDER:
				{
					WCHAR iconsPath[MAX_PATH];
					if (_snwprintf_s(iconsPath, _countof(iconsPath), (_countof(iconsPath) - 1), L"%sicons", myPathGlobal) < 1)
					{
						MessageBoxA(hWnd, "Unable to build icons folder path.", "Error:", (MB_OK | MB_ICONERROR));
						return (INT_PTR) TRUE;
					}

					if (!EnsureDirectoryExists(iconsPath))
					{
						MessageBoxA(hWnd, "Unable to create icons folder.", "Error:", (MB_OK | MB_ICONERROR));
						return (INT_PTR) TRUE;
					}

					if (!OpenDirectoryInExplorer(iconsPath))
						MessageBoxA(hWnd, "Unable to open icons folder.", "Error:", (MB_OK | MB_ICONERROR));

					return (INT_PTR) TRUE;
				}
				break;

				case IDC_IMPORT_ICON:
				{
					UINT copiedCount = 0;
					UINT convertedCount = 0;
					UINT failedCount = 0;
					std::vector<std::wstring> failedFiles;

					if (!ImportCustomIconFiles(hWnd, &copiedCount, &convertedCount, &failedCount, &failedFiles))
						return (INT_PTR) TRUE;

					if (isInstalled)
					{
						RefreshInstalledShellMenu();
					}

					char msg[1024];
					if (isInstalled)
						sprintf_s(msg, sizeof(msg), "Imported %u file(s), converted %u image(s), failed %u file(s). The Custom menu was refreshed without restarting Explorer.", copiedCount, convertedCount, failedCount);
					else
						sprintf_s(msg, sizeof(msg), "Imported %u file(s), converted %u image(s), failed %u file(s). Install Folcolor to publish them in the context menu.", copiedCount, convertedCount, failedCount);

					AppendFailedImportList(msg, sizeof(msg), failedFiles);
					MessageBoxA(hWnd, msg, "Completion:", (MB_OK | (failedCount ? MB_ICONWARNING : MB_ICONASTERISK)));
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