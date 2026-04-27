
// Foldrion(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"
#include "resource.h"
#include "FolderColorize.h"

extern WCHAR myPathGlobal[MAX_PATH];
extern int iconOffsetGlobal;

// Legacy embedded ranges from old 14-color builds.
static const int LEGACY_WIN10_OFFSET = 2;
static const int LEGACY_WIN78_OFFSET = 16;
static const int LEGACY_WIN11_OFFSET = 30;
static const int LEGACY_COLOR_ICON_COUNT = 14;


/**
Try to map a legacy built-in icon index to the current icon index range.
Returns TRUE and writes outNewIndex when a known legacy index is detected.
*/
static BOOL TryMapLegacyBuiltInIndex(int oldIndex, int* outNewIndex)
{
	if (!outNewIndex)
		return FALSE;

	if ((oldIndex >= LEGACY_WIN10_OFFSET) && (oldIndex < (LEGACY_WIN10_OFFSET + LEGACY_COLOR_ICON_COUNT)))
	{
		*outNewIndex = WIN10_ICON_OFFSET + (oldIndex - LEGACY_WIN10_OFFSET);
		return TRUE;
	}

	if ((oldIndex >= LEGACY_WIN78_OFFSET) && (oldIndex < (LEGACY_WIN78_OFFSET + LEGACY_COLOR_ICON_COUNT)))
	{
		*outNewIndex = WIN7_ICON_OFFSET + (oldIndex - LEGACY_WIN78_OFFSET);
		return TRUE;
	}

	if ((oldIndex >= LEGACY_WIN11_OFFSET) && (oldIndex < (LEGACY_WIN11_OFFSET + LEGACY_COLOR_ICON_COUNT)))
	{
		*outNewIndex = WIN11_ICON_OFFSET + (oldIndex - LEGACY_WIN11_OFFSET);
		return TRUE;
	}

	return FALSE;
}


/**
Mark a folder desktop.ini as a Foldrion-customized folder type.
This enables the shell to show Foldrion-only verbs for customized folders.
*/
static void SetFoldrionDirectoryClass(LPCWSTR initPath)
{
	if (!initPath || !initPath[0])
		return;

	WritePrivateProfileStringW(L".ShellClassInfo", L"DirectoryClass", FOLDER_PROGID_W, initPath);
}


/**
Remove the Foldrion folder type marker if this desktop.ini belongs to us.
Leaves third-party DirectoryClass values untouched.
*/
static void ClearFoldrionDirectoryClass(LPCWSTR initPath)
{
	if (!initPath || !initPath[0])
		return;

	WCHAR directoryClass[256] = {};
	GetPrivateProfileStringW(L".ShellClassInfo", L"DirectoryClass", L"", directoryClass, _countof(directoryClass), initPath);
	if (_wcsicmp(directoryClass, FOLDER_PROGID_W) != 0)
		return;

	WritePrivateProfileStringW(L".ShellClassInfo", L"DirectoryClass", NULL, initPath);
}


// Restore default folder icon by removing the folder "desktop.ini"
static void RestoreFolderIcon(LPWSTR widePath)
{
	// Skip if the folder system flag is not set
	if (widePath && PathIsSystemFolderW(widePath, 0))
	{
		WCHAR initPath[MAX_PATH];
		_snwprintf_s(initPath, MAX_PATH, (MAX_PATH-1), L"%s\\desktop.ini", widePath);

		// ini file exists?
		DWORD attr = GetFileAttributesW(initPath);
		if ((attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY))
		{
			// Yes, detect if the desktop.ini setup has other settings like a custom background, tip, etc.
			// Be nice to the user and not delete there custom "desktop.ini" setup for the folder if they have one.
			BOOL keepIt = FALSE;

			// Read in the the whole text block
			FILE *fp = NULL;
			errno_t err = _wfopen_s(&fp, initPath, L"rb");
			if (err == 0)
			{				
				long size = fsize(fp);
				if (size > 0)
				{
					LPSTR buffer = (LPSTR) malloc(size + 1);
					if (buffer)
					{
						// Ensure it's zero terminated
						buffer[size] = 0;

						if (fread_s(buffer, size, size, 1, fp) == 1)
						{							
							// Quick test if the file has a GUID def typical of extended attributes
							if (strchr(buffer, '{'))
								keepIt = TRUE;
							else
							{
								// Expanded search
								_strlwr_s(buffer, (size + 1));
								static const char *strLst[] =
								{
									"[extshellfolderviews]",
									"[viewstate]",
									"iconarea_image=",
									"iconarea_text=",
									"infotip=",
									"nosharing=",
									"logo="
								};

								for (UINT i = 0; i < _countof(strLst); i++)
								{
									if (strstr(buffer, strLst[i]) != NULL)
									{
										keepIt = TRUE;
										break;
									}
								}
							}
						}

						free(buffer);
					}
				}

				fclose(fp);
			}

			if (keepIt)
			{
				// Only remove the icon fields to fall back to the OS default icon
				WritePrivateProfileStringW(L".ShellClassInfo", L"IconFile", NULL, initPath);
				WritePrivateProfileStringW(L".ShellClassInfo", L"IconIndex", NULL, initPath);
				WritePrivateProfileStringW(L".ShellClassInfo", L"IconResource", NULL, initPath);
				ClearFoldrionDirectoryClass(initPath);

				// If there no fields left now in the ".ShellClassInfo" section remove it from the desktop.ini
				WCHAR buffer[1024];
				DWORD ppsr = GetPrivateProfileSectionW(L".ShellClassInfo", buffer, _countof(buffer), initPath);
				if (ppsr == 0)
					WritePrivateProfileStringW(L".ShellClassInfo", NULL, NULL, initPath);
			}
			else
			{
				// Detected nothing no significant settings, so we can delete it
				DeleteFileW(initPath);

				// Remove folder "system" flag too
				// #TODO: Looks like other tools don't do this step, any bad use cases?
				PathUnmakeSystemFolderW(widePath);
			}
		}
		else
		{
			// No, remove folder "system" flag
			// #TODO: Looks like other tools don't do this step, any bad use cases?
			PathUnmakeSystemFolderW(widePath);
		}
	}
}


/**
Migrate legacy Foldrion/Folcolor desktop.ini icon indices (14-color era) to
the current embedded index ranges. This keeps old customized folders aligned
after resource table expansions.
*/
void MigrateLegacyFolderIconIndex(LPWSTR folderPath)
{
	if (!folderPath || !folderPath[0])
		return;

	DWORD attr = GetFileAttributesW(folderPath);
	if ((attr == INVALID_FILE_ATTRIBUTES) || !(attr & FILE_ATTRIBUTE_DIRECTORY))
		return;

	WCHAR initPath[MAX_PATH] = {};
	if (_snwprintf_s(initPath, MAX_PATH, (MAX_PATH - 1), L"%s\\desktop.ini", folderPath) < 1)
		return;

	DWORD iniAttr = GetFileAttributesW(initPath);
	if ((iniAttr == INVALID_FILE_ATTRIBUTES) || (iniAttr & FILE_ATTRIBUTE_DIRECTORY))
		return;

	SHFOLDERCUSTOMSETTINGS pfcs = {};
	pfcs.dwSize = sizeof(SHFOLDERCUSTOMSETTINGS);
	pfcs.dwMask = FCSM_ICONFILE;

	WCHAR iconPath[MAX_PATH] = {};
	pfcs.pszIconFile = iconPath;
	pfcs.cchIconFile = MAX_PATH;

	if (FAILED(SHGetSetFolderCustomSettings(&pfcs, folderPath, FCS_READ)))
		return;

	if (!iconPath[0])
		return;

	WCHAR lowerPath[MAX_PATH] = {};
	if (wcscpy_s(lowerPath, _countof(lowerPath), iconPath) != 0)
		return;
	if (_wcslwr_s(lowerPath) != 0)
		return;

	if (!StrStrIW(lowerPath, L"\\foldrion.exe") && !StrStrIW(lowerPath, L"\\folcolor.exe"))
		return;

	int mappedIndex = 0;
	if (!TryMapLegacyBuiltInIndex(pfcs.iIconIndex, &mappedIndex))
		return;

	WCHAR iconResource[MAX_PATH + 16] = {};
	if (_snwprintf_s(iconResource, _countof(iconResource), (_countof(iconResource) - 1), L"%s,%d", iconPath, mappedIndex) < 1)
		return;

	WritePrivateProfileStringW(L".ShellClassInfo", L"IconResource", iconResource, initPath);
	SetFoldrionDirectoryClass(initPath);
	PathMakeSystemFolderW(folderPath);
}


// Set folder color icon for a given folder
void SetFolderColor(int index, LPWSTR folderPath)
{
	//trace("SetFolderColor: %d, \"%S\"\n", index, folderPath);
	if (!folderPath || (index < 0) || (index > COLOR_ICON_COUNT))
		return;

	// Shouldn't happen, but verify it's an exiting folder first
	DWORD attr = GetFileAttributesW(folderPath);
	if ((attr == INVALID_FILE_ATTRIBUTES) || !(attr & FILE_ATTRIBUTE_DIRECTORY))
		return;
	
	// Path to a "desktop.ini"
	WCHAR initPath[MAX_PATH];
	if (_snwprintf_s(initPath, MAX_PATH, (MAX_PATH-1), L"%s\\desktop.ini", folderPath) < 1)
		CRITICAL("Path size limit error!");

	// Folder already has system flag?
	BOOL hasIniAlready = FALSE;
	if (PathIsSystemFolderW(folderPath, 0))
	{
		// Yes, a "desktop.ini" there?
		DWORD attr = GetFileAttributesW(initPath);
		hasIniAlready = ((attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY));
		if (hasIniAlready)
		{
			// Yes, has an icon entry?
			// SHGetSetFolderCustomSettings() read combines "IconFile", "IconIndex" and "IconResource" types.
			SHFOLDERCUSTOMSETTINGS pfcs;
			ZeroMemory(&pfcs, sizeof(SHFOLDERCUSTOMSETTINGS));
			pfcs.dwSize = sizeof(SHFOLDERCUSTOMSETTINGS);
			pfcs.dwMask = FCSM_ICONFILE;
			WCHAR iconPath[MAX_PATH] = { 0 };
			pfcs.pszIconFile = iconPath;
			pfcs.cchIconFile = MAX_PATH;
			if (SUCCEEDED(SHGetSetFolderCustomSettings(&pfcs, folderPath, FCS_READ)) && iconPath[0])
			{
				// Special folder icon path?
				errno_t en = _wcslwr_s(iconPath);
				if (en != 0)
					CRITICAL_API_ERRNO(_wcslwr_s, en);

				if (wcsncmp(iconPath + SIZESTR(L"C:"), L"\\windows\\", SIZESTR(L"\\windows\\")) == 0)
				{
					// Yes, abort
					MessageBoxA(NULL,
						PROJECT_NAME " detects this as possibly a special folder (I.E. \"Downloads\", \"Documents\", \"Music\", etc.) and is not supported since restoring them is complex.\n\n"
						"If you really want to set the color/icon for this folder, manually edit or just delete the existing \"desktop.ini\" (hidden, system) file first.\n"
						"And then if you want to restore a special system folder icon later, it CAN be done through manual restore steps (Web search on how).\n"
						,
						PROJECT_NAME " abort:", (MB_OK | MB_ICONERROR));
					return;
				}
			}
		}
	}

	// Restore default folder icon?
	if (index == COLOR_ICON_COUNT)
	{
		RestoreFolderIcon(folderPath);
		ResetWindowsIconCache();
		return;
	}

	// If the desktop.ini already exists we can't use SHGetSetFolderCustomSettings() if it has other settting because of a bug in it where it will wipe out
	// other sections if "[.ShellClassInfo]" is not at the top.
	if (hasIniAlready)
	{
		// Need to remove the old first to get quick icon refresh on at least Windows 10
		RestoreFolderIcon(folderPath);

		// If desktop.ini is still here it means there are settings that needed to be saved and will have to take the delayed refresh route,
		// else we'll let it fall through and be recreated for the fast refresh option.
		DWORD attr = GetFileAttributesW(initPath);
		if ((attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY))
		{
			//trace("** hasIniAlready: Had to save the desktop.ini **\n");

			// Write our "IconResource" entry
			WCHAR iconPath[MAX_PATH];
			_snwprintf_s(iconPath, MAX_PATH, (MAX_PATH-1), L"%sFoldrion.exe,%d", myPathGlobal, (index + iconOffsetGlobal));
			WritePrivateProfileStringW(L".ShellClassInfo", L"IconResource", iconPath, initPath);
			SetFoldrionDirectoryClass(initPath);

			// Flush icon cache so the new icon setting take effect eventually
			PathMakeSystemFolderW(folderPath);
			ResetWindowsIconCache();
			return;
		}
		//else
		//	trace("** hasIniAlready: Making a new desktop.ini **\n");
	}

	// Let SHGetSetFolderCustomSettings() do the work of setting the folder as system, creating the "desktop.ini", etc.
	SHFOLDERCUSTOMSETTINGS pfcs;
	ZeroMemory(&pfcs, sizeof(SHFOLDERCUSTOMSETTINGS));
	pfcs.dwSize = sizeof(SHFOLDERCUSTOMSETTINGS);
	pfcs.dwMask = FCSM_ICONFILE;

	WCHAR iconPath[MAX_PATH];
	pfcs.pszIconFile = iconPath;
	pfcs.cchIconFile = MAX_PATH;
	_snwprintf_s(iconPath, MAX_PATH, (MAX_PATH-1), L"%sFoldrion.exe", myPathGlobal);
	pfcs.iIconIndex = (index + iconOffsetGlobal);

	HRESULT hr = SHGetSetFolderCustomSettings(&pfcs, folderPath, FCS_FORCEWRITE);
	if (FAILED(hr))
		CRITICAL_API_FAIL(SHGetSetFolderCustomSettings, HRESULT_CODE(hr));

	SetFoldrionDirectoryClass(initPath);
}


// Set folder icon from an arbitrary resource path (ICO or DLL with index)
void SetFolderIconResource(LPCWSTR iconResourcePath, int iconIndex, LPWSTR folderPath)
{
	if (!iconResourcePath || !iconResourcePath[0] || !folderPath || (iconIndex < 0))
		return;

	// Shouldn't happen, but verify it's an existing folder first
	DWORD attr = GetFileAttributesW(folderPath);
	if ((attr == INVALID_FILE_ATTRIBUTES) || !(attr & FILE_ATTRIBUTE_DIRECTORY))
		return;

	// Path to a "desktop.ini"
	WCHAR initPath[MAX_PATH];
	if (_snwprintf_s(initPath, MAX_PATH, (MAX_PATH-1), L"%s\\desktop.ini", folderPath) < 1)
		CRITICAL("Path size limit error!");

	// Folder already has system flag?
	BOOL hasIniAlready = FALSE;
	if (PathIsSystemFolderW(folderPath, 0))
	{
		// Yes, a "desktop.ini" there?
		DWORD iniAttr = GetFileAttributesW(initPath);
		hasIniAlready = ((iniAttr != INVALID_FILE_ATTRIBUTES) && !(iniAttr & FILE_ATTRIBUTE_DIRECTORY));
		if (hasIniAlready)
		{
			// Yes, has an icon entry?
			// SHGetSetFolderCustomSettings() read combines "IconFile", "IconIndex" and "IconResource" types.
			SHFOLDERCUSTOMSETTINGS pfcs;
			ZeroMemory(&pfcs, sizeof(SHFOLDERCUSTOMSETTINGS));
			pfcs.dwSize = sizeof(SHFOLDERCUSTOMSETTINGS);
			pfcs.dwMask = FCSM_ICONFILE;
			WCHAR iconPath[MAX_PATH] = { 0 };
			pfcs.pszIconFile = iconPath;
			pfcs.cchIconFile = MAX_PATH;
			if (SUCCEEDED(SHGetSetFolderCustomSettings(&pfcs, folderPath, FCS_READ)) && iconPath[0])
			{
				// Special folder icon path?
				errno_t en = _wcslwr_s(iconPath);
				if (en != 0)
					CRITICAL_API_ERRNO(_wcslwr_s, en);

				if (wcsncmp(iconPath + SIZESTR(L"C:"), L"\\windows\\", SIZESTR(L"\\windows\\")) == 0)
				{
					// Yes, abort
					MessageBoxA(NULL,
						PROJECT_NAME " detects this as possibly a special folder (I.E. \"Downloads\", \"Documents\", \"Music\", etc.) and is not supported since restoring them is complex.\n\n"
						"If you really want to set the color/icon for this folder, manually edit or just delete the existing \"desktop.ini\" (hidden, system) file first.\n"
						"And then if you want to restore a special system folder icon later, it CAN be done through manual restore steps (Web search on how).\n"
						,
						PROJECT_NAME " abort:", (MB_OK | MB_ICONERROR));
					return;
				}
			}
		}
	}

	// If the desktop.ini already exists we can't use SHGetSetFolderCustomSettings() if it has other settting because of a bug in it where it will wipe out
	// other sections if "[.ShellClassInfo]" is not at the top.
	if (hasIniAlready)
	{
		// Need to remove the old first to get quick icon refresh on at least Windows 10
		RestoreFolderIcon(folderPath);

		// If desktop.ini is still here it means there are settings that needed to be saved and will have to take the delayed refresh route,
		// else we'll let it fall through and be recreated for the fast refresh option.
		DWORD iniAttr = GetFileAttributesW(initPath);
		if ((iniAttr != INVALID_FILE_ATTRIBUTES) && !(iniAttr & FILE_ATTRIBUTE_DIRECTORY))
		{
			WCHAR iconResource[MAX_PATH + 16];
			if (_snwprintf_s(iconResource, _countof(iconResource), (_countof(iconResource)-1), L"%s,%d", iconResourcePath, iconIndex) < 1)
				CRITICAL("Path size limit error!");

			// Write our "IconResource" entry
			WritePrivateProfileStringW(L".ShellClassInfo", L"IconResource", iconResource, initPath);
			SetFoldrionDirectoryClass(initPath);

			// Flush icon cache so the new icon setting take effect eventually
			PathMakeSystemFolderW(folderPath);
			ResetWindowsIconCache();
			return;
		}
	}

	// Let SHGetSetFolderCustomSettings() do the work of setting the folder as system, creating the "desktop.ini", etc.
	SHFOLDERCUSTOMSETTINGS pfcs;
	ZeroMemory(&pfcs, sizeof(SHFOLDERCUSTOMSETTINGS));
	pfcs.dwSize = sizeof(SHFOLDERCUSTOMSETTINGS);
	pfcs.dwMask = FCSM_ICONFILE;
	pfcs.pszIconFile = (LPWSTR) iconResourcePath;
	pfcs.cchIconFile = 0;
	pfcs.iIconIndex = iconIndex;

	HRESULT hr = SHGetSetFolderCustomSettings(&pfcs, folderPath, FCS_FORCEWRITE);
	if (FAILED(hr))
		CRITICAL_API_FAIL(SHGetSetFolderCustomSettings, HRESULT_CODE(hr));

	SetFoldrionDirectoryClass(initPath);
}


// Reset the Windows icon cache and notify all applications that it changed
// Note: Some applications might not process the broadcast WM_SETTINGCHANGE message and thus will still reference cached/old icons.
void ResetWindowsIconCache()
{
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, NULL, NULL);

	DWORD_PTR smResult;
	SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) L"Software\\Classes", SMTO_ABORTIFHUNG, 5000, &smResult);
}


// Set the system-wide default folder icon by modifying the Shell Icons registry key
void SetSystemDefaultFolderIcon(LPCWSTR iconResourcePath, int iconIndex)
{
	HKEY hKey;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Icons", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
	if (status != ERROR_SUCCESS)
		return;

	WCHAR iconPath[1024];
	swprintf_s(iconPath, _countof(iconPath), L"%s,%d", iconResourcePath, iconIndex);

	status = RegSetValueExW(hKey, L"3", 0, REG_SZ, (const BYTE*)iconPath, (DWORD)((wcslen(iconPath) + 1) * sizeof(WCHAR)));
	RegCloseKey(hKey);

	if (status == ERROR_SUCCESS)
		ResetWindowsIconCache();
}


// Restore the system-wide default folder icon by removing the Shell Icons registry key
void RestoreSystemDefaultFolderIcon()
{
	HKEY hKey;
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Icons", 0, KEY_WRITE, &hKey);
	if (status != ERROR_SUCCESS)
		return;

	RegDeleteValueW(hKey, L"3");
	RegCloseKey(hKey);

	ResetWindowsIconCache();
}
