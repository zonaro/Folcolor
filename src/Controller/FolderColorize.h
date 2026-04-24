
// Foldrion(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#pragma once

// Amount of embedded color icons available in each Windows icon set.
#define COLOR_ICON_COUNT 961

// Offsets into our embedded icon resource
#define WIN10_ICON_OFFSET 2								// Windows 10 set
#define WIN7_ICON_OFFSET (2 + COLOR_ICON_COUNT)			// Windows 7 & 8 set
#define WIN11_ICON_OFFSET (2 + (COLOR_ICON_COUNT * 2))	// Windows 11 set


void SetFolderColor(int index, LPWSTR folderPath);
void SetFolderIconResource(LPCWSTR iconResourcePath, int iconIndex, LPWSTR folderPath);
void ResetWindowsIconCache();