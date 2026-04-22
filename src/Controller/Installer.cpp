// Folcolor(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"
#include "resource.h"
#include "FolderColorize.h"
#include <string>
#include <vector>
#include <algorithm>

extern WCHAR myPathGlobal[MAX_PATH];
extern int iconOffsetGlobal;


// Icon index to color label
static const LPCWSTR nameTable[COLOR_ICON_COUNT] =
{
L"Red",
L"Pink",
L"Purple",
L"Blue",
L"Cyan",
L"Teal",
L"Green",
L"Lime",
L"Yellow",
L"Orange",
L"Brown",
L"Grey",
L"Blue Grey",
L"Black",
};


// Return TRUE if system has our installed registry entry
BOOL HasInstallRegistry()
{
HKEY rootKey = NULL;
LSTATUS lStatus = RegOpenKeyExA(HKEY_CLASSES_ROOT, REGISTRY_PATH, 0, KEY_READ, &rootKey);
if (lStatus == ERROR_SUCCESS)
{
RegCloseKey(rootKey);
return TRUE;
}
return FALSE;
}


/**
 * Build absolute path to the installed executable.
 */
static std::wstring BuildInstalledExePath()
{
WCHAR exePath[MAX_PATH];
if (_snwprintf_s(exePath, _countof(exePath), (_countof(exePath) - 1), L"%s%S", myPathGlobal, TARGET_NAME) < 1)
CRITICAL("Path size limit error!");
return std::wstring(exePath);
}


/**
 * Create a subkey and fail hard on registry errors.
 */
static HKEY CreateSubKeyWOrFail(HKEY root, LPCWSTR path)
{
HKEY outKey = NULL;
LSTATUS lStatus = RegCreateKeyExW(root, path, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &outKey, NULL);
if (lStatus != ERROR_SUCCESS)
CRITICAL_API_FAIL(RegCreateKeyExW, lStatus);
return outKey;
}


/**
 * Write REG_SZ value to the registry.
 */
static void WriteRegSzWOrFail(HKEY key, LPCWSTR name, const std::wstring& value)
{
LPCWSTR valueName = (name && name[0]) ? name : NULL;
DWORD sizeBytes = DWORD((value.size() + 1) * sizeof(WCHAR));
LSTATUS lStatus = RegSetValueExW(key, valueName, 0, REG_SZ, (const BYTE*) value.c_str(), sizeBytes);
if (lStatus != ERROR_SUCCESS)
CRITICAL_API_FAIL(RegSetValueExW, lStatus);
}


/**
 * Write REG_DWORD value to the registry.
 */
static void WriteRegDwordWOrFail(HKEY key, LPCWSTR name, DWORD value)
{
LSTATUS lStatus = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*) &value, sizeof(value));
if (lStatus != ERROR_SUCCESS)
CRITICAL_API_FAIL(RegSetValueExW, lStatus);
}


/**
 * Build deterministic numeric key names to preserve menu order in shell.
 */
static std::wstring MakeOrderedKeyName(UINT order)
{
WCHAR num[12];
swprintf_s(num, _countof(num), L"%04u", order);
return std::wstring(num);
}


/**
 * Build command line for built-in color icons.
 */
static std::wstring BuildBuiltInCommand(const std::wstring& exePath, int index)
{
WCHAR cmd[2048];
if (_snwprintf_s(cmd, _countof(cmd), (_countof(cmd) - 1),
L"\"%s\" " L"" COMMAND_ICON L"%d " L"" COMMAND_FOLDER L"\"%%1\"",
exePath.c_str(), index) < 1)
{
CRITICAL("Path size limit error!");
}
return std::wstring(cmd);
}


/**
 * Build command line for external icon resources (ico/dll,index).
 */
static std::wstring BuildResourceCommand(const std::wstring& exePath, const std::wstring& resourcePath, int index)
{
WCHAR cmd[4096];
if (_snwprintf_s(cmd, _countof(cmd), (_countof(cmd) - 1),
L"\"%s\" " L"" COMMAND_RESOURCE L"\"%s\" " L"" COMMAND_RESOURCE_INDEX L"%d " L"" COMMAND_FOLDER L"\"%%1\"",
exePath.c_str(), resourcePath.c_str(), index) < 1)
{
CRITICAL("Path size limit error!");
}
return std::wstring(cmd);
}


/**
 * Build icon value format expected by shell menu entries: file,index.
 */
static std::wstring BuildIconSpec(const std::wstring& filePath, int index)
{
WCHAR icon[4096];
if (_snwprintf_s(icon, _countof(icon), (_countof(icon) - 1), L"%s,%d", filePath.c_str(), index) < 1)
CRITICAL("Path size limit error!");
return std::wstring(icon);
}


/**
 * Add command menu item under a shell key.
 */
static void AddCommandItem(HKEY parentShellKey, UINT& order, const std::wstring& label, const std::wstring& icon, const std::wstring& command, BOOL separatorBefore)
{
std::wstring keyName = MakeOrderedKeyName(order++);
HKEY itemKey = CreateSubKeyWOrFail(parentShellKey, keyName.c_str());
WriteRegSzWOrFail(itemKey, L"MUIVerb", label);
if (!icon.empty())
WriteRegSzWOrFail(itemKey, L"Icon", icon);
if (separatorBefore)
WriteRegDwordWOrFail(itemKey, L"CommandFlags", 0x20);

HKEY commandKey = CreateSubKeyWOrFail(itemKey, L"command");
WriteRegSzWOrFail(commandKey, NULL, command);
RegCloseKey(commandKey);
RegCloseKey(itemKey);
}


/**
 * Add submenu container and return its child shell key.
 */
static HKEY AddSubmenu(HKEY parentShellKey, UINT& order, const std::wstring& label, const std::wstring& icon)
{
std::wstring keyName = MakeOrderedKeyName(order++);
HKEY itemKey = CreateSubKeyWOrFail(parentShellKey, keyName.c_str());
WriteRegSzWOrFail(itemKey, L"MUIVerb", label);
if (!icon.empty())
WriteRegSzWOrFail(itemKey, L"Icon", icon);
WriteRegSzWOrFail(itemKey, L"SubCommands", L"");

HKEY shellKey = CreateSubKeyWOrFail(itemKey, L"shell");
RegCloseKey(itemKey);
return shellKey;
}


/**
 * Case-insensitive extension comparison helper.
 */
static BOOL HasExt(const std::wstring& path, LPCWSTR ext)
{
LPCWSTR e = PathFindExtensionW(path.c_str());
return (e && (_wcsicmp(e, ext) == 0));
}


/**
 * Return filename without extension for menu labels.
 */
static std::wstring FileStem(const std::wstring& fileName)
{
WCHAR copy[MAX_PATH];
wcsncpy_s(copy, _countof(copy), fileName.c_str(), _TRUNCATE);
PathRemoveExtensionW(copy);
return std::wstring(copy);
}


/**
 * Check recursively whether a folder has usable custom icon files.
 */
static BOOL HasCustomItemsRecursive(const std::wstring& dirPath)
{
std::wstring pattern = dirPath + L"\\*";
WIN32_FIND_DATAW fd;
HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
if (hFind == INVALID_HANDLE_VALUE)
return FALSE;

BOOL found = FALSE;
do
{
if ((wcscmp(fd.cFileName, L".") == 0) || (wcscmp(fd.cFileName, L"..") == 0))
continue;

std::wstring full = dirPath + L"\\" + fd.cFileName;
if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
{
if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && HasCustomItemsRecursive(full))
{
found = TRUE;
break;
}
}
else
{
if (HasExt(full, L".ico"))
{
found = TRUE;
break;
}
if (HasExt(full, L".dll"))
{
UINT iconCount = ExtractIconExW(full.c_str(), -1, NULL, NULL, 0);
if ((iconCount != UINT_MAX) && (iconCount > 0))
{
found = TRUE;
break;
}
}
}
}
while (FindNextFileW(hFind, &fd));

FindClose(hFind);
return found;
}


/**
 * Recursively write custom icon entries from the user icons folder.
 */
static void WriteCustomEntries(HKEY parentShellKey, const std::wstring& dirPath, const std::wstring& exePath)
{
std::vector<std::wstring> directories;
std::vector<std::wstring> files;

std::wstring pattern = dirPath + L"\\*";
WIN32_FIND_DATAW fd;
HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
if (hFind == INVALID_HANDLE_VALUE)
return;

do
{
if ((wcscmp(fd.cFileName, L".") == 0) || (wcscmp(fd.cFileName, L"..") == 0))
continue;

if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
{
if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
directories.push_back(fd.cFileName);
}
else
{
std::wstring full = dirPath + L"\\" + fd.cFileName;
if (HasExt(full, L".ico") || HasExt(full, L".dll"))
files.push_back(fd.cFileName);
}
}
while (FindNextFileW(hFind, &fd));

FindClose(hFind);

auto sortNoCase = [](const std::wstring& a, const std::wstring& b)
{
return _wcsicmp(a.c_str(), b.c_str()) < 0;
};
std::sort(directories.begin(), directories.end(), sortNoCase);
std::sort(files.begin(), files.end(), sortNoCase);

UINT order = 0;

for (size_t i = 0; i < directories.size(); i++)
{
std::wstring full = dirPath + L"\\" + directories[i];
if (!HasCustomItemsRecursive(full))
continue;

HKEY submenuShell = AddSubmenu(parentShellKey, order, directories[i], L"");
WriteCustomEntries(submenuShell, full, exePath);
RegCloseKey(submenuShell);
}

for (size_t i = 0; i < files.size(); i++)
{
std::wstring full = dirPath + L"\\" + files[i];
if (HasExt(full, L".ico"))
{
std::wstring label = FileStem(files[i]);
std::wstring iconSpec = BuildIconSpec(full, 0);
std::wstring command = BuildResourceCommand(exePath, full, 0);
AddCommandItem(parentShellKey, order, label, iconSpec, command, FALSE);
}
else if (HasExt(full, L".dll"))
{
UINT iconCount = ExtractIconExW(full.c_str(), -1, NULL, NULL, 0);
if ((iconCount == UINT_MAX) || (iconCount == 0))
continue;

HKEY dllShell = AddSubmenu(parentShellKey, order, FileStem(files[i]), BuildIconSpec(full, 0));
UINT dllOrder = 0;
for (UINT iconIndex = 0; iconIndex < iconCount; iconIndex++)
{
WCHAR label[64];
swprintf_s(label, _countof(label), L"Icon %03u", iconIndex);
std::wstring iconSpec = BuildIconSpec(full, (int) iconIndex);
std::wstring command = BuildResourceCommand(exePath, full, (int) iconIndex);
AddCommandItem(dllShell, dllOrder, label, iconSpec, command, FALSE);
}
RegCloseKey(dllShell);
}
}
}


// Write our shell registry
static void InstallRegistry()
{
// Delete our existing key if it's already there
DeleteRegistryPath(HKEY_CLASSES_ROOT, REGISTRY_PATH);

// Root: HKEY_CLASSES_ROOT\Directory\shell\Folcolor
HKEY rootKey = NULL;
LSTATUS lStatus = RegCreateKeyExA(HKEY_CLASSES_ROOT, REGISTRY_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &rootKey, NULL);
if (lStatus != ERROR_SUCCESS)
CRITICAL_API_FAIL(RegCreateKeyExA, lStatus);

std::wstring exePath = BuildInstalledExePath();

// Root command entry
WriteRegSzWOrFail(rootKey, L"MUIVerb", L"Color Folder");
WriteRegSzWOrFail(rootKey, L"SubCommands", L"");
WriteRegSzWOrFail(rootKey, L"Icon", exePath);

// "shell" sub-level
// HKEY_CLASSES_ROOT\Directory\shell\Folcolor\shell
HKEY shellKey = CreateSubKeyWOrFail(rootKey, L"shell");

// Colors submenu always comes first
UINT order = 0;
HKEY colorsShell = AddSubmenu(shellKey, order, L"Colors", exePath);
UINT colorsOrder = 0;
for (UINT i = 0; i < COLOR_ICON_COUNT; i++)
{
std::wstring iconSpec = BuildIconSpec(exePath, (int) (i + (UINT) iconOffsetGlobal));
std::wstring command = BuildBuiltInCommand(exePath, (int) i);
AddCommandItem(colorsShell, colorsOrder, nameTable[i], iconSpec, command, FALSE);
}
RegCloseKey(colorsShell);

// Custom icons from install folder "icons"
std::wstring customPath = std::wstring(myPathGlobal) + L"icons";
DWORD attr = GetFileAttributesW(customPath.c_str());
if ((attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY) && HasCustomItemsRecursive(customPath))
{
HKEY customShell = AddSubmenu(shellKey, order, L"Custom", L"");
WriteCustomEntries(customShell, customPath, exePath);
RegCloseKey(customShell);
}

// "Restore Default" entry
AddCommandItem(
shellKey,
order,
L"Restore Default",
L"%SystemRoot%\\system32\\shell32.dll,4",
BuildBuiltInCommand(exePath, COLOR_ICON_COUNT),
TRUE);

// "Launch Folcolor" entry
{
std::wstring keyName = MakeOrderedKeyName(order++);
HKEY launchKey = CreateSubKeyWOrFail(shellKey, keyName.c_str());
WriteRegSzWOrFail(launchKey, L"MUIVerb", L"Launch Folcolor");
WriteRegSzWOrFail(launchKey, L"Icon", exePath);
WriteRegSzWOrFail(launchKey, L"HasLUAShield", L"");
WriteRegDwordWOrFail(launchKey, L"CommandFlags", 0x20);

HKEY commandKey = CreateSubKeyWOrFail(launchKey, L"command");
WCHAR cmd[2048];
if (_snwprintf_s(cmd, _countof(cmd), (_countof(cmd) - 1), L"\"%s\"", exePath.c_str()) < 1)
CRITICAL("Path size limit error!");
WriteRegSzWOrFail(commandKey, NULL, cmd);
RegCloseKey(commandKey);
RegCloseKey(launchKey);
}

RegCloseKey(shellKey);
RegCloseKey(rootKey);
}


// Install ourself
void Install()
{
// Create installation folder
if (!CreateDirectoryW(myPathGlobal, NULL))
CRITICAL_API_FAIL(CreateDirectoryW, GetLastError());

// Copy ourself there
// ------------------------------------------------------------------------
WCHAR myPath[MAX_PATH];
if(!GetModuleFileNameW(NULL, myPath, _countof(myPath)))
CRITICAL_API_FAIL(GetModuleFileNameW, GetLastError());

WCHAR myName[_MAX_FNAME];
if (!GetModuleBaseNameW(GetCurrentProcess(), NULL, myName, _countof(myName)))
CRITICAL_API_FAIL(GetModuleBaseNameW, GetLastError());
WCHAR targetPath[MAX_PATH];
if(_snwprintf_s(targetPath, _countof(targetPath), _countof(targetPath)-1, L"%s%s", myPathGlobal, myName) < 1)
CRITICAL("Path size limit error!");

if(!CopyFileW(myPath, targetPath, FALSE))
CRITICAL_API_FAIL(CopyFileW, GetLastError());
// ------------------------------------------------------------------------

// And "README.md" file if it exists
// #TODO: Could have README.md as an embedded resource and extract it on demand
if (PathRemoveFileSpecW(myPath))
{
if (wcscat_s(myPath, _countof(myPath), L"\\README.md") != 0)
CRITICAL("Path size limit error!");

if (_snwprintf_s(targetPath, _countof(targetPath), _countof(targetPath)-1, L"%sREADME.md", myPathGlobal) < 1)
CRITICAL("Path size limit error!");

CopyFileW(myPath, targetPath, FALSE);
}

// Ensure custom icons folder exists
WCHAR iconsPath[MAX_PATH];
if (_snwprintf_s(iconsPath, _countof(iconsPath), (_countof(iconsPath) - 1), L"%sicons", myPathGlobal) < 1)
CRITICAL("Path size limit error!");
if (!CreateDirectoryW(iconsPath, NULL))
{
DWORD gle = GetLastError();
if (gle != ERROR_ALREADY_EXISTS)
CRITICAL_API_FAIL(CreateDirectoryW, gle);
}

// ------------------------------------------------------------------------

// TODO: Put UAC trick stuff here


// ------------------------------------------------------------------------

InstallRegistry();

// Without this the app icon might not show up in explorer right away
ResetWindowsIconCache();
}


// Uninstall ourself
// Returns 0 = Needs manual uninstall step, 1 = complete
int Uninstall()
{
// Remove our registry key
DeleteRegistryPath(HKEY_CLASSES_ROOT, REGISTRY_PATH);
ResetWindowsIconCache();

// Double check the path to avoid a disaster
if (wcsstr(myPathGlobal, INSTALL_FOLDER))
{
// Copy of our path without ending backslash and to be double terminated
WCHAR myPathCopy[MAX_PATH + 1];
ZeroMemory(myPathCopy, sizeof(myPathCopy));
if (wcsncpy_s(myPathCopy, MAX_PATH, myPathGlobal, wcslen(myPathGlobal) - 1) == 0)
{
// No, recursively delete our installation folder
// Note: Might fail (return = 2) while debug stepping, but then fine in release for what ever reason..
SHFILEOPSTRUCTW nfo =
{
NULL,
FO_DELETE,
myPathCopy,
NULL,
(FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_ALLOWUNDO),
FALSE,
NULL,
NULL
};
SHFileOperationW(&nfo);
}
}

// Return 0 if installation folder is still there
DWORD attr = GetFileAttributesW(myPathGlobal);
return !((attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY));
}
