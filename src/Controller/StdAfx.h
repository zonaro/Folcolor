
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
#include <ctime>
#include <wininet.h>

#include <intrin.h>
#pragma intrinsic(memset, memcpy,  strcat, strcmp, strcpy, strlen)

#include "Utility.h"

constexpr int GetDayOfYear(const char* date) {
    // date is "Mmm dd yyyy"
    // Extract month and day
    int month = 0;
    int day = 0;
    if (date[0] == 'J' && date[1] == 'a' && date[2] == 'n') month = 1;
    else if (date[0] == 'F' && date[1] == 'e' && date[2] == 'b') month = 2;
    else if (date[0] == 'M' && date[1] == 'a' && date[2] == 'r') month = 3;
    else if (date[0] == 'A' && date[1] == 'p' && date[2] == 'r') month = 4;
    else if (date[0] == 'M' && date[1] == 'a' && date[2] == 'y') month = 5;
    else if (date[0] == 'J' && date[1] == 'u' && date[2] == 'n') month = 6;
    else if (date[0] == 'J' && date[1] == 'u' && date[2] == 'l') month = 7;
    else if (date[0] == 'A' && date[1] == 'u' && date[2] == 'g') month = 8;
    else if (date[0] == 'S' && date[1] == 'e' && date[2] == 'p') month = 9;
    else if (date[0] == 'O' && date[1] == 'c' && date[2] == 't') month = 10;
    else if (date[0] == 'N' && date[1] == 'o' && date[2] == 'v') month = 11;
    else if (date[0] == 'D' && date[1] == 'e' && date[2] == 'c') month = 12;
    
    // Day: positions 4-5 or 5-6
    if (date[4] == ' ') {
        day = date[5] - '0';
    } else {
        day = (date[4] - '0') * 10 + (date[5] - '0');
    }
    
    // Days in months (non-leap year, adjust if needed)
    int daysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dayOfYear = 0;
    for (int m = 1; m < month; ++m) {
        dayOfYear += daysInMonth[m];
    }
    dayOfYear += day;
    return dayOfYear;
}

inline std::string GetAppVersion() {
    const char* date = __DATE__;
    const char* time = __TIME__;
    
    // Year: last 2 digits
    int year = ((date[7] - '0') * 10 + (date[8] - '0')) % 100;
    
    // Day of year
    int dayOfYear = GetDayOfYear(date);
    
    // Hour and minute from __TIME__
    int hour = (time[0] - '0') * 10 + (time[1] - '0');
    int min = (time[3] - '0') * 10 + (time[4] - '0');
    
    char version[32];
    sprintf_s(version, sizeof(version), "v1.%02d.%03d.%02d%02d", year, dayOfYear, hour, min);
    return std::string(version);
}  

#define PROJECT_NAME "Foldrion"
#define INSTALL_FOLDER L"Foldrion"
#define SYSTEM_ICON_CACHE_FILE L"system-icon-cache.txt"

#define REGISTRY_PATH "Directory\\shell\\Foldrion"
#define FOLDER_PROGID "Foldrion.CustomFolder"
#define FOLDER_PROGID_W L"Foldrion.CustomFolder"

#define COMMAND_ICON "--index="
#define COMMAND_RESOURCE "--resource="
#define COMMAND_RESOURCE_INDEX "--rindex="
#define COMMAND_FOLDER "--folder="

BOOL ImportCustomIconFiles(HWND owner, UINT* copiedCount, UINT* convertedCount, UINT* failedCount, std::vector<std::wstring>* failedFiles);
void RefreshInstalledShellMenu();

typedef void (*InstallDiscoveryProgressCallback)(LPCWSTR foundLibraryPath, void* userData);
void SetInstallDiscoveryProgressCallback(InstallDiscoveryProgressCallback callback, void* userData);
void RebuildSystemIconCacheOnly();