; Review-only probe for JDK-8365196 (review5). Assembled with ml64.
; Takes one access violation with chosen stack contents below rsp and reports which extended
; state survived the resume. The vectored exception handler in xprobe.c resumes at xprobe_cont.

OPTION CASEMAP:NONE

PUBLIC xprobe_run
PUBLIC xprobe_fault
PUBLIC xprobe_cont
PUBLIC amx_ldtilecfg
PUBLIC amx_tilezero0
PUBLIC amx_tilerelease
PUBLIC fill_below

; struct Probe (offsets in bytes)
;   0 fill_qwords  number of qwords written below the trap rsp: fill[i] -> [rsp - 8*(i+1)]
;   8 sp_adjust    bytes subtracted from the 64-byte aligned rsp
;  16 fill_ptr     qword array
;  24 scrub_len    0: none; else the one-store scrub: mov qword [((rsp - L) & -64) + 8], 0
;  32 flags        bit0: AVX-512 registers (zmm0 full, zmm16, k1)
;  40 trap_rsp     out: rsp at the fault
;  48 base_rsp     in: 0, or an absolute rsp to use instead of the current one (before align/adjust)
;  64 out zmm0 / ymm0 (64 bytes)
; 128 out zmm16 (64 bytes)
; 192 out k1

_TEXT SEGMENT

xprobe_run PROC
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    mov r12, rcx
    mov r13, rsp
    mov rax, qword ptr [r12+48]
    test rax, rax
    jz use_current
    mov rsp, rax
use_current:
    and rsp, -64
    sub rsp, qword ptr [r12+8]
    mov qword ptr [r12+40], rsp
    mov r8, qword ptr [r12+16]
    mov r9, qword ptr [r12+0]
    mov r10, rsp
    xor r11, r11
fill_loop:
    test r9, r9
    jz fill_done
    mov rax, qword ptr [r8 + r11*8]
    sub r10, 8
    mov qword ptr [r10], rax
    inc r11
    dec r9
    jmp fill_loop
fill_done:
    mov eax, 11111111h
    vmovd xmm0, eax
    vpbroadcastd ymm0, xmm0
    test qword ptr [r12+32], 1
    jz no512
    mov eax, 22222222h
    vpbroadcastd zmm0, eax
    mov eax, 33333333h
    vpbroadcastd zmm16, eax
    mov eax, 0A5A5h
    kmovw k1, eax
no512:
    mov rax, qword ptr [r12+24]
    test rax, rax
    jz no_scrub
    mov rdx, rsp
    sub rdx, rax
    and rdx, -64
    mov qword ptr [rdx+8], 0
no_scrub:
    xor edx, edx
xprobe_fault LABEL NEAR
    mov eax, dword ptr [rdx]
xprobe_cont LABEL NEAR
    test qword ptr [r12+32], 1
    jz st256
    vmovdqu64 zmmword ptr [r12+64], zmm0
    vmovdqu64 zmmword ptr [r12+128], zmm16
    kmovw eax, k1
    mov qword ptr [r12+192], rax
    jmp stdone
st256:
    vmovdqu ymmword ptr [r12+64], ymm0
stdone:
    vzeroupper
    mov rsp, r13
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
xprobe_run ENDP

; void amx_ldtilecfg(const void* cfg)
amx_ldtilecfg PROC
    db 0C4h, 0E2h, 078h, 049h, 001h     ; ldtilecfg [rcx]
    ret
amx_ldtilecfg ENDP

; void amx_tilezero0(void)
amx_tilezero0 PROC
    db 0C4h, 0E2h, 07Bh, 049h, 0C0h     ; tilezero tmm0
    ret
amx_tilezero0 ENDP

; void amx_tilerelease(void)
amx_tilerelease PROC
    db 0C4h, 0E2h, 078h, 049h, 0C0h     ; tilerelease
    ret
amx_tilerelease ENDP

; void fill_below(long long qwords, long long value): writes value to the qwords below rsp
fill_below PROC
    mov rax, rsp
fb_loop:
    sub rax, 8
    mov qword ptr [rax], rdx
    dec rcx
    jnz fb_loop
    ret
fill_below ENDP

_TEXT ENDS
END
