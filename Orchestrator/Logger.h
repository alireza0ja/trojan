#pragma once
#include <windows.h>
#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <ctime>
#include "Config.h"

/* Categories for logging */
enum LogCategory {
    SUCCESS,
    ERROR_LOG,
    INFO,
    VERIFY,
    NETWORK,
    ATOM_EVENT,
    TURBO_PULSE,
    MESH_SYNC
};

class Logger {
public:
    static void Init(const std::string& logFile, bool enableConsole) {
        getInstance().m_logFile = logFile;
        getInstance().m_enableConsole = enableConsole;
        
        if (enableConsole) {
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            std::cout << "[*] --- SHATTERED MIRROR: SUPER LOGGING ENGINE INITIALIZED ---" << std::endl;
        }

        getInstance().LogInternal(INFO, "Logging system started. Log File: " + logFile);
    }

    static void Log(LogCategory cat, const std::string& msg) {
        getInstance().LogInternal(cat, msg);
    }

    /* Verification helper */
    static bool Verify(const std::string& action, bool condition, const std::string& errorDetail = "Unknown Error") {
        if (condition) {
            Log(VERIFY, "Action Verified: [" + action + "] -> [PASSED]");
            return true;
        } else {
            Log(ERROR_LOG, "Action Failed: [" + action + "] -> [FAILED] | Reason: " + errorDetail);
            return false;
        }
    }

    static void Shutdown() {
        if (getInstance().m_enableConsole) {
            FreeConsole();
        }
    }

private:
    Logger() : m_enableConsole(false) {}
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void LogInternal(LogCategory cat, const std::string& msg) {
        if (!Config::LOGGING_ENABLED) return;
        std::string tag;
        switch (cat) {
            case SUCCESS:    tag = "[+] SUCCESS "; break;
            case ERROR_LOG:  tag = "[!] ERROR   "; break;
            case INFO:       tag = "[*] INFO    "; break;
            case VERIFY:     tag = "[?] VERIFY  "; break;
            case NETWORK:    tag = "[N] NETWORK "; break;
            case ATOM_EVENT: tag = "[A] ATOM    "; break;
            case TURBO_PULSE:tag = "[T] TURBO   "; break;
            case MESH_SYNC: tag = "[M] MESH    "; break;
        }

        time_t now = time(0);
        char timeBuf[26];
        ctime_s(timeBuf, sizeof(timeBuf), &now);
        std::string timestamp(timeBuf);
        timestamp = timestamp.substr(0, timestamp.length() - 1); // remove newline

        std::string formatted = timestamp + " | " + tag + " | " + msg;

        // Write to file
        std::ofstream file(m_logFile, std::ios::app);
        if (file.is_open()) {
            file << formatted << std::endl;
            file.flush();
            file.close();
        }

        // Write to console
        if (m_enableConsole) {
            std::cout << formatted << std::endl;
        }
    }

    std::string m_logFile;
    bool m_enableConsole;
};
