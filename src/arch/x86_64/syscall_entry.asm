; SPDX-License-Identifier: GPL-2.0-or-later
; VortexOS Kernel — Syscall Entry Point

global syscall_entry
extern syscall_handler

syscall_entry:
    ; 1. Swap GS to kernel GS
    swapgs
    
    ; 2. Switch to kernel stack
    ; Assuming TSS.rsp0 is set correctly in GDT (Phase 1.2)
    ; We need to save the user RSP before switching.
    ; Since we don't have a per-CPU structure yet, let's just use a scratch area for now.
    mov [gs:0], rsp
    mov rsp, [gs:8] ; Load kernel stack from per-CPU struct
    
    ; 3. Save user registers (to match InterruptFrame)
    push ss
    push qword [gs:0] ; User RSP
    push r11          ; User RFLAGS
    push rcx          ; User RIP
    
    push 0            ; Error code
    push 0            ; Vector
    
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; 4. Call syscall_handler (RDI is first arg = frame)
    mov rdi, rsp
    call syscall_handler
    
    ; 5. Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    add rsp, 16 ; Skip vector and error code
    
    ; 6. Restore user context
    pop rcx ; RIP
    pop r11 ; RFLAGS
    pop rsp ; RSP
    
    ; 7. Swap GS back
    swapgs
    
    ; 8. Return to user
    sysretq
