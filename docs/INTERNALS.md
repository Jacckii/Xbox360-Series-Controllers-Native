# Internals

How the driver works, and the Xbox 360 (kernel 17559) behaviour it depends on. Addresses are
for retail kernel 17559 and its XAM; ordinals are from the kernel's own export table.

## Layout

| Path | What |
|---|---|
| `core/` | Portable driver: MT76 chip (`mt76.c`), adapter and 802.11 (`dongle.c`), GIP protocol, security handshake and gamepad (`gip.c`), SHA-256/HMAC (`sha256.c`). Talks to the host only through `platform.h`. |
| `xbox360/` | The 360 plugin: Usbd backend (`platform_360.c`), class driver and driver thread (`xa_plugin.c`), XInput presentation (`xinput_360.c`), function hooks (`detour.c`), 64-bit register thunks (`thunks.s`). |
| `linux/` | The same core on libusb (`xadapter`), for testing on a PC or Raspberry Pi. |
| `third_party/micro-ecc/` | ECDH P-256 for the handshake. |

The core is a port of xone (`transport/mt76.c`, `transport/dongle.c`, `bus/`, `auth/`,
`driver/gamepad.c`). Linux infrastructure (URBs, skbs, workqueues, the kernel crypto API) is
replaced by a small platform layer: control and bulk transfers with completion callbacks, a
receive queue, a clock, allocation, logging.

## Adapter

One interface, class/subclass/protocol FF/FF/FF, USB 2.0 high speed: bulk IN 0x84 (commands)
and 0x85 (radio frames), bulk OUT 0x04–0x09, 512-byte packets.

Start-up: the firmware (≈70 KB) is uploaded with vendor control requests and bulk OUT; on a
warm adapter the running firmware is reset instead. Then the radio is initialised (calibration,
channels, MAC address, beacon) and pairing is switched on for 60 seconds. The adapter's pair
button (`XA_MT_EVT_BUTTON`) reopens pairing.

Controllers: 802.11 association, then GIP. The v2 security handshake uses ECDH P-256, a
transcript hash and an HMAC-SHA256 PRF to derive each controller's key; the key goes to the
adapter, which encrypts the radio link in hardware. Input reports, rumble and keep-alives are
GIP messages inside 802.11 QoS-data frames.

## USB on the 360

- **Claiming the adapter.** A class-driver entry linked into the kernel's runtime driver list
  (`0x801A8048`, walked by `UsbdSelectClassDriver`) matches 045e:02fe. No hooks are involved.
  The link is done in a DPC on Usbd's processor.
- **Transfer API** (hiddriver360 documents the same calls): 740 `UsbdAddDeviceComplete`,
  741 `UsbdCancelAsyncTransfer`, 746 `UsbdOpenDefaultEndpoint`, 747 `UsbdOpenEndpoint`,
  748 `UsbdQueueAsyncTransfer`, 749/750 close default/other endpoint,
  751 `UsbdRemoveDeviceComplete`, 758 `UsbdResetDevice`, 759 `UsbdGetDeviceDescriptor`.
  Queue, close and cancel return nothing; the outcome arrives through the transfer block's
  callback `(transfer, status)` on Usbd's DPC (a cancel completes with `0xC0000120`).
- **Usbd takes no locks.** `UsbdQueueAsyncTransfer` edits an unlocked qTD free list, and the
  kernel's own drivers only call Usbd from its DPC and timers. The driver thread therefore never
  calls Usbd: it posts requests to a ring that a DPC on Usbd's processor executes, and every
  transfer completion drains the ring too, so requests keep flowing when that processor is busy.
- **Outgoing traffic** is a FIFO with one transfer in flight, chained from each completion.
  The thread only appends and never waits, and protocol packets are never dropped.
- **Incoming traffic:** each IN completion copies its frame into a 128 KB ring and re-queues
  the transfer immediately, as xone resubmits its URBs; the driver thread drains the ring.
- **An IN buffer must fit one EHCI transfer descriptor** (≤ 0x5000 bytes from its page start).
  Usbd splits larger buffers without an alternate-next pointer, and after a short packet its
  bookkeeping and the controller disagree. IN buffers are page-aligned and sized accordingly.
- **A control transfer without data gets a NULL buffer.** Usbd adds a data stage whenever the
  pointer is set, even for zero bytes, and the device never completes such a request.
- Plain image memory works as a transfer buffer; Usbd translates each page itself.

## Picking up an adapter that's already plugged in

A device that no class driver claims is completed with an error by the kernel's add-device
path, and its hub port is marked in the unrecognized-port table (`0x801A7FA0`,
`{hub node, port bitmask}` × 20). `UsbdResetDevice` doesn't offer it to drivers again.

`UsbdTitleDriverResetAllUnrecognizedPorts` (ordinal 890) does: it re-queries unrecognized
devices and re-enumerates every marked port, at which point the linked class driver is asked
again. It takes a caller-owned context (≈0xF40 bytes, with a KEVENT at +0x1C that is set when
the work finishes), queues transfers and timers, and therefore runs on Usbd's DPC.

The plugin calls it once, after the title that was running when it loaded has been replaced
(the boot launching the dashboard, or the development loader handing over) and the new title
has been stable for 8 seconds: a title switch resets unrecognized ports itself, and the two must
not overlap. `UsbdGetDeviceDescriptor` can't identify a parked device (it returns the
descriptor of whichever device the controller enumerated last), so the rescan runs whenever the
table is non-empty; other parked devices simply stay unclaimed.

## Presenting controllers to the system

This follows hiddriver360's route:

- Each controller is bound as a virtual device with `XamUserBindDeviceCallback`
  (`0x816D9060`, not exported; its prologue is checked before use), which assigns a user index.
- Three functions are hooked: `XInputdReadState` (kernel 486) serves the controller state,
  `XamInputGetCapabilitiesEx` (xam 685) reports a standard gamepad, and `XamInputSetState`
  (xam 402) forwards rumble to the controller.
- The Guide button goes through XAM's registered Guide callback (the entry next to
  `XamUserBindDeviceCallback` in the kernel's table at `0x80189EA0`), the way the console's
  own wireless driver reports it. It fires on release after a short press.
- Every XAM call (bind, unbind, Guide, notifications) runs on a dedicated thread, never on the
  driver thread.

The hooks (`detour.c`) replace a function's first four instructions with a far branch and run
the originals from a trampoline. Each trampoline builds its branch in a scratch register that
the relocated instructions don't use, decoded per opcode: the XAM functions start with
`mflr r12; bl __savegprlr`, and `__savegprlr` stores r12 as the return address. While a hook is
written, callers are parked on a `b .` instruction.

Notifications use `XNotifyQueueUI` (xam 656). Its third parameter is a 64-bit value; it's
declared 32-bit so that the text and context land in r6 and r7, as XAM expects.

## Threads and priorities

The driver thread is a system thread pinned to hardware thread 2 (the affinity mask is the top
byte of `ExCreateThread`'s flags; without one, a system thread created from a DLL's entry point
is never scheduled). Hardware thread 2 also runs Usbd's DPCs and a XAM worker thread at
priority 6, which is busy while the guide is open. At the default priority of 5 the driver
thread would starve whenever the guide is open, so the thread raises itself to 9 with
`KeSetBasePriorityThread` (ordinal 153); 9 is the maximum for the system process. Its work is
short bursts between 2 ms sleeps, and at most 64 received frames are handled per pass.

## Toolchain constraints

OpenXeChain's clang targets the 32-bit PowerPC SVR4 ABI, while the kernel is built for the
360's 64-bit-register ABI. Three rules follow:

- **Every function the kernel calls** (DllMain, DPCs, thread starts, transfer completions,
  driver callbacks, hooks) goes through a thunk (`thunks.s`) that saves r14–r31 as 64-bit values.
- **No kernel function that takes or returns a 64-bit value by value** is called directly: such
  a value would be passed in a register pair instead of one register.
- **No floating point.** DPCs have no FP context, and the ABI's variadic prologue saves f1–f8
  unconditionally, so a hard-float `printf`-style function faults in a DPC. The plugin builds
  with `-msoft-float`.

SynthXEX maps only the bytes the PE file contains, so zero-initialised data would fall outside
the loaded image; the plugin builds with `-fno-zero-initialized-in-bss`. `check_xex.py` enforces
both of the last two points.
