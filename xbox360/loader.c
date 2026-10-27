// SPDX-License-Identifier: GPL-3.0-or-later
//
// Development loader: a title that loads a system-DLL plugin without a reboot.
// Normally the plugin is a launch.ini boot plugin.
//
// What loading a system DLL from a title takes on kernel 17559 (XeUnshackle + Aurora):
//   - a title that statically imports XexLoadImage (ord 409) never reaches its entry
//     point (Aurora hooks that export), so it is resolved at runtime;
//   - XexLoadImage(flags 8 = system DLL) returns C0000022 unless the calling thread's
//     process type is 2 (system). The kernel reads it from KTHREAD+0x73
//     (KeGetCurrentProcessType, 0x80071A68), and KeSetCurrentProcessType only works
//     inside a DPC, so the loader flips that byte around the call and restores it.
//
// A loaded module's name can't be loaded twice: to load a second copy next to a
// resident copy, build it with its own /base and name (build.sh XA_SLOT, -DPLUGIN_NAME).
//
// g_stage (magic "STGE", read over xbdm): [1] 3 main, 5 resolved, 10 loading,
// 20 done; [2] heartbeat; [3] XexLoadImage status; [6] its address; [7] old process type.
// The title then launches Aurora; if that export can't be resolved, it idles instead.

#include <xecore/xboxkrnl.h>

#ifndef PLUGIN_NAME
#define PLUGIN_NAME "xa.xex" /* build.sh XA_SLOT sets xa<n>.xex */
#endif
#ifndef PLUGIN_PATH
#define PLUGIN_PATH "\\Device\\Mass0\\Apps\\xadapter\\" PLUGIN_NAME
#endif

__attribute__((used)) volatile uint32_t g_stage[8] = {0x53544745 /* STGE */};

typedef NTSTATUS (*XexLoadImage_t)(const char *, uint32_t, uint32_t, HMODULE *);

#define ORD_XexLoadImage     409
#define XEX_LOADIMG_FLAG_DLL 8
#define PROC_SYSTEM          2
#define ORD_XamLoaderLaunchTitleEx 421 /* xam.xex: launch a title with its game: mount */

#define AURORA_DIR  "\\Device\\Mass0\\Apps\\Aurora"
#define AURORA_PATH AURORA_DIR "\\Aurora.xex"

typedef NTSTATUS (*XamLoaderLaunchTitleEx_t)(const char *path, const char *mount,
                                             const char *cmdline, uint32_t flags);

extern void _start(void);

// PE entry point (-Wl,/entry:stage_entry): marks that the title runs at all, then crt0
void stage_entry(void) {
    g_stage[1] = 1;
    _start();
}

static void sleep_s(int s) {
    int64_t interval = -(int64_t)s * 10000000;
    KeDelayExecutionThread(1, 0, &interval);
}

// KTHREAD of the running thread: PCR (r13) + 0x100
static volatile uint8_t *thread_proc_type(void) {
    uint32_t pcr;
    __asm__ volatile("mr %0, 13" : "=r"(pcr));
    return (volatile uint8_t *)(*(volatile uint32_t *)(pcr + 0x100) + 0x73);
}

int main(void) {
    g_stage[1] = 3;
    HMODULE krnl = 0;
    void *p_load = 0;
    XexGetModuleHandle("xboxkrnl.exe", &krnl);
    XexGetProcedureAddress(krnl, ORD_XexLoadImage, &p_load);
    g_stage[6] = (uint32_t)p_load;
    g_stage[1] = 5;

    if (p_load) {
        HMODULE module = 0;
        volatile uint8_t *type = thread_proc_type();
        uint8_t old = *type;
        g_stage[7] = old;
        g_stage[3] = 0xEEEEEEEE;   // still there = XexLoadImage never returned
        g_stage[1] = 10;
        *type = PROC_SYSTEM;
        NTSTATUS status = ((XexLoadImage_t)p_load)(PLUGIN_PATH, XEX_LOADIMG_FLAG_DLL, 0, &module);
        *type = old;
        g_stage[3] = (uint32_t)status;
        g_stage[1] = 20;
    }

    /* hand off to Aurora so the console doesn't sit on this loader's black screen (the
     * plugin is a system DLL and stays resident after we exit). Done from here, in the
     * title's own context, rather than relying on an xbdm magicboot afterwards. If the
     * launch export can't be resolved, fall through to the idle loop (manual magicboot). */
    sleep_s(1); /* let the plugin's DllMain/thread get going first */
    {
        HMODULE xam = 0;
        void *p_launch = 0;

        XexGetModuleHandle("xam.xex", &xam);
        /* the Ex form, with game: mounted at Aurora's folder as xbdm's magicboot does:
         * plain XamLoaderLaunchTitle (420) left game: elsewhere and the console fell
         * back to the stock dashboard */
        XexGetProcedureAddress(xam, ORD_XamLoaderLaunchTitleEx, &p_launch);
        g_stage[4] = (uint32_t)p_launch;
        if (p_launch) {
            g_stage[1] = 30;
            g_stage[5] = (uint32_t)((XamLoaderLaunchTitleEx_t)p_launch)(AURORA_PATH, AURORA_DIR,
                                                                        0, 0);
        }
    }
    for (;;) {
        sleep_s(1);
        g_stage[2]++;
    }
}
