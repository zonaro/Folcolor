
// Foldrion(tm) (c) 2020 Kevin Weatherman
// MIT license https://opensource.org/licenses/MIT
#include "StdAfx.h"


// Formated output to OutputDebugStringA() for development
void trace(LPCSTR pszFormat, ...)
{
	va_list vl;
	char szBuffer[2048];
	va_start(vl, pszFormat);
	vsprintf_s(szBuffer, (sizeof(szBuffer) - 1), pszFormat, vl);
	szBuffer[sizeof(szBuffer) - 1] = 0;
	va_end(vl);
	OutputDebugStringA(szBuffer);
}

// ------------------------------------------------------------------------------------------------

// Get an error string for a GetLastError() code
LPSTR GetErrorString(DWORD error, __out_bcount_z(1024) LPSTR buffer)
{
	if (!FormatMessageA((FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS),
		NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		buffer, 1024, NULL))
	{
		strncpy_s(buffer, 1024, "Unknown", (1024 - 1));
	}
	else
	{
		// Remove the trailing '\r'
		if (LPSTR lineFeed = strstr(buffer, "\r"))
			*lineFeed = 0;
	}
	return buffer;
}


/* 
Critical fail abort application:
Unlike the common design pattern of error handling that attempts to clean itself up to exit by collapsing 
(hopefully) backward on the call stack. 
Here we take intentionally take advantage of the fact that Windows OS user mode app virtualization frees 
memory, handles, etc., on exit for us.
Intended for critical/fatal error handling we must exist anyhow.

Combined with helper macros, this idiom encourages the covering of more, if not all, of the bad API return 
cases vs the coding exasperation and confusion of the more common pattern et al.
*/
void CriticalErrorAbort(int line, __in LPCSTR file, __in LPCSTR reason)
{
	if (reason)
	{
		if (!file)
			file = "???";

		char buffer[2048];
		_snprintf_s(buffer, sizeof(buffer), "CRITICAL ERROR: \"%s\", File: \"%s\", line: #%d **\n", reason, file, line);
		MessageBoxA(NULL, buffer, PROJECT_NAME ": CRITICAL ERROR!", (MB_ICONSTOP | MB_OK));
	}
	else
		MessageBoxA(NULL, "Unknown error!", PROJECT_NAME ": CRITICAL ERROR!", (MB_ICONSTOP | MB_OK));

	exit(EXIT_FAILURE);
}

// ------------------------------------------------------------------------------------------------

// Force a window into focus
void ForceWindowFocus(HWND hWnd)
{
	SwitchToThisWindow(hWnd, TRUE);
	BringWindowToTop(hWnd);
	SetForegroundWindow(hWnd);
}

// Get a PID's first related HWND if it has one.
// Note: For the simple case used here, any given process could potentially have more than one associated HWND
HWND GetHwndForPid(UINT pid)
{
	HWND hwndNext = FindWindowEx(NULL, NULL, NULL, NULL);
	while (hwndNext)
	{
		DWORD pid2;
		GetWindowThreadProcessId(hwndNext, &pid2);
		if (pid == pid2)
			return hwndNext;
		else
			hwndNext = FindWindowEx(NULL, hwndNext, NULL, NULL);
	};
	return NULL;
}

// ------------------------------------------------------------------------------------------------


// Run a command line process, waiting for it to complete, and returning the error code
DWORD ShellCommand(__in LPWSTR cmdLine, BOOL invisible)
{
	DWORD exitCode = -1;

	PROCESS_INFORMATION pi = {};
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	if (invisible)
	{
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
	}

	// Create child process
	// Note command line needs to RW per MSDN
	if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		CRITICAL_API_FAIL(CreateProcessW, GetLastError());

	// Wait until processes exits	
	DWORD status = WaitForSingleObject(pi.hProcess, (8 * 1000));
	if (status != WAIT_OBJECT_0)
		CRITICAL_API_FAIL(WaitForSingleObject, (status == WAIT_FAILED) ? GetLastError() : HRESULT_FROM_WIN32(status));

	// Get process return code	
	GetExitCodeProcess(pi.hProcess, &exitCode);	

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);	

	return exitCode;
}



// Get a 32bit file size by handle
long fsize(FILE *fp)
{
	long psave, endpos;
	long result = -1;

	if ((psave = ftell(fp)) != -1L)
	{
		if (fseek(fp, 0, SEEK_END) == 0)
		{
			if ((endpos = ftell(fp)) != -1L)
			{
				fseek(fp, psave, SEEK_SET);
				result = endpos;
			}
		}
	}

	return(result);
}

// ------------------------------------------------------------------------------------------------

BOOL DeleteRegistryPath(__in HKEY hKeyRoot, __in LPCSTR subKey)
{
	LSTATUS lStatus = RegDeleteTreeA(hKeyRoot, subKey);
	if ((lStatus != ERROR_SUCCESS) && (lStatus != ERROR_FILE_NOT_FOUND))
	{
		SetLastError(lStatus);
		return FALSE;
	}

	lStatus = RegDeleteKeyA(hKeyRoot, subKey);
	if ((lStatus == ERROR_SUCCESS) || (lStatus == ERROR_FILE_NOT_FOUND))
		return TRUE;

	SetLastError(lStatus);
	return FALSE;
}
