#include <cstdio>
#include <windows.h>
#include "Config.h"
#include "AtomManager.h"

/*
 * Shattered Mirror v1 — Standalone Entry Point
 */

// Reference the main orchestrator loop from AtomManager.cpp
extern DWORD WINAPI OrchestratorMain(LPVOID lpParam);

int main() {
  if (Config::LOGGING_ENABLED) {
    printf("==========================================\n");
    printf("   SHATTERED MIRROR — STANDALONE CORE     \n");
    printf("==========================================\n");
    printf("[*] Initializing execution environment...\n");

    // Create log directory if it doesn't exist
    CreateDirectoryA("log", NULL);

    // Clear logs if they exist
    DeleteFileA("log\\shattered_debug.log");
  }

  // Call the main loop directly.
  OrchestratorMain(NULL);

  return 0;
}
