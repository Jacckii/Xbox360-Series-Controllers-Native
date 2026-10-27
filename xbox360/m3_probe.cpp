// SPDX-License-Identifier: GPL-3.0-or-later
//
// xa_m3: first contact between an Xbox 360 (kernel 17559) and the Xbox Wireless Adapter.
// A system DLL that claims the adapter through the kernel's runtime USB class-driver
// list, opens its control endpoint and reads two MT76 registers. Every step lands in
// g_status (magic "XAM3"), read over xbdm.
//
// Expected with a working setup: ASIC_VERSION 0x76320044 (chip ID); FCE_DMA_ADDR 0 on a
// cold adapter (no firmware yet) or 0x80000001 with firmware running.
//
// USB transfers follow the kernel's Usbd API as used by hiddriver360 (EinTim23, GPL-3.0):
// UsbdOpenDefaultEndpoint fills a transfer block, UsbdQueueAsyncTransfer queues it, and
// the completion callback gets (transfer block, status) on Usbd's DPC.
//
// Build: see build.sh (OpenXeChain, system DLL at 0x91D00000). Every function the kernel
// calls goes through NV64_THUNK: the toolchain saves r14-r31 as 32-bit values, the
// kernel keeps 64-bit values there.

typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned long  DWORD;   // 32-bit on ppc32-xbox360
typedef long           LONG;
typedef int            BOOL;
typedef void          *HANDLE;

#define TRUE 1
#define DLL_PROCESS_ATTACH 1

extern "C" {
    long XexGetModuleHandle(const char *name, HANDLE *module);
    long XexGetProcedureAddress(HANDLE module, DWORD ordinal, void *address);
    unsigned long DbgPrint(const char *fmt, ...);
}

// ---- kernel -> plugin callbacks ------------------------------------------------------
__asm__(R"(
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
    NV64_THUNK XaControlDone, XaControlDoneC
)");

struct KDPC360;
struct UsbdClassDriver;

extern "C" {
    void XaLinkDpc(KDPC360 *dpc, void *context, void *arg1, void *arg2);
    DWORD XaMatch(const BYTE *ifdesc, const BYTE *devdesc, UsbdClassDriver *self);
    void XaAddDevice(void *node);
    void XaRemoveDevice(void *node);
    long XaControlDone(void *transfer, long status);
}
#define NV64_BODY extern "C" __attribute__((used, noinline))

// ---- kernel 17559 ----------------------------------------------------------------------
#define K_DRIVER_LIST      0x801A8048   // LIST_ENTRY head walked by UsbdSelectClassDriver
#define K_USBD_TIMER_DPC   0x801721D0   // Usbd's own DPC: byte 3 = target processor + 1
#define K_WGC_DRIVER       0x80172598   // the console's own wireless (FF/5D/81) driver entry

#define ORD_KeInitializeDpc              0x06F
#define ORD_KeInsertQueueDpc             0x07B
#define ORD_UsbdAddDeviceComplete        740
#define ORD_UsbdOpenDefaultEndpoint      746
#define ORD_UsbdQueueAsyncTransfer       748
#define ORD_UsbdRemoveDeviceComplete     751

#define ADAPTER_VID 0x045E
#define ADAPTER_PID 0x02FE

struct KDPC360 {
    WORD  Type;
    BYTE  Inserted;
    BYTE  TargetProcessor;   // processor + 1, 0 = current
    void *Flink, *Blink;
    void (*Routine)(KDPC360 *dpc, void *context, void *arg1, void *arg2);
    void *Context, *Arg1, *Arg2;
};

struct UsbdClassDriver {
    UsbdClassDriver *Flink, *Blink;
    DWORD Flags;
    void (*AddDevice)(void *node);
    void (*RemoveDevice)(void *node);
    DWORD (*Match)(const BYTE *ifdesc, const BYTE *devdesc, UsbdClassDriver *self);
    void (*AfterAdd)(void *node, DWORD one, void *ctx);
};

// transfer block filled by UsbdOpen*Endpoint and queued with UsbdQueueAsyncTransfer
struct UsbTransfer {
    DWORD endpoint;          // set by UsbdOpen*Endpoint
    void *callback;          // (transfer, status) on completion
    DWORD saved_endpoint;    // copy of endpoint, set before each queue
    BYTE  reserved[4];
    BYTE  flags;             // 1 for every transfer
    BYTE  controller_index;  // written by UsbdQueueAsyncTransfer
    BYTE  reserved2;
    BYTE  endpoint_index;    // written by UsbdQueueAsyncTransfer
    void *buffer;
    DWORD length;
};

struct UsbControlTransfer {
    UsbTransfer t;
    BYTE  reserved[4];
    BYTE  bmRequestType;
    BYTE  bRequest;
    WORD  wValue;            // little-endian on the wire: stored byte-swapped
    WORD  wIndex;
    WORD  wLength;
};

static void (*pKeInitializeDpc)(KDPC360 *, void *routine, void *context);
static DWORD (*pKeInsertQueueDpc)(KDPC360 *, void *arg1, void *arg2);
static void (*pUsbdAddDeviceComplete)(void *node, long status);
static long (*pUsbdOpenDefaultEndpoint)(void *node, void *transfer);
static long (*pUsbdQueueAsyncTransfer)(void *node, void *transfer);
static void (*pUsbdRemoveDeviceComplete)(void *node);

static UsbdClassDriver g_driver;
static KDPC360 g_link_dpc;
static UsbControlTransfer g_ctrl __attribute__((aligned(16)));
static BYTE g_data[16] __attribute__((aligned(16)));
static void *g_node;

// Progress for xbdm, found by the magic. Initialised, so .data isn't empty (SynthXEX).
enum { ST_STARTED = 1, ST_RESOLVE_FAILED = 2, ST_CODE_MISMATCH = 3, ST_LINKED = 4,
       ST_ADDED = 5, ST_ENDPOINT_OPEN = 6, ST_ASIC_QUEUED = 7, ST_ASIC_DONE = 8,
       ST_FCE_QUEUED = 9, ST_DONE = 10, ST_REMOVED = 11, ST_FAILED = 12 };
enum { S_MAGIC, S_STATE, S_NODE, S_OPEN_STATUS, S_QUEUE_STATUS, S_ASIC_STATUS, S_ASIC,
       S_FCE_STATUS, S_FCE, S_ADDS, S_REMOVES, S_MATCH_CALLS, S_LAST_IDS, S_COUNT };
__attribute__((used)) static volatile DWORD g_status[S_COUNT] = {0x58414D33 /* XAM3 */};

static WORD swap16(WORD v) { return (WORD)(v << 8 | v >> 8); }
static DWORD le32(const BYTE *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }

// vendor IN control transfer of 4 bytes (an MT76 register read)
static long queue_register_read(BYTE request, DWORD addr)
{
    g_ctrl.bmRequestType = 0xC0;              // device to host, vendor, device
    g_ctrl.bRequest = request;
    g_ctrl.wValue = swap16((WORD)(addr >> 16));
    g_ctrl.wIndex = swap16((WORD)addr);
    g_ctrl.wLength = swap16(4);
    g_ctrl.t.buffer = g_data;
    g_ctrl.t.length = 4;
    g_ctrl.t.flags = 1;
    g_ctrl.t.callback = (void *)XaControlDone;
    g_ctrl.t.saved_endpoint = g_ctrl.t.endpoint;
    return pUsbdQueueAsyncTransfer(g_node, &g_ctrl);
}

NV64_BODY long XaControlDoneC(void *transfer, long status)
{
    if (g_status[S_STATE] == ST_ASIC_QUEUED) {
        g_status[S_ASIC_STATUS] = status;
        g_status[S_ASIC] = le32(g_data);
        g_status[S_STATE] = ST_ASIC_DONE;
        if (status >= 0) {
            // FCE_DMA_ADDR in config space (vendor request 0x47): non-zero = firmware runs
            g_status[S_STATE] = ST_FCE_QUEUED;
            long err = queue_register_read(0x47, 0x0230);
            if (err < 0) {
                g_status[S_QUEUE_STATUS] = err;
                g_status[S_STATE] = ST_FAILED;
            }
        }
    } else if (g_status[S_STATE] == ST_FCE_QUEUED) {
        g_status[S_FCE_STATUS] = status;
        g_status[S_FCE] = le32(g_data);
        g_status[S_STATE] = ST_DONE;
        DbgPrint("xa_m3: ASIC %08X, FCE_DMA_ADDR %08X\n", g_status[S_ASIC], g_status[S_FCE]);
    }
    return 0;
}

NV64_BODY DWORD XaMatchC(const BYTE *ifdesc, const BYTE *devdesc, UsbdClassDriver *self)
{
    WORD vid = devdesc[8] | devdesc[9] << 8, pid = devdesc[10] | devdesc[11] << 8;

    g_status[S_MATCH_CALLS]++;
    g_status[S_LAST_IDS] = (DWORD)vid << 16 | pid;
    return vid == ADAPTER_VID && pid == ADAPTER_PID;
}

NV64_BODY void XaAddDeviceC(void *node)
{
    long status;

    g_node = node;
    g_status[S_ADDS]++;
    g_status[S_NODE] = (DWORD)node;
    g_status[S_STATE] = ST_ADDED;
    pUsbdAddDeviceComplete(node, 0);

    status = pUsbdOpenDefaultEndpoint(node, &g_ctrl);
    g_status[S_OPEN_STATUS] = status;
    if (status < 0) {
        g_status[S_STATE] = ST_FAILED;
        return;
    }
    g_status[S_STATE] = ST_ENDPOINT_OPEN;

    // ASIC_VERSION, vendor request 0x07 (multi read)
    g_status[S_STATE] = ST_ASIC_QUEUED;
    status = queue_register_read(0x07, 0x0000);
    g_status[S_QUEUE_STATUS] = status;
    if (status < 0)
        g_status[S_STATE] = ST_FAILED;
}

NV64_BODY void XaRemoveDeviceC(void *node)
{
    g_status[S_REMOVES]++;
    g_status[S_STATE] = ST_REMOVED;
    if (node == g_node)
        g_node = 0;
    pUsbdRemoveDeviceComplete(node);
}

NV64_BODY void XaLinkDpcC(KDPC360 *dpc, void *context, void *arg1, void *arg2)
{
    UsbdClassDriver *head = (UsbdClassDriver *)K_DRIVER_LIST;

    g_driver.Flink = head->Flink;
    g_driver.Blink = head;
    __asm__ __volatile__("lwsync" ::: "memory");   // entry complete before Usbd sees it
    head->Flink->Blink = &g_driver;
    head->Flink = &g_driver;
    g_status[S_STATE] = ST_LINKED;
    DbgPrint("xa_m3: waiting for the Xbox Wireless Adapter\n");
}

static int resolve(HANDLE krnl, DWORD ordinal, void *out)
{
    void *p = 0;

    XexGetProcedureAddress(krnl, ordinal, &p);
    if (!p) {
        g_status[S_STATE] = ST_RESOLVE_FAILED;
        return 0;
    }
    *(void **)out = p;
    return 1;
}

static int check_code(DWORD addr, DWORD expected)
{
    if (*(volatile DWORD *)addr == expected)
        return 1;
    g_status[S_STATE] = ST_CODE_MISMATCH;
    return 0;
}

static void start(void)
{
    HANDLE krnl;

    g_status[S_STATE] = ST_STARTED;
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0)
        return;
    if (!resolve(krnl, ORD_KeInitializeDpc, &pKeInitializeDpc) ||
        !resolve(krnl, ORD_KeInsertQueueDpc, &pKeInsertQueueDpc) ||
        !resolve(krnl, ORD_UsbdAddDeviceComplete, &pUsbdAddDeviceComplete) ||
        !resolve(krnl, ORD_UsbdOpenDefaultEndpoint, &pUsbdOpenDefaultEndpoint) ||
        !resolve(krnl, ORD_UsbdQueueAsyncTransfer, &pUsbdQueueAsyncTransfer) ||
        !resolve(krnl, ORD_UsbdRemoveDeviceComplete, &pUsbdRemoveDeviceComplete))
        return;

    // the class-driver list, the wireless driver entry and Usbd's DPC as kernel 17559 has them
    if (!check_code(0x800D6184, 0x3D60801B) || !check_code(0x800D618C, 0x3BCB8048) ||
        !check_code(K_WGC_DRIVER + 0x0C, 0x800F3400) ||
        (*(volatile WORD *)K_USBD_TIMER_DPC) != 0x0013) {
        g_status[S_STATE] = ST_CODE_MISMATCH;
        return;
    }

    // flags as the console's own wireless driver has them (interface-level driver)
    g_driver.Flags = *(volatile DWORD *)(K_WGC_DRIVER + 0x08);
    g_driver.AddDevice = XaAddDevice;
    g_driver.RemoveDevice = XaRemoveDevice;
    g_driver.Match = XaMatch;
    g_driver.AfterAdd = 0;

    // link on Usbd's processor, serialised with Usbd itself
    pKeInitializeDpc(&g_link_dpc, (void *)XaLinkDpc, 0);
    g_link_dpc.TargetProcessor = *(volatile BYTE *)(K_USBD_TIMER_DPC + 3);
    pKeInsertQueueDpc(&g_link_dpc, 0, 0);
}

NV64_BODY int DllMainC(unsigned int handle, unsigned int reason, unsigned int reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        start();
    return TRUE;
}
