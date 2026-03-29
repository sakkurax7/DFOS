# Application Developer Guide

## Current Reality

DFOS does not yet provide a traditional user-space process model. There is no:

- User-mode execution
- Syscall layer
- ELF loader
- Process isolation
- Stable userspace ABI

So today, "application development" on DFOS means one of two things:

1. Writing code that runs as a kernel thread inside the OS
2. Shipping data or scripts in the initrd for the kernel and debugger to consume

This guide explains what is currently possible and how to prepare for the future system model.

## What You Can Build Today

### Kernel-Resident Services

The easiest way to prototype application logic today is to add a kernel task with:

- `scheduler_create_kernel_task`
- `printf`
- `scheduler_sleep`
- `vfs_open` / `vfs_read`

See the demo workers in [kernel.c](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/kernel.c).

Good early prototypes:

- Background daemons
- File readers
- Input-driven tools
- Debug/test harnesses

### Initrd Content

The initrd is built from the [`initrd/`](/Users/n1le/Documents/Projects/DFOS/initrd) directory and packed into `initrd.tar`.

You can add:

- Text files
- Configuration files
- Data tables
- Assets consumed by future services

At runtime the debugger can inspect them with:

- `ls`
- `cat <file>`

## Available Runtime Facilities

Current developer-usable facilities inside DFOS:

- Console output through `printf`
- Tick-based sleeping through `scheduler_sleep`
- Access to initrd-backed files through the VFS
- Keyboard-driven debugger entry with `F1`

Current caveats:

- There is no input API beyond the kernel keyboard buffer
- There is no dynamic module loader
- There is no userspace allocator or hosted libc

## Writing A Kernel Task

Pattern:

1. Add a function `static void my_task(void* arg)`
2. Put your task logic in a loop
3. Sleep or yield instead of spinning forever
4. Register it from `kernel_main`

Example shape:

```c
static void my_task(void* arg) {
	(void) arg;

	while (true) {
		printf("my task tick=%u\n", scheduler_ticks());
		scheduler_sleep(10);
	}
}
```

Important:

- Returning from a task exits it
- Tasks share the whole kernel address space
- Bugs in task code can crash the entire OS

## File Access Model

Files currently come from the first Multiboot module and are indexed at boot.

Use:

- `vfs_open(path, &node)`
- `vfs_read(&node, offset, buffer, size)`

Paths are currently simple strings matching the tar entry names, for example:

- `hello.txt`
- `system/info.txt`

## How To Prepare For Future Userland

If you want your code to be easy to migrate once DFOS has processes and syscalls:

- Keep logic separate from kernel-specific glue
- Avoid direct dependence on hardware headers
- Treat `printf`, sleep, and file reads as replaceable interfaces
- Keep application state self-contained instead of global

That way, later migration to:

- syscalls
- a user-mode libc
- an executable loader

will be much easier.

## What Is Missing Before Real Apps Exist

The main milestones before DFOS can host true applications are:

1. User mode segments / privilege transitions
2. Syscall entry and ABI design
3. Process and address-space management
4. Executable loading
5. A hosted libc and standard runtime startup
