// JDK-8365196, review 11: the main loop of R10Bench.compareAcrossLoadMaskedStore and of
// R9Bench.segmentCompareBlendStore as C2 emits them with AVX-512 (PrintOptoAssembly of bench run
// 46154984810, Xeon 8370C), with the limit of the compare made in different ways. One function per
// variant, System V calling convention: rdi = src, rsi = src2, rdx = dst.
//
// O   the mode off: the limit in zmm1 across the loop, kortest
// K   the limit in zmm1 across the loop, kmov and cmp (not allowed by the rule; isolates the test)
// KN  as K, with a 10 byte nop where the broadcast would be (a slot of the front end, no port)
// KL  as K, with a scalar load of the limit where the broadcast would be (a load, no vector)
// M   the mode on: vpbroadcastd zmm1, m32 in front of each compare
// G   vpbroadcastd zmm1, r32 in front of each compare
// X   vpbroadcastd zmm1, xmm (the limit in the low 128 bits of xmm15)
// E   the limit as the m32bcst operand of the compare
// F   the limit as the m512 operand of the compare
// MD  as M, the element in the data section and not next to the code
// K2, K3, K4, K5, KF; MK3; XK3: the limit as in K, M or X, the bits of the mask by kmovw and
// a 32 bit cmp (2), by kmovw and the 64 bit cmp (3), by kmovd and the 64 bit cmp (4), by kmovq
// and a 32 bit cmp (5), or the test by the condition codes, set behind the compare and read in
// front of the branch (F; in blend, where no raw access is between the compare and the branch,
// the kortest of the mode off)
        .intel_syntax noprefix

        .data
        .p2align 6
d100:   .long 100
        .long 100

        .text
        .p2align 6
c100v:  .rept 16
        .long 100
        .endr
c100:   .long 100
        .long 100

.macro LIMIT v
  .if \v == 4
        vpbroadcastd zmm1, dword ptr [rip + c100]
  .elseif \v == 5
        vpbroadcastd zmm1, r11d
  .elseif \v == 6
        vpbroadcastd zmm1, xmm15
  .elseif \v == 2
        .byte 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
  .elseif \v == 3
        mov r11d, dword ptr [rip + c100]
  .elseif \v == 9
        vpbroadcastd zmm1, dword ptr [rip + d100]
  .endif
.endm

.macro COMPARE v
  .if \v == 7
        vpcmpd k7, zmm0, dword ptr [rip + c100]{1to16}, 1
  .elseif \v == 8
        vpcmpd k7, zmm0, zmmword ptr [rip + c100v], 1
  .else
        vpcmpd k7, zmm0, zmm1, 1
  .endif
.endm


// How the mask is tested. t = 1: kmovq and cmp (the mode on). t = 2: kmovw and a 32 bit cmp.
// t = 3: kortest and setb behind the compare, test and jne in front of the branch (the test
// as a byte in a general purpose register). t = 4: kortest behind the compare, jb in front of
// the branch (the condition codes across the instructions in between, which must not set them).
.macro TEST_DEF t
  .if \t == 1
        kmovq rsi, k7
  .elseif \t == 2
        kmovw esi, k7
  .elseif \t == 3
        kortestw k7, k7
        setb sil
  .elseif \t == 4
        kortestw k7, k7
  .elseif \t == 5
        kmovw esi, k7
  .elseif \t == 6
        kmovd esi, k7
  .elseif \t == 7
        kmovq rsi, k7
  .endif
.endm

.macro TEST_USE t
  .if \t == 1
        cmp rsi, 65535
        je 9f
  .elseif \t == 2
        cmp esi, 65535
        je 9f
  .elseif \t == 3
        test sil, sil
        jne 9f
  .elseif \t == 4
        jb 9f
  .elseif \t == 5 || \t == 6
        cmp rsi, 65535
        je 9f
  .elseif \t == 7
        cmp esi, 65535
        je 9f
  .endif
.endm

.macro PROLOG
        push rbx
        push rbp
        push r12
        push r13
        push r14
        push r15
        sub rsp, 88
        mov rcx, rdi
        mov r9, rsi
        mov rax, rdx
        mov rbx, 0x7fffffff
        mov r11d, 100
        vpbroadcastd zmm1, r11d
        vmovd xmm15, r11d
        mov dword ptr [rsp + 68], 0
        xor r8d, r8d
        xor r10d, r10d
.endm

.macro EPILOG
        vzeroupper
        add rsp, 88
        pop r15
        pop r14
        pop r13
        pop r12
        pop rbp
        pop rbx
        ret
.endm

// compareAcrossLoadMaskedStore, the mode on (v selects how the limit is made)
.macro MASKED_N name, v, t=1
        .globl \name
        .p2align 6
\name:
        PROLOG
        jmp 2f
        .p2align 4
1:      mov r8d, r10d
        mov r10d, dword ptr [rsp + 68]
2:      movsxd rdx, r8d
        shl rdx, 2
        mov rdi, rcx
        add rdi, rdx
        vmovdqu32 zmm0, zmmword ptr [rdi]
        LIMIT \v
        COMPARE \v
  .if \t >= 3
        mov rbp, r9
        add rbp, rdx
        TEST_DEF \t
  .else
        TEST_DEF \t
        mov rbp, r9
        add rbp, rdx
  .endif
        vpaddd zmm0, zmm0, zmmword ptr [rbp]
        TEST_USE \t
        cmp rdx, rbx
        jg 9f
        mov dword ptr [rsp + 68], r10d
        lea r10, [rdx + 64]
        mov r13, rax
        add r13, rdx
        vmovdqu32 zmmword ptr [r13]{k7}, zmm0
        vmovdqu32 zmm0, zmmword ptr [rdi + 64]
        LIMIT \v
        COMPARE \v
        TEST_DEF \t
        vpaddd zmm0, zmm0, zmmword ptr [rbp + 64]
        TEST_USE \t
        cmp r10, rbx
        jg 9f
        vmovdqu32 zmmword ptr [r13 + 64]{k7}, zmm0
        vmovdqu32 zmm0, zmmword ptr [rdi + 128]
        LIMIT \v
        COMPARE \v
        TEST_DEF \t
        vpaddd zmm0, zmm0, zmmword ptr [rbp + 128]
        lea r10, [rdx + 128]
        TEST_USE \t
        cmp r10, rbx
        jg 9f
        vmovdqu32 zmmword ptr [r13 + 128]{k7}, zmm0
        vmovdqu32 zmm0, zmmword ptr [rdi + 192]
        LIMIT \v
        COMPARE \v
        TEST_DEF \t
        vpaddd zmm0, zmm0, zmmword ptr [rbp + 192]
        lea r10, [rdx + 192]
        TEST_USE \t
        cmp r10, rbx
        jg 9f
        vmovdqu32 zmmword ptr [r13 + 192]{k7}, zmm0
        lea r10d, [r8 + 64]
        cmp r10d, 944
        jl 1b
9:      EPILOG
.endm

// compareAcrossLoadMaskedStore, the mode off
        .globl masked_O
        .p2align 6
masked_O:
        PROLOG
        mov r13, rcx
        mov rsi, rax
        mov rax, r9
        mov r9, rbx
        xor ebp, ebp
        mov qword ptr [rsp + 40], 0
        jmp 2f
        .p2align 4
1:      mov r10d, ebp
        mov rdx, qword ptr [rsp + 40]
2:      movsxd r8, r10d
        shl r8, 2
        mov rdi, r13
        add rdi, r8
        vmovdqu32 zmm0, zmmword ptr [rdi]
        mov rbp, rax
        add rbp, r8
        vpaddd zmm2, zmm0, zmmword ptr [rbp]
        vpcmpd k7, zmm0, zmm1, 1
        kortestw k7, k7
        jb 9f
        cmp r8, r9
        jg 9f
        mov qword ptr [rsp + 40], rdx
        lea rcx, [r8 + 64]
        mov rdx, rsi
        add rdx, r8
        vmovdqu32 zmmword ptr [rdx]{k7}, zmm2
        vmovdqu32 zmm0, zmmword ptr [rdi + 64]
        vpaddd zmm2, zmm0, zmmword ptr [rbp + 64]
        vpcmpd k7, zmm0, zmm1, 1
        kortestw k7, k7
        jb 9f
        cmp rcx, r9
        jg 9f
        vmovdqu32 zmmword ptr [rdx + 64]{k7}, zmm2
        vmovdqu32 zmm0, zmmword ptr [rdi + 128]
        vpaddd zmm2, zmm0, zmmword ptr [rbp + 128]
        lea rcx, [r8 + 128]
        vpcmpd k7, zmm0, zmm1, 1
        kortestw k7, k7
        jb 9f
        cmp rcx, r9
        jg 9f
        vmovdqu32 zmmword ptr [rdx + 128]{k7}, zmm2
        vmovdqu32 zmm0, zmmword ptr [rdi + 192]
        vpaddd zmm2, zmm0, zmmword ptr [rbp + 192]
        lea rcx, [r8 + 192]
        vpcmpd k7, zmm0, zmm1, 1
        kortestw k7, k7
        jb 9f
        cmp rcx, r9
        jg 9f
        vmovdqu32 zmmword ptr [rdx + 192]{k7}, zmm2
        lea ebp, [r10 + 64]
        cmp ebp, 944
        jl 1b
9:      EPILOG

        MASKED_N masked_K, 1
        MASKED_N masked_KN, 2
        MASKED_N masked_KL, 3
        MASKED_N masked_M, 4
        MASKED_N masked_G, 5
        MASKED_N masked_X, 6
        MASKED_N masked_E, 7
        MASKED_N masked_F, 8
        MASKED_N masked_MD, 9
        MASKED_N masked_K2, 1, 2
        MASKED_N masked_K3, 1, 5
        MASKED_N masked_K4, 1, 6
        MASKED_N masked_K5, 1, 7
        MASKED_N masked_KF, 1, 4
        MASKED_N masked_MK3, 4, 5
        MASKED_N masked_XK3, 6, 5

// segmentCompareBlendStore: zmm2 = 0 and zmm3 = 1 across the loop (the operands of the blend and
// of the masked add reach no check), the limit in zmm1.
// sh: 1 = the add reads its second operand from memory (src2), a second load of 64 bytes for
// each vector; 2 = no blend. So blend is one load and three vector operations for each vector
// (compare, blend, add), L2A3 two loads and three, L1A2 one load and two, L2A2 two and two.
.macro BLEND_VEC v, off, test, sh=0
        vmovdqu32 zmm0, zmmword ptr [rdi + \off]
        LIMIT \v
        COMPARE \v
  .if \test == 3
        TEST_DEF 3
  .endif
  .if (\sh & 2) == 0
        vpblendmd zmm0{k7}, zmm0, zmm2
  .endif
  .if (\sh & 1) == 0
        vpaddd zmm0{k7}, zmm0, zmm3
  .else
        vpaddd zmm0{k7}, zmm0, zmmword ptr [r15 + r13 + \off]
  .endif
  .if \test == 0
        kortestw k7, k7
        jb 9f
  .elseif \test == 3
        TEST_USE 3
  .else
        TEST_DEF \test
        TEST_USE \test
  .endif
.endm

.macro BLEND name, v, test, sh=0
        .globl \name
        .p2align 6
\name:
        PROLOG
        vpxord zmm2, zmm2, zmm2
        mov r14d, 1
        vpbroadcastd zmm3, r14d
        mov r12, rcx
        mov rbp, rax
        mov r15, r9
        mov r9, rbx
        xor ecx, ecx
        jmp 2f
        .p2align 4
1:      mov ecx, edi
2:      movsxd r13, ecx
        shl r13, 2
        mov rdi, r12
        add rdi, r13
        BLEND_VEC \v, 0, \test, \sh
        cmp r13, r9
        jg 9f
        lea r8, [r13 + 64]
        mov r14, rbp
        add r14, r13
        vmovdqu32 zmmword ptr [r14]{k7}, zmm0
        BLEND_VEC \v, 64, \test, \sh
        cmp r8, r9
        jg 9f
        vmovdqu32 zmmword ptr [r14 + 64]{k7}, zmm0
        lea r8, [r13 + 128]
        BLEND_VEC \v, 128, \test, \sh
        cmp r8, r9
        jg 9f
        vmovdqu32 zmmword ptr [r14 + 128]{k7}, zmm0
        lea r8, [r13 + 192]
        BLEND_VEC \v, 192, \test, \sh
        cmp r8, r9
        jg 9f
        vmovdqu32 zmmword ptr [r14 + 192]{k7}, zmm0
        lea edi, [rcx + 64]
        cmp edi, 944
        jl 1b
9:      EPILOG
.endm

        BLEND blend_O, 1, 0
        BLEND blend_K, 1, 1
        BLEND blend_KN, 2, 1
        BLEND blend_KL, 3, 1
        BLEND blend_M, 4, 1
        BLEND blend_G, 5, 1
        BLEND blend_X, 6, 1
        BLEND blend_E, 7, 1
        BLEND blend_F, 8, 1
        BLEND blend_MD, 9, 1
        BLEND blend_K2, 1, 2
        BLEND blend_K3, 1, 5
        BLEND blend_K4, 1, 6
        BLEND blend_K5, 1, 7
        BLEND blend_KF, 1, 0
        BLEND blend_MK3, 4, 5
        BLEND blend_XK3, 6, 5

        BLEND l2a3_O, 1, 0, 1
        BLEND l2a3_K3, 1, 5, 1
        BLEND l2a3_MK3, 4, 5, 1
        BLEND l2a3_XK3, 6, 5, 1
        BLEND l1a2_O, 1, 0, 2
        BLEND l1a2_K3, 1, 5, 2
        BLEND l1a2_MK3, 4, 5, 2
        BLEND l1a2_XK3, 6, 5, 2
        BLEND l2a2_O, 1, 0, 3
        BLEND l2a2_K3, 1, 5, 3
        BLEND l2a2_MK3, 4, 5, 3
        BLEND l2a2_XK3, 6, 5, 3
