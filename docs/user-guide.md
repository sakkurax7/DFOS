# User Guide

## What DFOS Is

DFOS is an experimental text-mode operating system project. Right now it is aimed at developers and testers, not general desktop use.

When DFOS boots successfully, you should see:

- Boot status messages
- Memory and paging information
- The active console, input, timer, and IRQ controller names
- Periodic heartbeat lines from worker tasks

## Starting DFOS

From the project root:

```sh
make run
```

This will:

1. Install headers into the sysroot
2. Build the kernel and support library
3. Pack the initrd
4. Build a bootable ISO
5. Launch QEMU

Important:

- You need the expected cross compiler, GRUB tools, and QEMU installed first
- The project currently targets an `i686-elf` toolchain
- The bundled launcher forces QEMU to boot from the generated ISO first, which helps when firmware would otherwise fall through to another device

If you need a sticky debug session with a `qemu.log` trace, use:

```sh
make debug
```

## Basic Runtime Behavior

At boot DFOS:

- Registers the current platform drivers
- Initializes the active console
- Sets up memory management
- Initializes input, the IRQ controller, and the system timer
- Starts a few background kernel tasks
- Loads files from the initrd

The worker tasks print heartbeat messages so you can tell the scheduler is alive.

## Input And Debugger

The current platform input driver targets a PS/2 keyboard model in QEMU-compatible PC environments.

Press `F1` to enter the built-in kernel debugger.

Debugger commands:

- `help`: show available commands
- `tasks`: show kernel task state
- `mem`: show total and free memory
- `ls`: list files in the initrd
- `cat <file>`: print a file from the initrd
- `continue`: leave the debugger

Example session:

```text
[kdebug] ls
hello.txt
system/info.txt
[kdebug] cat hello.txt
```

## Files Available At Boot

DFOS ships a small initialization ramdisk. The bundled sample files live in [`../initrd/`](../initrd/) and are available from the debugger after boot.

## Troubleshooting

### The build fails immediately

Check that:

- `i686-elf-gcc` is installed and in `PATH`
- `grub-mkimage` is installed
- `xorriso` is installed
- `wget`, `apt-cache`, and `dpkg-deb` are installed if you want the build to download the GRUB `i386-pc` platform files locally
- `qemu-system-i386` is installed

### The system boots but no keyboard input appears

Check that:

- QEMU is emulating a normal PC keyboard
- You are pressing `F1` inside the QEMU window, not your host terminal

### QEMU skips the ISO and never reaches GRUB

Check that:

- You launched the VM through [`../scripts/qemu.sh`](../scripts/qemu.sh) or `make run`, not a stale custom command line
- The guest is using legacy PC BIOS boot for this image, not a UEFI-only firmware configuration
- `dfos.iso` was rebuilt successfully before launch
- The ISO build found or downloaded the GRUB `i386-pc` platform files and produced a BIOS El Torito image instead of a host-native UEFI-only image

### The guest resets immediately after selecting the GRUB menu entry

Try:

- `make boot-layout` to confirm the bootstrap paging symbols are aligned in the built kernel image
- `make debug` to capture `qemu.log` without an automatic reboot
- Reviewing the last exception sequence in `qemu.log` before changing the bootstrap code

### The system halts with a panic

Read the panic line on screen carefully. Current panic output is meant to include:

- The reason
- Exception or fault context when available

## Current Limits

DFOS is still early-stage software. Current limits include:

- No graphical interface
- No persistent disk filesystem
- No userspace programs
- No process isolation
- No login or shell environment

The current experience is best thought of as "boot the kernel, inspect it, and test subsystems."
