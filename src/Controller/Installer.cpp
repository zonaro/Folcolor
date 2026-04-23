// Folcolor(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"
#include "resource.h"
#include "FolderColorize.h"
#include <commdlg.h>
#include <string>
#include <vector>
#include <algorithm>
#include <wincodec.h>
#include <tlhelp32.h>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Userenv.lib")

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
L"\"%s\" --resource \"%s\" --rindex %d --folder \"%%1\"",
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
    LPCWSTR ext = PathFindExtensionW(filePath.c_str());
    if (ext && (_wcsicmp(ext, L".ico") == 0))
        return filePath;

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
 * Return the installation icons folder path.
 */
static std::wstring BuildIconsFolderPath()
{
return std::wstring(myPathGlobal) + L"icons";
}


/**
 * Return the file name portion of an absolute path.
 */
static std::wstring FileNameFromPath(const std::wstring& fullPath)
{
LPCWSTR fileName = PathFindFileNameW(fullPath.c_str());
return fileName ? std::wstring(fileName) : std::wstring();
}


/**
 * Return TRUE when the file extension is a convertible bitmap format.
 */
static BOOL IsConvertibleImageFile(const std::wstring& path)
{
return HasExt(path, L".png") || HasExt(path, L".jpg") || HasExt(path, L".jpeg");
}


/**
 * Build a destination path that does not collide with existing files.
 */
static std::wstring MakeUniqueDestinationPath(const std::wstring& dirPath, const std::wstring& fileName)
{
WCHAR baseName[MAX_PATH];
WCHAR extension[MAX_PATH];
_wsplitpath_s(fileName.c_str(), NULL, 0, NULL, 0, baseName, _countof(baseName), extension, _countof(extension));

std::wstring candidate = dirPath + L"\\" + fileName;
if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
return candidate;

for (UINT suffix = 1; suffix < 10000; suffix++)
{
WCHAR uniqueName[MAX_PATH];
if (_snwprintf_s(uniqueName, _countof(uniqueName), (_countof(uniqueName) - 1), L"%s (%u)%s", baseName, suffix, extension) < 1)
continue;

candidate = dirPath + L"\\" + uniqueName;
if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
return candidate;
}

return std::wstring();
}


/**
 * Release a COM interface pointer safely.
 */
template<typename T>
static void SafeRelease(T*& ptr)
{
if (ptr)
{
ptr->Release();
ptr = NULL;
}
}


#pragma pack(push, 1)
struct ICONDIRHEADER
{
WORD idReserved;
WORD idType;
WORD idCount;
};

struct ICONDIRENTRYFILE
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


/**
 * Copy a selected source into a padded square BGRA canvas suitable for an icon.
 */
static HRESULT LoadImageAsSquareBgra(LPCWSTR sourcePath, std::vector<BYTE>& pixels, UINT& canvasSize)
{
HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
BOOL shouldUninitialize = SUCCEEDED(hr);
if ((hr != S_OK) && (hr != S_FALSE) && (hr != RPC_E_CHANGED_MODE))
return hr;

IWICImagingFactory* factory = NULL;
IWICBitmapDecoder* decoder = NULL;
IWICBitmapFrameDecode* frame = NULL;
IWICBitmapSource* scaledSource = NULL;
IWICBitmapScaler* scaler = NULL;
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

UINT srcWidth = 0;
UINT srcHeight = 0;
hr = frame->GetSize(&srcWidth, &srcHeight);
if (FAILED(hr) || (srcWidth == 0) || (srcHeight == 0))
{
if (SUCCEEDED(hr))
hr = E_FAIL;
break;
}

UINT maxDim = (srcWidth > srcHeight) ? srcWidth : srcHeight;
canvasSize = (maxDim > 256) ? 256 : maxDim;
if (canvasSize == 0)
canvasSize = 1;

double scale = (maxDim > 256) ? (256.0 / double(maxDim)) : 1.0;
UINT drawWidth = (UINT) ((double(srcWidth) * scale) + 0.5);
UINT drawHeight = (UINT) ((double(srcHeight) * scale) + 0.5);
if (drawWidth == 0)
drawWidth = 1;
if (drawHeight == 0)
drawHeight = 1;

IWICBitmapSource* sourceForConvert = frame;
if ((drawWidth != srcWidth) || (drawHeight != srcHeight))
{
hr = factory->CreateBitmapScaler(&scaler);
if (FAILED(hr))
break;

hr = scaler->Initialize(frame, drawWidth, drawHeight, WICBitmapInterpolationModeFant);
if (FAILED(hr))
break;

scaledSource = scaler;
sourceForConvert = scaledSource;
}

hr = factory->CreateFormatConverter(&converter);
if (FAILED(hr))
break;

hr = converter->Initialize(sourceForConvert, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
if (FAILED(hr))
break;

UINT drawStride = drawWidth * 4;
std::vector<BYTE> drawPixels(drawStride * drawHeight);
hr = converter->CopyPixels(NULL, drawStride, (UINT) drawPixels.size(), drawPixels.data());
if (FAILED(hr))
break;

UINT canvasStride = canvasSize * 4;
pixels.assign(canvasStride * canvasSize, 0);
UINT offsetX = (canvasSize - drawWidth) / 2;
UINT offsetY = (canvasSize - drawHeight) / 2;

for (UINT y = 0; y < drawHeight; y++)
memcpy(&pixels[((y + offsetY) * canvasStride) + (offsetX * 4)], &drawPixels[y * drawStride], drawStride);
}
while (FALSE);

SafeRelease(converter);
SafeRelease(scaler);
SafeRelease(scaledSource);
SafeRelease(frame);
SafeRelease(decoder);
SafeRelease(factory);

if (shouldUninitialize)
CoUninitialize();

return hr;
}


/**
 * Encode a BGRA square bitmap into a PNG byte buffer using WIC.
 */
static HRESULT EncodeSquarePng(const std::vector<BYTE>& pixels, UINT canvasSize, std::vector<BYTE>& pngData)
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

SafeRelease(props);
SafeRelease(frame);
SafeRelease(encoder);
SafeRelease(stream);
SafeRelease(bitmap);
SafeRelease(factory);

if (shouldUninitialize)
CoUninitialize();

return hr;
}


/**
 * Write a single-image ICO file that stores a PNG payload.
 */
static HRESULT WritePngIcoFile(LPCWSTR targetPath, const std::vector<BYTE>& pngData, UINT canvasSize)
{
ICONDIRHEADER header = { 0, 1, 1 };
ICONDIRENTRYFILE entry = {};
entry.bWidth = (canvasSize >= 256) ? 0 : (BYTE) canvasSize;
entry.bHeight = (canvasSize >= 256) ? 0 : (BYTE) canvasSize;
entry.bColorCount = 0;
entry.bReserved = 0;
entry.wPlanes = 1;
entry.wBitCount = 32;
entry.dwBytesInRes = (DWORD) pngData.size();
entry.dwImageOffset = sizeof(header) + sizeof(entry);

HANDLE file = CreateFileW(targetPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
if (file == INVALID_HANDLE_VALUE)
return HRESULT_FROM_WIN32(GetLastError());

HRESULT hr = S_OK;
DWORD written = 0;
if (!WriteFile(file, &header, sizeof(header), &written, NULL) || (written != sizeof(header)))
hr = HRESULT_FROM_WIN32(GetLastError());
else if (!WriteFile(file, &entry, sizeof(entry), &written, NULL) || (written != sizeof(entry)))
hr = HRESULT_FROM_WIN32(GetLastError());
else if (!WriteFile(file, pngData.data(), (DWORD) pngData.size(), &written, NULL) || (written != pngData.size()))
hr = HRESULT_FROM_WIN32(GetLastError());

CloseHandle(file);

if (FAILED(hr))
DeleteFileW(targetPath);

return hr;
}


/**
 * Convert a supported image file to an ICO file using a PNG-backed icon entry.
 */
static HRESULT ConvertImageFileToIcoFile(LPCWSTR sourcePath, LPCWSTR targetPath)
{
std::vector<BYTE> pixels;
std::vector<BYTE> pngData;
UINT canvasSize = 0;

HRESULT hr = LoadImageAsSquareBgra(sourcePath, pixels, canvasSize);
if (FAILED(hr))
return hr;

hr = EncodeSquarePng(pixels, canvasSize, pngData);
if (FAILED(hr))
return hr;

return WritePngIcoFile(targetPath, pngData, canvasSize);
}


/**
 * Ask the user for one or more source files to import.
 */
static BOOL SelectImportFiles(HWND owner, std::vector<std::wstring>& selectedFiles)
{
std::vector<WCHAR> buffer(65536, 0);
OPENFILENAMEW ofn = {};
static const WCHAR filter[] =
L"Supported files (*.ico;*.dll;*.jpg;*.jpeg;*.png)\0*.ico;*.dll;*.jpg;*.jpeg;*.png\0"
L"Icon files (*.ico)\0*.ico\0"
L"Dynamic libraries (*.dll)\0*.dll\0"
L"Images (*.jpg;*.jpeg;*.png)\0*.jpg;*.jpeg;*.png\0"
L"All files (*.*)\0*.*\0\0";

ofn.lStructSize = sizeof(ofn);
ofn.hwndOwner = owner;
ofn.lpstrFilter = filter;
ofn.lpstrFile = buffer.data();
ofn.nMaxFile = (DWORD) buffer.size();
ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT;
ofn.lpstrTitle = L"Import Custom Icons";

if (!GetOpenFileNameW(&ofn))
return FALSE;

LPCWSTR first = buffer.data();
if (!first[0])
return FALSE;

LPCWSTR second = first + wcslen(first) + 1;
if (!second[0])
{
selectedFiles.push_back(first);
return TRUE;
}

std::wstring folder = first;
while (second[0])
{
selectedFiles.push_back(folder + L"\\" + second);
second += wcslen(second) + 1;
}

return !selectedFiles.empty();
}


/**
 * Copy or convert selected files into the installation icons folder.
 */
BOOL ImportCustomIconFiles(HWND owner, UINT* copiedCount, UINT* convertedCount, UINT* failedCount, std::vector<std::wstring>* failedFiles)
{
if (copiedCount)
*copiedCount = 0;
if (convertedCount)
*convertedCount = 0;
if (failedCount)
*failedCount = 0;
if (failedFiles)
failedFiles->clear();

std::vector<std::wstring> selectedFiles;
if (!SelectImportFiles(owner, selectedFiles))
return FALSE;

	if (!CreateDirectoryW(myPathGlobal, NULL))
	{
		DWORD gle = GetLastError();
		if (gle != ERROR_ALREADY_EXISTS)
		{
			MessageBoxA(owner, "Unable to create install folder.", "Error:", (MB_OK | MB_ICONERROR));
			return FALSE;
		}
	}

std::wstring iconsPath = BuildIconsFolderPath();
if (!CreateDirectoryW(iconsPath.c_str(), NULL))
{
DWORD gle = GetLastError();
if (gle != ERROR_ALREADY_EXISTS)
{
MessageBoxA(owner, "Unable to create icons folder.", "Error:", (MB_OK | MB_ICONERROR));
return FALSE;
}
}

UINT copied = 0;
UINT converted = 0;
UINT failed = 0;

for (size_t i = 0; i < selectedFiles.size(); i++)
{
const std::wstring& sourcePath = selectedFiles[i];
std::wstring fileName = FileNameFromPath(sourcePath);
if (fileName.empty())
{
			if (failedFiles)
				failedFiles->push_back(sourcePath);
failed++;
continue;
}

std::wstring destinationName = fileName;
if (IsConvertibleImageFile(sourcePath))
destinationName = FileStem(fileName) + L".ico";

std::wstring destinationPath = MakeUniqueDestinationPath(iconsPath, destinationName);
if (destinationPath.empty())
{
			if (failedFiles)
				failedFiles->push_back(fileName);
failed++;
continue;
}

if (HasExt(sourcePath, L".ico") || HasExt(sourcePath, L".dll"))
{
if (CopyFileW(sourcePath.c_str(), destinationPath.c_str(), TRUE))
copied++;
else
			{
				if (failedFiles)
					failedFiles->push_back(fileName);
failed++;
			}
}
else if (IsConvertibleImageFile(sourcePath))
{
if (SUCCEEDED(ConvertImageFileToIcoFile(sourcePath.c_str(), destinationPath.c_str())))
converted++;
else
			{
				if (failedFiles)
					failedFiles->push_back(fileName);
failed++;
			}
}
else
		{
			if (failedFiles)
				failedFiles->push_back(fileName);
failed++;
		}
}

if (copiedCount)
*copiedCount = copied;
if (convertedCount)
*convertedCount = converted;
if (failedCount)
*failedCount = failed;

return TRUE;
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
std::wstring command = BuildResourceCommand(exePath, full, 0);
    AddCommandItem(parentShellKey, order, label, L"", command, FALSE);
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


/**
 * Expand an environment-variable path (e.g. %SystemRoot%\\...).
 * Returns an empty string on failure.
 */
static std::wstring ExpandEnvPath(LPCWSTR envPath)
{
    WCHAR expanded[MAX_PATH * 2];
    DWORD r = ExpandEnvironmentStringsW(envPath, expanded, _countof(expanded));
    if ((r == 0) || (r > _countof(expanded)))
        return std::wstring();
    return std::wstring(expanded);
}


/**
 * Add a DLL/EXE resource submenu under parentShellKey, splitting icons into
 * pages of SYSTEM_DLL_PAGE_SIZE to stay within Explorer's menu-item limits.
 * Returns TRUE when at least one icon entry was written.
 */
#define SYSTEM_DLL_PAGE_SIZE 40u

static BOOL WriteSystemDllSubmenu(HKEY parentShellKey, UINT& order,
                                  const std::wstring& exePath,
                                  LPCWSTR envPath, LPCWSTR label)
{
    std::wstring filePath = ExpandEnvPath(envPath);
    if (filePath.empty())
        return FALSE;

    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return FALSE;

    UINT iconCount = ExtractIconExW(filePath.c_str(), -1, NULL, NULL, 0);
    if ((iconCount == UINT_MAX) || (iconCount == 0))
        return FALSE;

    /* Keep the same submenu creation pattern used by "Colors". */
    HKEY dllShell = AddSubmenu(parentShellKey, order, label, BuildIconSpec(filePath, 0));

    UINT pageCount = (iconCount + SYSTEM_DLL_PAGE_SIZE - 1) / SYSTEM_DLL_PAGE_SIZE;

    if (pageCount <= 1)
    {
        /* All icons fit in a single page — write directly. */
        UINT dllOrder = 0;
        for (UINT idx = 0; idx < iconCount; idx++)
        {
            WCHAR iconLabel[64];
            swprintf_s(iconLabel, _countof(iconLabel), L"Icon %03u", idx);
            AddCommandItem(dllShell, dllOrder,
                           iconLabel,
                           BuildIconSpec(filePath, (int) idx),
                           BuildResourceCommand(exePath, filePath, (int) idx),
                           FALSE);
        }
    }
    else
    {
        /* Split into pages so no submenu exceeds SYSTEM_DLL_PAGE_SIZE items. */
        UINT pageOrder = 0;
        for (UINT page = 0; page < pageCount; page++)
        {
            UINT firstIdx = page * SYSTEM_DLL_PAGE_SIZE;
            UINT lastIdx  = min(firstIdx + SYSTEM_DLL_PAGE_SIZE, iconCount) - 1;

            WCHAR pageLabel[64];
            swprintf_s(pageLabel, _countof(pageLabel), L"%03u-%03u", firstIdx, lastIdx);

            HKEY pageShell = AddSubmenu(dllShell, pageOrder,
                                        pageLabel,
                                        BuildIconSpec(filePath, (int) firstIdx));
            UINT itemOrder = 0;
            for (UINT idx = firstIdx; idx <= lastIdx; idx++)
            {
                WCHAR iconLabel[64];
                swprintf_s(iconLabel, _countof(iconLabel), L"Icon %03u", idx);
                AddCommandItem(pageShell, itemOrder,
                               iconLabel,
                               BuildIconSpec(filePath, (int) idx),
                               BuildResourceCommand(exePath, filePath, (int) idx),
                               FALSE);
            }
            RegCloseKey(pageShell);
        }
    }

    RegCloseKey(dllShell);
    return TRUE;
}


/**
 * Write the "System" submenu with one child submenu per common Windows
 * resource DLL/EXE.  Placed immediately after the "Colors" submenu.
 */
static void WriteSystemIconsMenu(HKEY shellKey, UINT& order, const std::wstring& exePath)
{
    struct SysEntry { LPCWSTR envPath; LPCWSTR label; };
    static const SysEntry sysEntries[] =
    {
        { L"%SystemRoot%\\System32\\imageres.dll",         L"Windows Icon Library (imageres.dll)"         },
        { L"%SystemRoot%\\System32\\shell32.dll",          L"Windows Shell Core (shell32.dll)"            },
        { L"%SystemRoot%\\System32\\pifmgr.dll",           L"Legacy Program Icons (pifmgr.dll)"           },
        { L"%SystemRoot%\\System32\\ddores.dll",           L"Device Category Resources (ddores.dll)"      },
        { L"%SystemRoot%\\System32\\accessibilitycpl.dll", L"Ease of Access (accessibilitycpl.dll)"       },
        { L"%SystemRoot%\\System32\\mmres.dll",            L"Audio Resources (mmres.dll)"                 },
        { L"%SystemRoot%\\System32\\netshell.dll",         L"Network Connections (netshell.dll)"          },
        { L"%SystemRoot%\\explorer.exe",                     L"File Explorer Shell (explorer.exe)"          },
        { L"%SystemRoot%\\System32\\wmploc.DLL",           L"Media Player Resources (wmploc.dll)"         },
    };

    /* Use imageres icon 109 (settings/tools) as the System submenu icon. */
    std::wstring sysIcon = ExpandEnvPath(L"%SystemRoot%\\System32\\imageres.dll");
    if (!sysIcon.empty())
        sysIcon = BuildIconSpec(sysIcon, 109);

    HKEY sysShell = AddSubmenu(shellKey, order, L"System", sysIcon);
    UINT sysOrder = 0;
    for (UINT i = 0; i < _countof(sysEntries); i++)
        WriteSystemDllSubmenu(sysShell, sysOrder, exePath, sysEntries[i].envPath, sysEntries[i].label);
    RegCloseKey(sysShell);
}


// Write our shell registry
static void InstallRegistry()
{
// Delete our existing key if it's already there
if (!DeleteRegistryPath(HKEY_CLASSES_ROOT, REGISTRY_PATH))
CRITICAL_API_FAIL(DeleteRegistryPath, GetLastError());

// Root: HKEY_CLASSES_ROOT\Directory\shell\Folcolor
HKEY rootKey = NULL;
LSTATUS lStatus = RegCreateKeyExA(HKEY_CLASSES_ROOT, REGISTRY_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &rootKey, NULL);
if (lStatus != ERROR_SUCCESS)
CRITICAL_API_FAIL(RegCreateKeyExA, lStatus);

std::wstring exePath = BuildInstalledExePath();

// Root command entry: open picker window directly.
WriteRegSzWOrFail(rootKey, L"MUIVerb", L"Color Folder");
WriteRegSzWOrFail(rootKey, L"Icon", exePath);

HKEY commandKey = CreateSubKeyWOrFail(rootKey, L"command");
WCHAR cmd[2048];
if (_snwprintf_s(cmd, _countof(cmd), (_countof(cmd) - 1),
L"\"%s\" --pick --folder \"%%1\"", exePath.c_str()) < 1)
CRITICAL("Path size limit error!");
WriteRegSzWOrFail(commandKey, NULL, cmd);
RegCloseKey(commandKey);

RegCloseKey(rootKey);
}


/**
 * Rebuild shell registry entries and notify shell changes without restarting Explorer.
 */
void RefreshInstalledShellMenu()
{
    InstallRegistry();

    std::wstring iconsPath = BuildIconsFolderPath();
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, iconsPath.c_str(), NULL);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, myPathGlobal, NULL);

    std::wstring exePath = BuildInstalledExePath();
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, exePath.c_str(), NULL);

    ResetWindowsIconCache();
}

// Install ourself
void Install()
{
// Create installation folder
if (!CreateDirectoryW(myPathGlobal, NULL))
{
DWORD gle = GetLastError();
if (gle != ERROR_ALREADY_EXISTS)
CRITICAL_API_FAIL(CreateDirectoryW, gle);
}

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

RefreshInstalledShellMenu();
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
