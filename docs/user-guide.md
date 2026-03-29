# User Guide

## What DFOS Is

DFOS is an experimental text-mode operating system project. Right now it is aimed at developers and testers, not general desktop use.

When DFOS boots successfully, you should see:

- Boot status messages
- Memory and paging information
- A note that the keyboard is active
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

## Basic Runtime Behavior

At boot DFOS:

- Initializes the screen
- Sets up memory management
- Starts interrupts and the system timer
- Starts a few background kernel tasks
- Loads files from the initrd

The worker tasks print heartbeat messages so you can tell the scheduler is alive.

## Keyboard And Debugger

The keyboard driver currently targets a PS/2 keyboard model in QEMU-compatible PC environments.

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

DFOS currently ships a small initialization ramdisk. The bundled sample files live in the [`initrd/`](/Users/n1le/Documents/Projects/DFOS/initrd) directory in the source tree and are available from the debugger after boot.

## Troubleshooting

### The build fails immediately

Check that:

- `i686-elf-gcc` is installed and in `PATH`
- `grub-mkrescue` is installed
- `qemu-system-i386` is installed

### The system boots but no keyboard input appears

Check that:

- QEMU is emulating a normal PC keyboard
- You are pressing `F1` inside the QEMU window, not your host terminal

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
