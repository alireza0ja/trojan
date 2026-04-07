#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Common Definitions
 * Shared types, PE structures, and compile-time hash macros.
 * Target: Windows 10 1809+ / Windows 11 (all builds through 25H2+)
 * Arch:   x86-64 only
 *===========================================================================*/

#ifndef SMIRROR_COMMON_H
#define SMIRROR_COMMON_H

#include <windows.h>

/*---------------------------------------------------------------------------
 *  NT Status / Type Definitions (avoid including ntdll headers directly)
 *-------------------------------------------------------------------------*/
typedef LONG NTSTATUS;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

typedef struct _SM_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} SM_UNICODE_STRING, *PSM_UNICODE_STRING;

typedef struct _SM_OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PSM_UNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} SM_OBJECT_ATTRIBUTES, *PSM_OBJECT_ATTRIBUTES;

typedef struct _SM_CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} SM_CLIENT_ID, *PSM_CLIENT_ID;

typedef struct _SM_PS_ATTRIBUTE {
    ULONG_PTR Attribute;
    SIZE_T    Size;
    union {
        ULONG_PTR Value;
        PVOID     ValuePtr;
    };
    PSIZE_T ReturnLength;
} SM_PS_ATTRIBUTE, *PSM_PS_ATTRIBUTE;

typedef struct _SM_PS_ATTRIBUTE_LIST {
    SIZE_T         TotalLength;
    SM_PS_ATTRIBUTE Attributes[1];
} SM_PS_ATTRIBUTE_LIST, *PSM_PS_ATTRIBUTE_LIST;

/*---------------------------------------------------------------------------
 *  Compile-Time DJB2 Hash (for string-free function resolution)
 *  We hash function names at compile time; at runtime we compare hashes
 *  instead of strcmp. No static strings in the binary.
 *-------------------------------------------------------------------------*/
constexpr DWORD djb2_hash_ct(const char* str) {
    DWORD hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(*str);
        str++;
    }
    return hash;
}

/* Runtime version (for walking export tables) */
inline DWORD djb2_hash_rt(const char* str) {
    DWORD hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(*str);
        str++;
    }
    return hash;
}

/*---------------------------------------------------------------------------
 *  Pre-computed hashes for target NT functions
 *  Generated with djb2_hash_ct at compile time
 *-------------------------------------------------------------------------*/
#define HASH_NtAllocateVirtualMemory   djb2_hash_ct("NtAllocateVirtualMemory")
#define HASH_NtWriteVirtualMemory      djb2_hash_ct("NtWriteVirtualMemory")
#define HASH_NtProtectVirtualMemory    djb2_hash_ct("NtProtectVirtualMemory")
#define HASH_NtCreateThreadEx          djb2_hash_ct("NtCreateThreadEx")
#define HASH_NtSetContextThread        djb2_hash_ct("NtSetContextThread")
#define HASH_NtGetContextThread        djb2_hash_ct("NtGetContextThread")
#define HASH_NtClose                   djb2_hash_ct("NtClose")
#define HASH_NtOpenProcess             djb2_hash_ct("NtOpenProcess")
#define HASH_NtCreateSection           djb2_hash_ct("NtCreateSection")
#define HASH_NtMapViewOfSection        djb2_hash_ct("NtMapViewOfSection")
#define HASH_NtUnmapViewOfSection      djb2_hash_ct("NtUnmapViewOfSection")
#define HASH_NtQueueApcThread          djb2_hash_ct("NtQueueApcThread")
#define HASH_NtWaitForSingleObject     djb2_hash_ct("NtWaitForSingleObject")
#define HASH_NtDelayExecution          djb2_hash_ct("NtDelayExecution")
#define HASH_NtSetInformationThread    djb2_hash_ct("NtSetInformationThread")
#define HASH_NtResumeThread            djb2_hash_ct("NtResumeThread")
#define HASH_NtSuspendThread           djb2_hash_ct("NtSuspendThread")
#define HASH_NtQueryInformationProcess djb2_hash_ct("NtQueryInformationProcess")

/* Module name hashes */
#define HASH_NTDLL                     djb2_hash_ct("ntdll.dll")
#define HASH_KERNEL32                  djb2_hash_ct("kernel32.dll")
#define HASH_ADVAPI32                  djb2_hash_ct("advapi32.dll")

/*---------------------------------------------------------------------------
 *  PEB / LDR structures (for manual module resolution via PEB walk)
 *  Avoids GetModuleHandle / GetProcAddress which are hooked by EDRs
 *-------------------------------------------------------------------------*/
typedef struct _PEB_LDR_DATA_FULL {
    ULONG      Length;
    BOOLEAN    Initialized;
    HANDLE     SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA_FULL, *PPEB_LDR_DATA_FULL;

typedef struct _LDR_DATA_TABLE_ENTRY_FULL {
    LIST_ENTRY     InLoadOrderLinks;
    LIST_ENTRY     InMemoryOrderLinks;
    LIST_ENTRY     InInitializationOrderLinks;
    PVOID          DllBase;
    PVOID          EntryPoint;
    ULONG          SizeOfImage;
    SM_UNICODE_STRING FullDllName;
    SM_UNICODE_STRING BaseDllName;
    /* ... more fields exist but we don't need them */
} LDR_DATA_TABLE_ENTRY_FULL, *PLDR_DATA_TABLE_ENTRY_FULL;

/*---------------------------------------------------------------------------
 *  Syscall table entry — stores resolved SSN + gadget for one NT function
 *-------------------------------------------------------------------------*/
typedef struct _SYSCALL_ENTRY {
    DWORD  dwHash;          /* DJB2 hash of function name              */
    DWORD  dwSSN;           /* System Service Number                   */
    PVOID  pSyscallGadget;  /* Address of syscall;ret inside ntdll     */
    PVOID  pFunctionAddr;   /* Original function address in ntdll      */
    BOOL   bResolved;       /* TRUE if successfully resolved           */
} SYSCALL_ENTRY, *PSYSCALL_ENTRY;

/*---------------------------------------------------------------------------
 *  Max number of syscall entries we track
 *-------------------------------------------------------------------------*/
#define MAX_SYSCALL_ENTRIES 32

/*---------------------------------------------------------------------------
 *  Global syscall table (populated during init)
 *-------------------------------------------------------------------------*/
typedef struct _SYSCALL_TABLE {
    SYSCALL_ENTRY Entries[MAX_SYSCALL_ENTRIES];
    DWORD         dwCount;
    PVOID         pNtdllBase;     /* Cached ntdll base address */
    DWORD         dwNtdllSize;    /* Size of ntdll image       */
} SYSCALL_TABLE, *PSYSCALL_TABLE;

#endif /* SMIRROR_COMMON_H */
