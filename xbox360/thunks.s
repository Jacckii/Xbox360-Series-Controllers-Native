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

# detour trampolines (detour.c): 8 x 30 words, written at run time. In .text because
# code in data pages doesn't run on the 360.
    .text
    .globl xa_trampolines
    .p2align 4
xa_trampolines:
    .space 8 * 30 * 4
