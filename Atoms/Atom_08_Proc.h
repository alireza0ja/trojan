#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 08: Process Enumeration & Management
 *
 * Enumerates running processes via Toolhelp32 snapshots and reports
 * the process list back to the Orchestrator via IPC. Can also
 * terminate a target process by name.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_08_PROC_H
#define SMIRROR_ATOM_08_PROC_H

#include <windows.h>

/* Launch Process Manager thread */
DWORD WINAPI ProcessAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_08_PROC_H */
