
#pragma once
#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0601 // _WIN32_WINNT_WIN7
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <Shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <vector>

#include <intrin.h>
#pragma intrinsic(memset, memcpy,  strcat, strcmp, strcpy, strlen)

#include "Utility.h"

#define APP_VERSION "1.2.0"  

#define PROJECT_NAME "Foldrion"
#define INSTALL_FOLDER L"Foldrion"

#define REGISTRY_PATH "Directory\\shell\\Foldrion"
#define FOLDER_PROGID "Foldrion.CustomFolder"
#define FOLDER_PROGID_W L"Foldrion.CustomFolder"

#define COMMAND_ICON "--index="
#define COMMAND_RESOURCE "--resource="
#define COMMAND_RESOURCE_INDEX "--rindex="
#define COMMAND_FOLDER "--folder="

BOOL ImportCustomIconFiles(HWND owner, UINT* copiedCount, UINT* convertedCount, UINT* failedCount, std::vector<std::wstring>* failedFiles);
void RefreshInstalledShellMenu();