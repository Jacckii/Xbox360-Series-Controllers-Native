# Development

## Building

```bash
cd xbox360
OXC=/opt/openxechain/sysroot bash build.sh
```

`build.sh` builds:

| Output | What |
|---|---|
| `out/xa.xex` | The driver, a system DLL at `0x91E00000` |
| `out/xa_loader.xex` | A development loader: a title that loads the plugin without a reboot, then launches Aurora |
| `out/xa_m3.xex` | A minimal probe that claims the adapter and reads two registers (bring-up aid) |

Options: `XA_HOOKS=read` installs only the input hook (no capabilities or rumble hooks), for
isolating problems. `XA_SLOT=n` builds `xa<n>.xex` at its own base address, so a new build can
be loaded next to a running one.

Never build with `-g`: the toolchain's clang crashes emitting DWARF into COFF.

## Testing the core on Linux

`linux/` runs the same core on libusb, which is much easier to debug than the console. The
adapter must not be bound to xone:

```bash
sudo modprobe -r xone_gip_gamepad xone_dongle
make -C linux
sudo linux/xadapter /lib/firmware/xone_dongle_02fe.bin
```

It brings up the radio, pairs controllers and prints their input. `XA_DEBUG=1..3` adds frame
traces and raw dumps; `XA_NOSCAN=1` keeps the pairing channel fixed. `tools/readregs.c` reads a
few MT76 registers, for comparing what two hosts see.

`tests/test_sha256.c` checks the SHA-256/HMAC code against known answers:

```bash
cc -O2 -Wall -o test_sha256 tests/test_sha256.c core/sha256.c && ./test_sha256
```

## Working on the console

`xbox360/xa.py` talks to the console's debug monitor (xbdm, TCP 730). Set the console's
address with `XBOX=host[:port]`. Run it next to `out/`.

| Command | What |
|---|---|
| `xa.py upload [FIRMWARE]` | Copy the plugin, the loader and the firmware to `Usb0:\Apps\xadapter` |
| `xa.py boot on` / `off` | Add or remove the plugin as a `launch.ini` boot plugin (the old file is backed up first) |
| `xa.py load` | Start the development loader, which loads the plugin and launches Aurora |
| `xa.py status` | Driver state, log and controller states, once |
| `xa.py watch [SECONDS]` | Follow the log and controller states |
| `xa.py pair` | Open the pairing window |
| `xa.py unload` | Tear the driver down (hooks, endpoints, class driver) |
| `xa.py reload` | Experimental: build the next slot and hand the adapter over to it without a reboot |
| `xa.py reg ADDR [VALUE]` | Read or write an adapter register |
| `xa.py hold 0\|1`, `step` | Step through a USB removal one Usbd call at a time |

A loaded module's name stays taken until the console reboots, so after `unload` a new build of
the same name needs a reboot (or `XA_SLOT`).

The driver keeps a status block (magic `XA36`) and a log ring (magic `XLOG`) in its data
section; `xa.py` finds them through the build's PE file, so it must match the loaded build.

## Debugging tips

- **A stuck thread:** `getcontext` gives its registers; walk the stack's back chain from r1
  (SVR4 frames keep LR at +4 of the caller's frame, kernel frames at −8). Relinking with
  `-Wl,/map:` gives a map for the exact build (the output differs only in the PE timestamp).
- **Stopped or starved?** xbdm's `isstopped` tells a debugger stop from a thread that simply
  isn't scheduled. Briefly `suspend`ing other threads on the same hardware thread shows which
  one is taking the CPU.
- **Kernel ordinals** come from the 17559 kernel PE's export table (RVA + `0x80040000` is the
  live address), even though the PE's section table doesn't match the loaded image.
- **A hang right after loading** is usually a kernel callback without its thunk, floating-point
  code in a DPC, or data outside the image; `check_xex.py` catches the last two at build time.

## Code layout and conventions

- Kernel → plugin entry points are `XaXxx` thunks in `thunks.s`, with the C bodies named
  `XaXxxC` (`BODY`/`HOOK` attributes keep them from being inlined away).
- Usbd is only ever called on Usbd's DPC, through `xa_usbd_run` or the transfer helpers in
  `platform_360.c`.
- XAM is only ever called from the XAM thread in `xinput_360.c`.
- Kernel and XAM addresses that aren't exported are checked against known code before use.
