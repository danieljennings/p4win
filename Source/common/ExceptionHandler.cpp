// ExceptionHandler.cpp  Version 1.4
//
// Copyright � 1998 Bruce Dawson
//
// This source file contains the exception handler for recording error
// information after crashes. See ExceptionHandler.h for information
// on how to hook it in.
//
// Author:       Bruce Dawson
//               brucedawson@cygnus-software.com
//
// Modified by:  Hans Dietrich
//               hdietrich2@hotmail.com
//
// Version 1.4:  - Added invocation of XCrashReport.exe
//
// Version 1.3:  - Added minidump output
//
// Version 1.1:  - reformatted output for XP-like error report
//               - added ascii output to stack dump
//
// A paper by the original author can be found at:
//     http://www.cygnus-software.com/papers/release_debugging.html
//
///////////////////////////////////////////////////////////////////////////////

// This is derived from code by Hans Deitrich, found at:
//   http://www.codeproject.com/debug/XCrashReportPt4.asp
// Modifications for P4Win:
//   adapted to build with UNICODE
//   tries first to load dbghelp.dll from executable directory
//     this allows distributing an updated version of this dll
//   places files in new folder alongside app
//     new folder is called "error_reports"
//     under that folder, a new folder is created for each error report
//     foldername is concatenation of appname and date
//   added registry saving here, adapted from XCrashReport

#include "stdafx.h"

// Disable warnings generated by the Windows header files.
#pragma warning(disable : 4514)
#pragma warning(disable : 4201)

#define _WIN32_WINDOWS 0x0500	// for IsDebuggerPresent

// does not require MFC;  use 'Not using precompiled headers'

#include <windows.h>
#include <tchar.h>
#include <dbghelp.h>

#include "GetWinVer.h"
#include "MiniVersion.h"
#include "CrashFileNames.h"
#include "WriteRegistry.h"

#ifndef _countof
#define _countof(array) (sizeof(array)/sizeof(array[0]))
#endif

const int NumCodeBytes = 16;	// Number of code bytes to record.
const int MaxStackDump = 3072;	// Maximum number of DWORDS in stack dumps.
const int StackColumns = 4;		// Number of columns in stack dump.

#define	ONEK			1024
#define	SIXTYFOURK		(64*ONEK)
#define	ONEM			(ONEK*ONEK)
#define	ONEG			(ONEK*ONEK*ONEK)

#if defined(_M_IX86)
# define IMAGE_FILE_ARCH IMAGE_FILE_MACHINE_I386
# define AXREG Eax
# define BXREG Ebx
# define CXREG Ecx
# define DXREG Edx
# define SIREG Esi
# define DIREG Edi
# define BPREG Ebp
# define IPREG Eip
# define SPREG Esp
#elif defined(_M_X64)
# define IMAGE_FILE_ARCH IMAGE_FILE_MACHINE_AMD64
# define AXREG Rax
# define BXREG Rbx
# define CXREG Rcx
# define DXREG Rdx
# define SIREG Rsi
# define DIREG Rdi
# define BPREG Rbp
# define IPREG Rip
# define SPREG Rsp
#else
# error Machine type not supported
#endif // _M_IX86

static bool allowER = false;

void EnableErrorRecording(bool allow)
{
	allowER = allow;
}

///////////////////////////////////////////////////////////////////////////////
// lstrrchr (avoid the C Runtime )
static TCHAR * lstrrchr(LPCTSTR string, int ch)
{
	TCHAR *start = (TCHAR *)string;

	while (*string++)                       /* find end of string */
		;
											/* search towards front */
	while (--string != start && *string != (TCHAR) ch)
		;

	if (*string == (TCHAR) ch)                /* char found ? */
		return (TCHAR *)string;

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// hprintf behaves similarly to printf, with a few vital differences.
// It uses wvsprintf to do the formatting, which is a system routine,
// thus avoiding C run time interactions. For similar reasons it
// uses WriteFile rather than fwrite.
// The one limitation that this imposes is that wvsprintf, and
// therefore hprintf, cannot handle floating point numbers.

// Too many calls to WriteFile can take a long time, causing
// confusing delays when programs crash. Therefore I implemented
// a simple buffering scheme for hprintf

#define HPRINTF_BUFFER_SIZE (8*1024)				// must be at least 2048
static TCHAR hprintf_buffer[HPRINTF_BUFFER_SIZE];	// wvsprintf never prints more than one K.
#ifdef UNICODE
static char hprintf_bufferA[HPRINTF_BUFFER_SIZE*2];
#endif
static int  hprintf_index = 0;

///////////////////////////////////////////////////////////////////////////////
// hflush
static void hflush(HANDLE LogFile, int size = 0)
{
	if (hprintf_index > size)
	{
		DWORD NumBytes;
#ifdef UNICODE
		NumBytes = WideCharToMultiByte(CP_ACP, 0, hprintf_buffer, lstrlen(hprintf_buffer), hprintf_bufferA, sizeof(hprintf_bufferA), NULL, NULL);
		if(NumBytes)
			WriteFile(LogFile, hprintf_bufferA, NumBytes - 1, &NumBytes, 0);
#else
		WriteFile(LogFile, hprintf_buffer, lstrlen(hprintf_buffer), &NumBytes, 0);
#endif
		hprintf_index = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
// hprintf
static void hprintf(HANDLE LogFile, LPCTSTR Format, ...)
{
	hflush(LogFile, (HPRINTF_BUFFER_SIZE-1024));

	va_list arglist;
	va_start( arglist, Format);
	hprintf_index += wvsprintf(&hprintf_buffer[hprintf_index], Format, arglist);
	va_end( arglist);
}

typedef BOOL (WINAPI *MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
									CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
									CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
									CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
									);

static LPCTSTR dbgHelpDll = _T("DBGHELP.DLL");

static HMODULE LoadDbgHelpDll()
{
	// try to load dbghelp.dll.  Just using LoadLibrary will try to match to
	// a loaded module.  This might get us an old one, like if running on
	// Win2k, which we have replaced when we installed.  So first, look in 
	// the same dir as this .exe.
	HMODULE hDll = NULL;
	TCHAR path[_MAX_PATH];
	if (GetModuleFileName( NULL, path, _MAX_PATH ))
	{
		TCHAR *sep = lstrrchr( path, '\\' );
		if (sep)
		{
			lstrcpy( sep+1, dbgHelpDll );
			hDll = ::LoadLibrary( path );
		}
	}

	if (hDll==NULL)
	{
		// couldn't load one we installed, so take what we can get
		// and hope for the best
		hDll = ::LoadLibrary( dbgHelpDll );
	}
	return hDll;
}

///////////////////////////////////////////////////////////////////////////////
// DumpMiniDump
static void DumpMiniDump(HANDLE hFile, PEXCEPTION_POINTERS excpInfo)
{
	if (excpInfo == NULL) 
	{
		// Generate exception to get proper context in dump
		__try 
		{
			OutputDebugString(_T("raising exception\r\n"));
			RaiseException(EXCEPTION_BREAKPOINT, 0, 0, NULL);
		} 
		__except(DumpMiniDump(hFile, GetExceptionInformation()),
				 EXCEPTION_CONTINUE_EXECUTION) 
		{
		}
	} 
	else
	{
		OutputDebugString(_T("writing minidump\r\n"));
		MINIDUMP_EXCEPTION_INFORMATION eInfo;
		eInfo.ThreadId = GetCurrentThreadId();
		eInfo.ExceptionPointers = excpInfo;
		eInfo.ClientPointers = FALSE;

		HMODULE hDll = LoadDbgHelpDll();
		if(!hDll)
		{
			OutputDebugString(_T("dbghelp.dll not found\r\n"));
			return;
		}
		MINIDUMPWRITEDUMP pDump = (MINIDUMPWRITEDUMP)::GetProcAddress( hDll, "MiniDumpWriteDump" );
		if (!pDump)
		{
			OutputDebugString(_T("dbghelp.dll too old\r\n"));
			return;
		}
		// note:  MiniDumpWithIndirectlyReferencedMemory does not work on Win98
		pDump(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			MiniDumpNormal,
			excpInfo ? &eInfo : NULL,
			NULL,
			NULL);
	}
}

///////////////////////////////////////////////////////////////////////////////
// FormatTime
//
// Format the specified FILETIME to output in a human readable format,
// without using the C run time.
static void FormatTime(LPTSTR output, FILETIME TimeToPrint)
{
	output[0] = _T('\0');
	WORD Date, Time;
	if (FileTimeToLocalFileTime(&TimeToPrint, &TimeToPrint) &&
				FileTimeToDosDateTime(&TimeToPrint, &Date, &Time))
	{
		wsprintf(output, _T("%d/%d/%d %02d:%02d:%02d"),
					(Date / 32) & 15, Date & 31, (Date / 512) + 1980,
					(Time >> 11), (Time >> 5) & 0x3F, (Time & 0x1F) * 2);
	}
}

///////////////////////////////////////////////////////////////////////////////
// DumpModuleInfo
//
// Print information about a code module (DLL or EXE) such as its size,
// location, time stamp, etc.
static bool DumpModuleInfo(HANDLE LogFile, HINSTANCE ModuleHandle, int nModuleNo)
{
	bool rc = false;
	TCHAR szModName[MAX_PATH*2];
	ZeroMemory(szModName, sizeof(szModName));

	__try
	{
		if (GetModuleFileName(ModuleHandle, szModName, sizeof(szModName)-2) > 0)
		{
			// If GetModuleFileName returns greater than zero then this must
			// be a valid code module address. Therefore we can try to walk
			// our way through its structures to find the link time stamp.
			IMAGE_DOS_HEADER *DosHeader = (IMAGE_DOS_HEADER*)ModuleHandle;
		    if (IMAGE_DOS_SIGNATURE != DosHeader->e_magic)
	    	    return false;

			IMAGE_NT_HEADERS *NTHeader = (IMAGE_NT_HEADERS*)((TCHAR *)DosHeader
						+ DosHeader->e_lfanew);
		    if (IMAGE_NT_SIGNATURE != NTHeader->Signature)
	    	    return false;

			// open the code module file so that we can get its file date and size
			HANDLE ModuleFile = CreateFile(szModName, GENERIC_READ,
						FILE_SHARE_READ, 0, OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL, 0);

			TCHAR TimeBuffer[100];
			TimeBuffer[0] = _T('\0');
			
			DWORD FileSize = 0;
			if (ModuleFile != INVALID_HANDLE_VALUE)
			{
				FileSize = GetFileSize(ModuleFile, 0);
				FILETIME LastWriteTime;
				if (GetFileTime(ModuleFile, 0, 0, &LastWriteTime))
				{
					FormatTime(TimeBuffer, LastWriteTime);
				}
				CloseHandle(ModuleFile);
			}
			hprintf(LogFile, _T("Module %d\r\n"), nModuleNo);
			hprintf(LogFile, _T("%s\r\n"), szModName);
			hprintf(LogFile, _T("Image Base: 0x%08x  Image Size: 0x%08x\r\n"), 
				NTHeader->OptionalHeader.ImageBase, 
				NTHeader->OptionalHeader.SizeOfImage), 

			hprintf(LogFile, _T("Checksum:   0x%08x  Time Stamp: 0x%08x\r\n"), 
				NTHeader->OptionalHeader.CheckSum,
				NTHeader->FileHeader.TimeDateStamp);

			hprintf(LogFile, _T("File Size:  %-10d  File Time:  %s\r\n"),
						FileSize, TimeBuffer);

			hprintf(LogFile, _T("Version Information:\r\n"));

			CMiniVersion ver(szModName);
			TCHAR szBuf[200];
			WORD dwBuf[4];

			ver.GetCompanyName(szBuf, sizeof(szBuf)-1);
			hprintf(LogFile, _T("   Company:    %s\r\n"), szBuf);

			ver.GetProductName(szBuf, sizeof(szBuf)-1);
			hprintf(LogFile, _T("   Product:    %s\r\n"), szBuf);

			ver.GetFileDescription(szBuf, sizeof(szBuf)-1);
			hprintf(LogFile, _T("   FileDesc:   %s\r\n"), szBuf);

			ver.GetFileVersion(dwBuf);
			hprintf(LogFile, _T("   FileVer:    %d.%d.%d.%d\r\n"), 
				dwBuf[0], dwBuf[1], dwBuf[2], dwBuf[3]);

			ver.GetProductVersion(dwBuf);
			hprintf(LogFile, _T("   ProdVer:    %d.%d.%d.%d\r\n"), 
				dwBuf[0], dwBuf[1], dwBuf[2], dwBuf[3]);

			ver.Release();

			hprintf(LogFile, _T("\r\n"));

			rc = true;
		}
	}
	// Handle any exceptions by continuing from this point.
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return rc;
}

///////////////////////////////////////////////////////////////////////////////
// DumpModuleList
//
// Scan memory looking for code modules (DLLs or EXEs). VirtualQuery is used
// to find all the blocks of address space that were reserved or committed,
// and ShowModuleInfo will display module information if they are code
// modules.
static void DumpModuleList(HANDLE LogFile)
{
	SYSTEM_INFO	SystemInfo;
	GetSystemInfo(&SystemInfo);

	const size_t PageSize = SystemInfo.dwPageSize;

	// Set NumPages to the number of pages in the 4GByte address space,
	// while being careful to avoid overflowing ints
	const size_t NumPages = 4 * size_t(ONEG / PageSize);
	size_t pageNum = 0;
	void *LastAllocationBase = 0;

	int nModuleNo = 1;

	while (pageNum < NumPages)
	{
		MEMORY_BASIC_INFORMATION MemInfo;
		if (VirtualQuery((void *)(pageNum * PageSize), &MemInfo,
					sizeof(MemInfo)))
		{
			if (MemInfo.RegionSize > 0)
			{
				// Adjust the page number to skip over this block of memory
				pageNum += MemInfo.RegionSize / PageSize;
				if (MemInfo.State == MEM_COMMIT && MemInfo.AllocationBase >
							LastAllocationBase)
				{
					// Look for new blocks of committed memory, and try
					// recording their module names - this will fail
					// gracefully if they aren't code modules
					LastAllocationBase = MemInfo.AllocationBase;
					if (DumpModuleInfo(LogFile, 
									   (HINSTANCE)LastAllocationBase, 
									   nModuleNo))
					{
						nModuleNo++;
					}
				}
			}
			else
				pageNum += SIXTYFOURK / PageSize;
		}
		else
			pageNum += SIXTYFOURK / PageSize;

		// If VirtualQuery fails we advance by 64K because that is the
		// granularity of address space doled out by VirtualAlloc()
	}
}

///////////////////////////////////////////////////////////////////////////////
// DumpSystemInformation
//
// Record information about the user's system, such as processor type, amount
// of memory, etc.
static void DumpSystemInformation(HANDLE LogFile)
{
	FILETIME CurrentTime;
	GetSystemTimeAsFileTime(&CurrentTime);
	TCHAR szTimeBuffer[100];
	FormatTime(szTimeBuffer, CurrentTime);

	hprintf(LogFile, _T("Error occurred at %s.\r\n"), szTimeBuffer);

	TCHAR szModuleName[MAX_PATH*2];
	ZeroMemory(szModuleName, sizeof(szModuleName));
	if (GetModuleFileName(0, szModuleName, _countof(szModuleName)-2) <= 0)
		lstrcpy(szModuleName, _T("Unknown"));

	TCHAR szUserName[200];
	ZeroMemory(szUserName, sizeof(szUserName));
	DWORD UserNameSize = _countof(szUserName)-2;
	if (!GetUserName(szUserName, &UserNameSize))
		lstrcpy(szUserName, _T("Unknown"));

	hprintf(LogFile, _T("%s, run by %s.\r\n"), szModuleName, szUserName);

	// print out operating system
	TCHAR szWinVer[50], szMajorMinorBuild[50];
	int nWinVer;
	GetWinVer(szWinVer, &nWinVer, szMajorMinorBuild);
	hprintf(LogFile, _T("Operating system:  %s (%s).\r\n"), 
		szWinVer, szMajorMinorBuild);

	SYSTEM_INFO	SystemInfo;
	GetSystemInfo(&SystemInfo);
	hprintf(LogFile, _T("%d processor(s), type %d.\r\n"),
				SystemInfo.dwNumberOfProcessors, SystemInfo.dwProcessorType);

	MEMORYSTATUS MemInfo;
	MemInfo.dwLength = sizeof(MemInfo);
	GlobalMemoryStatus(&MemInfo);

	// Print out info on memory, rounded up.
	hprintf(LogFile, _T("%d%% memory in use.\r\n"), MemInfo.dwMemoryLoad);
	hprintf(LogFile, _T("%d MBytes physical memory.\r\n"), (MemInfo.dwTotalPhys +
				ONEM - 1) / ONEM);
	hprintf(LogFile, _T("%d MBytes physical memory free.\r\n"), 
		(MemInfo.dwAvailPhys + ONEM - 1) / ONEM);
	hprintf(LogFile, _T("%d MBytes paging file.\r\n"), (MemInfo.dwTotalPageFile +
				ONEM - 1) / ONEM);
	hprintf(LogFile, _T("%d MBytes paging file free.\r\n"), 
		(MemInfo.dwAvailPageFile + ONEM - 1) / ONEM);
	hprintf(LogFile, _T("%d MBytes user address space.\r\n"), 
		(MemInfo.dwTotalVirtual + ONEM - 1) / ONEM);
	hprintf(LogFile, _T("%d MBytes user address space free.\r\n"), 
		(MemInfo.dwAvailVirtual + ONEM - 1) / ONEM);
}

///////////////////////////////////////////////////////////////////////////////
// GetExceptionDescription
//
// Translate the exception code into something human readable
static const TCHAR *GetExceptionDescription(DWORD ExceptionCode)
{
	struct ExceptionNames
	{
		DWORD	ExceptionCode;
		TCHAR *	ExceptionName;
	};

#if 0  // from winnt.h
#define STATUS_WAIT_0                    ((DWORD   )0x00000000L)    
#define STATUS_ABANDONED_WAIT_0          ((DWORD   )0x00000080L)    
#define STATUS_USER_APC                  ((DWORD   )0x000000C0L)    
#define STATUS_TIMEOUT                   ((DWORD   )0x00000102L)    
#define STATUS_PENDING                   ((DWORD   )0x00000103L)    
#define STATUS_SEGMENT_NOTIFICATION      ((DWORD   )0x40000005L)    
#define STATUS_GUARD_PAGE_VIOLATION      ((DWORD   )0x80000001L)    
#define STATUS_DATATYPE_MISALIGNMENT     ((DWORD   )0x80000002L)    
#define STATUS_BREAKPOINT                ((DWORD   )0x80000003L)    
#define STATUS_SINGLE_STEP               ((DWORD   )0x80000004L)    
#define STATUS_ACCESS_VIOLATION          ((DWORD   )0xC0000005L)    
#define STATUS_IN_PAGE_ERROR             ((DWORD   )0xC0000006L)    
#define STATUS_INVALID_HANDLE            ((DWORD   )0xC0000008L)    
#define STATUS_NO_MEMORY                 ((DWORD   )0xC0000017L)    
#define STATUS_ILLEGAL_INSTRUCTION       ((DWORD   )0xC000001DL)    
#define STATUS_NONCONTINUABLE_EXCEPTION  ((DWORD   )0xC0000025L)    
#define STATUS_INVALID_DISPOSITION       ((DWORD   )0xC0000026L)    
#define STATUS_ARRAY_BOUNDS_EXCEEDED     ((DWORD   )0xC000008CL)    
#define STATUS_FLOAT_DENORMAL_OPERAND    ((DWORD   )0xC000008DL)    
#define STATUS_FLOAT_DIVIDE_BY_ZERO      ((DWORD   )0xC000008EL)    
#define STATUS_FLOAT_INEXACT_RESULT      ((DWORD   )0xC000008FL)    
#define STATUS_FLOAT_INVALID_OPERATION   ((DWORD   )0xC0000090L)    
#define STATUS_FLOAT_OVERFLOW            ((DWORD   )0xC0000091L)    
#define STATUS_FLOAT_STACK_CHECK         ((DWORD   )0xC0000092L)    
#define STATUS_FLOAT_UNDERFLOW           ((DWORD   )0xC0000093L)    
#define STATUS_INTEGER_DIVIDE_BY_ZERO    ((DWORD   )0xC0000094L)    
#define STATUS_INTEGER_OVERFLOW          ((DWORD   )0xC0000095L)    
#define STATUS_PRIVILEGED_INSTRUCTION    ((DWORD   )0xC0000096L)    
#define STATUS_STACK_OVERFLOW            ((DWORD   )0xC00000FDL)    
#define STATUS_CONTROL_C_EXIT            ((DWORD   )0xC000013AL)    
#define STATUS_FLOAT_MULTIPLE_FAULTS     ((DWORD   )0xC00002B4L)    
#define STATUS_FLOAT_MULTIPLE_TRAPS      ((DWORD   )0xC00002B5L)    
#define STATUS_ILLEGAL_VLM_REFERENCE     ((DWORD   )0xC00002C0L)     
#endif

	ExceptionNames ExceptionMap[] =
	{
		{0x40010005, _T("a Control-C")},
		{0x40010008, _T("a Control-Break")},
		{0x80000002, _T("a Datatype Misalignment")},
		{0x80000003, _T("a Breakpoint")},
		{0xc0000005, _T("an Access Violation")},
		{0xc0000006, _T("an In Page Error")},
		{0xc0000017, _T("a No Memory")},
		{0xc000001d, _T("an Illegal Instruction")},
		{0xc0000025, _T("a Noncontinuable Exception")},
		{0xc0000026, _T("an Invalid Disposition")},
		{0xc000008c, _T("a Array Bounds Exceeded")},
		{0xc000008d, _T("a Float Denormal Operand")},
		{0xc000008e, _T("a Float Divide by Zero")},
		{0xc000008f, _T("a Float Inexact Result")},
		{0xc0000090, _T("a Float Invalid Operation")},
		{0xc0000091, _T("a Float Overflow")},
		{0xc0000092, _T("a Float Stack Check")},
		{0xc0000093, _T("a Float Underflow")},
		{0xc0000094, _T("an Integer Divide by Zero")},
		{0xc0000095, _T("an Integer Overflow")},
		{0xc0000096, _T("a Privileged Instruction")},
		{0xc00000fD, _T("a Stack Overflow")},
		{0xc0000142, _T("a DLL Initialization Failed")},
		{0xe06d7363, _T("a Microsoft C++ Exception")},
	};

	for (int i = 0; i < sizeof(ExceptionMap) / sizeof(ExceptionMap[0]); i++)
		if (ExceptionCode == ExceptionMap[i].ExceptionCode)
			return ExceptionMap[i].ExceptionName;

	return _T("an Unknown exception type");
}

///////////////////////////////////////////////////////////////////////////////
// GetFilePart
static TCHAR * GetFilePart(LPCTSTR source)
{
	TCHAR *result = lstrrchr(source, _T('\\'));
	if (result)
		result++;
	else
		result = (TCHAR *)source;
	return result;
}

///////////////////////////////////////////////////////////////////////////////
// DumpStack
static void DumpStack(HANDLE LogFile, DWORD *pStack)
{
	hprintf(LogFile, _T("\r\n\r\nStack:\r\n"));

	__try
	{
		// Esp contains the bottom of the stack, or at least the bottom of
		// the currently used area.
		void* pStackTop;

#ifdef _M_AMD64
		NT_TIB* pTib = (NT_TIB *)NtCurrentTeb();
		pStackTop = pTib->StackBase;
#else
		__asm
		{
			// Load the top (highest address) of the stack from the
			// thread information block. It will be found there in
			// Win9x and Windows NT.
			mov	eax, fs:[4]
			mov pStackTop, eax
		}
#endif

		if (pStackTop > pStack + MaxStackDump)
			pStackTop = pStack + MaxStackDump;

		int Count = 0;

		DWORD* pStackStart = pStack;

		int nDwordsPrinted = 0;

		while (pStack + 1 <= pStackTop)
		{
			if ((Count % StackColumns) == 0)
			{
				pStackStart = pStack;
				nDwordsPrinted = 0;
				hprintf(LogFile, _T("0x%08x: "), pStack);
			}

			if ((++Count % StackColumns) == 0 || pStack + 2 > pStackTop)
			{
				hprintf(LogFile, _T("%08x "), *pStack);
				nDwordsPrinted++;

				int n = nDwordsPrinted;
				while (n < 4)
				{
					hprintf(LogFile, _T("         "));
					n++;
				}

				for (int i = 0; i < nDwordsPrinted; i++)
				{
					DWORD dwStack = *pStackStart;
					for (int j = 0; j < 4; j++)
					{
						char c = (char)(dwStack & 0xFF);
						if (c < 0x20 || c > 0x7E)
							c = '.';
#ifdef _UNICODE
						WCHAR w = (WCHAR)c;
						hprintf(LogFile, _T("%c"), w);
#else
						hprintf(LogFile, _T("%c"), c);
#endif
						dwStack = dwStack >> 8;
					}
					pStackStart++;
				}

				hprintf(LogFile, _T("\r\n"));
			}
			else
			{
				hprintf(LogFile, _T("%08x "), *pStack);
				nDwordsPrinted++;
			}
			pStack++;
		}
		hprintf(LogFile, _T("\r\n"));
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		hprintf(LogFile, _T("Exception encountered during stack dump.\r\n"));
	}
}

///////////////////////////////////////////////////////////////////////////////
// DumpRegisters
static void DumpRegisters(HANDLE LogFile, PCONTEXT Context)
{
	// Print out the register values in an XP error window compatible format.
	hprintf(LogFile, _T("\r\n"));
	hprintf(LogFile, _T("Context:\r\n"));
	hprintf(LogFile, _T("DI:    0x%08x  SI: 0x%08x  AX:   0x%08x\r\n"),
				Context->DIREG, Context->SIREG, Context->AXREG);
	hprintf(LogFile, _T("BX:    0x%08x  CX: 0x%08x  DX:   0x%08x\r\n"),
				Context->BXREG, Context->CXREG, Context->DXREG);
	hprintf(LogFile, _T("IP:    0x%08x  BP: 0x%08x  SegCs: 0x%08x\r\n"),
				Context->IPREG, Context->BPREG, Context->SegCs);
	hprintf(LogFile, _T("Flags: 0x%08x  SP: 0x%08x  SegSs: 0x%08x\r\n"),
				Context->EFlags, Context->SPREG, Context->SegSs);
}

static bool WriteErrorLog(LPCTSTR pszPath, PEXCEPTION_POINTERS pExceptPtrs, LPCTSTR pszModuleFileName)
{
	HANDLE hLogFile = CreateFile(pszPath, GENERIC_WRITE, 0, 0,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, 0);

	if (hLogFile == INVALID_HANDLE_VALUE)
	{
		OutputDebugString(_T("Error creating exception report\r\n"));
		return false;
	}

	// Append to the error log
	SetFilePointer(hLogFile, 0, 0, FILE_END);
	PEXCEPTION_RECORD Exception = pExceptPtrs->ExceptionRecord;
	PCONTEXT          Context   = pExceptPtrs->ContextRecord;

	TCHAR szCrashModulePathName[MAX_PATH*2];
	ZeroMemory(szCrashModulePathName, sizeof(szCrashModulePathName));

	TCHAR *pszCrashModuleFileName = _T("Unknown");

	MEMORY_BASIC_INFORMATION MemInfo;

	// VirtualQuery can be used to get the allocation base associated with a
	// code address, which is the same as the ModuleHandle. This can be used
	// to get the filename of the module that the crash happened in.
	if (VirtualQuery((void*)Context->IPREG, &MemInfo, sizeof(MemInfo)) &&
						(GetModuleFileName((HINSTANCE)MemInfo.AllocationBase,
										  szCrashModulePathName,
										  sizeof(szCrashModulePathName)-2) > 0))
	{
		pszCrashModuleFileName = GetFilePart(szCrashModulePathName);
	}

	// Print out the beginning of the error log in a Win95 error window
	// compatible format.
	hprintf(hLogFile, _T("%s caused %s (0x%08x) \r\nin module %s at %04x:%08x.\r\n\r\n"),
				pszModuleFileName, GetExceptionDescription(Exception->ExceptionCode),
				Exception->ExceptionCode,
				pszCrashModuleFileName, Context->SegCs, Context->IPREG);

	DumpSystemInformation(hLogFile);

	// If the exception was an access violation, print out some additional
	// information, to the error log and the debugger.
	if (Exception->ExceptionCode == STATUS_ACCESS_VIOLATION &&
				Exception->NumberParameters >= 2)
	{
		TCHAR szDebugMessage[1000];
		const TCHAR* readwrite = _T("Read from");
		if (Exception->ExceptionInformation[0])
			readwrite = _T("Write to");
		wsprintf(szDebugMessage, _T("%s location %08x caused an access violation.\r\n"),
					readwrite, Exception->ExceptionInformation[1]);

#ifdef	_DEBUG
		// The Visual C++ debugger doesn't actually tell you whether a read
		// or a write caused the access violation, nor does it tell what
		// address was being read or written. So I fixed that.
		OutputDebugString(_T("Exception handler: "));
		OutputDebugString(szDebugMessage);
#endif

		hprintf(hLogFile, _T("%s"), szDebugMessage);
	}

	DumpRegisters(hLogFile, Context);

	// Print out the bytes of code at the instruction pointer. Since the
	// crash may have been caused by an instruction pointer that was bad,
	// this code needs to be wrapped in an exception handler, in case there
	// is no memory to read. If the dereferencing of code[] fails, the
	// exception handler will print '??'.
	hprintf(hLogFile, _T("\r\nBytes at CS:EIP:\r\n"));
	BYTE * code = (BYTE *)Context->IPREG;
	for (int codebyte = 0; codebyte < NumCodeBytes; codebyte++)
	{
		__try
		{
			hprintf(hLogFile, _T("%02x "), code[codebyte]);

		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			hprintf(hLogFile, _T("?? "));
		}
	}

	// Time to print part or all of the stack to the error log. This allows
	// us to figure out the call stack, parameters, local variables, etc.

	// Esp contains the bottom of the stack, or at least the bottom of
	// the currently used area
	DWORD* pStack = (DWORD *)Context->SPREG;

	DumpStack(hLogFile, pStack);

	DumpModuleList(hLogFile);

	hprintf(hLogFile, _T("\r\n===== [end of %s] =====\r\n"), 
		XCRASHREPORT_ERROR_LOG_FILE);
	hflush(hLogFile);
	CloseHandle(hLogFile);
	return true;
}

static void WriteMiniDump(LPCTSTR pszPath, PEXCEPTION_POINTERS pExceptPtrs)
{
	// Create the file
	HANDLE hMiniDumpFile = CreateFile(
		pszPath,
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
		NULL);

	// Write the minidump to the file
	if (hMiniDumpFile != INVALID_HANDLE_VALUE)
	{
		DumpMiniDump(hMiniDumpFile, pExceptPtrs);

		// Close file
		CloseHandle(hMiniDumpFile);
	}
}

void WriteRegistryDetails(LPCTSTR pszPath)
{
	WriteRegistryTreeToFile(_T("HKCU\\Software\\Perforce"), pszPath);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
//
// RecordExceptionInfo
//
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

int __cdecl RecordExceptionInfo(PEXCEPTION_POINTERS pExceptPtrs)
{
	static bool bFirstTime = true;
	if (!bFirstTime)	// Going recursive! That must mean this routine crashed!
		return EXCEPTION_CONTINUE_SEARCH;
	bFirstTime = false;

	if(!allowER)
		return EXCEPTION_CONTINUE_SEARCH;

	// Create a filename to record the error information to.
	// Storing it in the executable directory works well.

	TCHAR szModuleName[MAX_PATH*2];
	ZeroMemory(szModuleName, sizeof(szModuleName));
	if (GetModuleFileName(0, szModuleName, _countof(szModuleName)-2) <= 0)
		lstrcpy(szModuleName, _T("Unknown"));

	TCHAR *pszFilePart = GetFilePart(szModuleName);

	// Extract the file name portion and remove it's file extension
	TCHAR szFileName[MAX_PATH*2];
	lstrcpy(szFileName, pszFilePart);
	TCHAR *lastperiod = lstrrchr(szFileName, _T('.'));
	if (lastperiod)
		lastperiod[0] = 0;

	// See if they want to create the error report
	TCHAR msg[1024];
	wsprintf(msg, _T("%s has encountered a problem and needs to close.  We are sorry for the inconvenience.\r\rWould you like to create an error report that you can send to Perforce support?"),
		CRASHING_APP_NAME);
	if(::MessageBox(NULL, msg, CRASHING_APP_NAME, MB_YESNO | MB_ICONERROR | MB_TASKMODAL | MB_TOPMOST) != IDYES)
		return EXCEPTION_CONTINUE_SEARCH;


	// Get path to executable directory and append error folder name
	TCHAR szPath[MAX_PATH*2];
	lstrcpyn(szPath, szModuleName, static_cast<int>(pszFilePart - szModuleName));
	szPath[pszFilePart - szModuleName] = 0;
	lstrcat(szPath, XCRASHREPORT_ERROR_FOLDER);
	CreateDirectory(szPath,NULL);

	// Use module name + date to create a unique folder under error folder
	SYSTEMTIME st;
	GetLocalTime(&st);
	wsprintf(szPath+lstrlen(szPath),
		_T("%s_%04d-%02d-%02d_%02d-%02d-%02d\\"),
		szFileName,
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond);
	pszFilePart = szPath + lstrlen(szPath);
	CreateDirectory(szPath,NULL);
	
	// Replace the executable filename with our error log file name
	lstrcpy(pszFilePart, XCRASHREPORT_ERROR_LOG_FILE);

	if(!WriteErrorLog(szPath, pExceptPtrs, szFileName))
		return EXCEPTION_CONTINUE_SEARCH;

	// Replace the filename with our minidump file name
	lstrcpy(pszFilePart, XCRASHREPORT_MINI_DUMP_FILE);

	WriteMiniDump(szPath, pExceptPtrs);

	// Replace the filename with our minidump file name
	lstrcpy(pszFilePart, XCRASHREPORT_REGISTRY_DUMP_FILE);
	WriteRegistryDetails(szPath);
#if 0
	if (IsDebuggerPresent())
	{
		// let the debugger catch this -
		// return the magic value which tells Win32 that this handler didn't
		// actually handle the exception - so that things will proceed as per
		// normal.
		return EXCEPTION_CONTINUE_SEARCH;
	}
	else
#endif
	{
		TCHAR msg[MAX_PATH*2];
		TCHAR *lastslash = lstrrchr(szPath, _T('\\'));
		if (lastslash)
			lastslash[0] = 0;
		wsprintf(msg, _T("An error report was saved at %s."), szPath);
		::MessageBox(NULL, msg, CRASHING_APP_NAME, MB_OK | MB_ICONINFORMATION | MB_TASKMODAL | MB_TOPMOST);
		return EXCEPTION_EXECUTE_HANDLER;
	}
}
