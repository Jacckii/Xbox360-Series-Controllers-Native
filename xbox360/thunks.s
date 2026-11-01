# SPDX-License-Identifier: GPL-3.0-or-later
#
# Entry points the kernel calls. OpenXeChain's clang uses the ppc32 SVR4 ABI and saves
# r14-r31 as 32-bit values; the kernel keeps 64-bit values there. Each thunk saves the
# full 64-bit r14-r31 around the C body (named <thunk>C). Arguments in r3-r10 and the
# result in r3 pass through untouched.
#
# Frame: 0 back chain, 4 callee LR slot, 16..159 r14-r31, 168 LR.
# (Names starting with "L" are assembler-private on this target.)

    .macro NV64_THUNK name, target
    .text
    .globl \name
    .p2align 2
\name:
    mflr 0
    stwu 1, -176(1)
    stw 0, 168(1)
    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    std \r, (16 + (\r - 14) * 8)(1)
    .endr
    bl \target
    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld \r, (16 + (\r - 14) * 8)(1)
    .endr
    lwz 0, 168(1)
    mtlr 0
    addi 1, 1, 176
    blr
    .endm

    NV64_THUNK DllMain, DllMainC
    NV64_THUNK XaLinkDpc, XaLinkDpcC
    NV64_THUNK XaMatch, XaMatchC
    NV64_THUNK XaAddDevice, XaAddDeviceC
    NV64_THUNK XaRemoveDevice, XaRemoveDeviceC
    NV64_THUNK XaTransferDone, XaTransferDoneC
    NV64_THUNK XaInDone, XaInDoneC
    NV64_THUNK XaCloseDone, XaCloseDoneC
    NV64_THUNK XaThread, XaThreadC
    NV64_THUNK XaCallDpc, XaCallDpcC

# XInput hooks (xinput_360.c): kernel and XAM jump here from the hooked functions
    NV64_THUNK XaReadStateHook, XaReadStateHookC
    NV64_THUNK XaCapsHook, XaCapsHookC
    NV64_THUNK XaSetStateHook, XaSetStateHookC
    NV64_THUNK XaXamThread, XaXamThreadC

# xa_kcall9(args, fn): calls kernel function fn with the nine u32 arguments at args.
# The 360 ABI reads a ninth argument from its 8-byte parameter slot, low word at
# 0x54(r1); clang puts stack arguments elsewhere (96(r1)), so the kernel would read
# whatever lies at 0x54. args[0..7] go in r3-r10, args[8] into that slot. Taking only
# register arguments keeps clang's own stack-argument layout out of it. The frame covers
# the parameter slots the callee may spill to; LR is kept above them.
    .text
    .globl xa_kcall9
    .p2align 2
xa_kcall9:
    mflr 0
    stwu 1, -112(1)
    stw 0, 104(1)
    mtctr 4
    li 0, 0
    stw 0, 0x50(1)
    lwz 0, 32(3)
    stw 0, 0x54(1)
    lwz 4, 4(3)
    lwz 5, 8(3)
    lwz 6, 12(3)
    lwz 7, 16(3)
    lwz 8, 20(3)
    lwz 9, 24(3)
    lwz 10, 28(3)
    lwz 3, 0(3)
    bctrl
    lwz 0, 104(1)
    mtlr 0
    addi 1, 1, 112
    blr

# detour trampolines (detour.c): 8 x 30 words, written at run time. In .text because
# code in data pages doesn't run on the 360.
    .text
    .globl xa_trampolines
    .p2align 4
xa_trampolines:
    .space 8 * 30 * 4
