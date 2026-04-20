# DFOS TODO And Roadmap

This is the working checklist from current DFOS status to:

1. A usable userland with processes, syscalls, files, and a shell.
2. A self-hosting workflow where DFOS can build DFOS from its own command line.

## Definition Of Done

- [ ] DFOS boots to a userland `init` process.
- [ ] DFOS provides an interactive shell on a terminal.
- [ ] DFOS can run a native C toolchain in userland.
- [ ] DFOS can build the kernel, libc, initrd, and ISO from inside DFOS.
- [ ] The resulting image boots and passes core smoke tests.

## Phase 0: Foundations And Reliability

- [x] Add kernel locking primitives (spinlock, IRQ-safe lock, sleepable mutex).
- [x] Add wait queues / condition wait primitives.
- [x] Add kernel object lifetime model (reference counting where needed).
- [x] Add per-subsystem test harness hooks and uniform PASS/FAIL reporting.
- [x] Expand debugger tests to cover scheduler edge-cases and memory stress.
- [x] Add a persistent panic log or crash report capture path.

## Phase 1: Real Multiprocessing And Scheduling Maturity

- [ ] Bring up AP cores on i386 SMP path (LAPIC + startup IPIs).
- [ ] Implement real `cpu_current_id` from APIC/CPU-local state.
- [ ] Add inter-processor interrupts for reschedule and remote wakeup.
- [ ] Add periodic load balancing across per-CPU runqueues.
- [ ] Add scheduler-safe task migration between CPUs.
- [ ] Add NUMA-aware physical page allocation policy (not only CPU placement).
- [ ] Add scheduler tracing counters and latency stats for tuning.
- [ ] Add thread join/reap path to clean zombie tasks.

## Phase 2: User/Kernel Boundary

- [ ] Finalize ring-3 entry/return path (`iret` or `sysenter`/`sysexit` strategy).
- [ ] Define syscall ABI (register calling convention, error model, restart rules).
- [ ] Implement syscall dispatch table with versioned syscall numbers.
- [ ] Add secure user-pointer validation and safe copy helpers (`copy_from_user`, `copy_to_user`).
- [ ] Add per-process handle table and file descriptor abstraction.
- [ ] Add process credentials and ownership model (at least uid/gid placeholders).

## Phase 3: Process Model And Program Loading

- [ ] Implement process structure separate from threads.
- [ ] Add thread structure for user threads within a process.
- [ ] Implement `fork`-like or `posix_spawn`-like creation strategy.
- [ ] Implement `execve`-style image replacement.
- [ ] Implement `waitpid`-style child collection.
- [ ] Add signal delivery basics (at least terminate/interrupt/child).
- [ ] Add ELF loader for user executables.
- [ ] Add user stack setup with argv/envp/auxv.

## Phase 4: Virtual Memory Features Needed By Userland

- [ ] Implement user `mmap`/`munmap` and page protections.
- [ ] Implement anonymous memory mappings.
- [ ] Implement file-backed mappings.
- [ ] Implement demand paging for executable/text/data segments.
- [ ] Add copy-on-write support for process cloning.
- [ ] Implement `brk`/`sbrk` compatibility path (if needed by libc/toolchain).
- [ ] Add guard pages for user stacks.

## Phase 5: Filesystems, Storage, And Devices

- [ ] Add block device layer abstraction.
- [ ] Add disk driver path (ATA PIO first, AHCI later).
- [ ] Add writable filesystem support (start with simple FS, then stronger FS).
- [ ] Add mount table and VFS path resolution with directories/symlinks.
- [ ] Add buffered page/block cache.
- [ ] Add `/dev` device node model.
- [ ] Add pseudo-filesystems (`proc` or equivalent introspection surface).

## Phase 6: Userland Runtime And Base System

- [ ] Split libc into freestanding and hosted profiles.
- [ ] Implement hosted libc syscall wrappers.
- [ ] Add process startup runtime (`crt0`, dynamic/static startup model).
- [ ] Add dynamic memory allocator in userland libc.
- [ ] Implement `stdio`, `fcntl`, `unistd`, `dirent`, `signal`, and time APIs needed by tooling.
- [ ] Add `init` process that mounts filesystems and starts services.
- [ ] Add TTY subsystem and terminal line discipline.
- [ ] Add login/session model or single-user bootstrap session.

## Phase 7: Shell And Core Userland Utilities

- [ ] Implement a basic shell with command execution and PATH lookup.
- [ ] Add redirection (`>`, `<`, `2>`) and pipelines.
- [ ] Add job control basics (`&`, foreground/background) or explicitly defer.
- [ ] Add core utilities (`sh`, `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `echo`, `env`).
- [ ] Add process tools (`ps`, `kill`, `wait`, `time`).
- [ ] Add build helpers (`make`-compatible path or custom build runner).

## Phase 8: Native Toolchain Bring-Up

- [ ] Package/binutils support (`as`, `ld`, `ar`, `ranlib`) for DFOS target.
- [ ] Port a C compiler in stages (tiny compiler first, GCC/Clang later).
- [ ] Add hosted libc headers and ABI-compatible sysroot in-userland.
- [ ] Build and run a native hello-world compile/link/execute cycle inside DFOS.
- [ ] Add native archive and linker script flow for kernel/user binaries.
- [ ] Add reproducible build script entirely runnable inside DFOS shell.

## Phase 9: Self-Hosting DFOS Build

- [ ] Make source tree available inside DFOS filesystem.
- [ ] Port build scripts from host shell assumptions to DFOS shell/tooling.
- [ ] Ensure native tools can build libc and kernel objects.
- [ ] Ensure native tools can link `dfos.kernel`.
- [ ] Build initrd and ISO image from inside DFOS.
- [ ] Boot-test produced image in CI/emulator and compare with host-built image.
- [ ] Document the canonical self-hosting command sequence.

## Phase 10: Quality, Security, And Performance

- [ ] Add syscall fuzz tests and user/kernel boundary stress tests.
- [ ] Add filesystem crash-consistency tests.
- [ ] Add process and scheduler stress workloads.
- [ ] Add long-running soak tests for memory leaks and resource exhaustion.
- [ ] Add privilege checks and hardening review for all syscalls.
- [ ] Add performance baselines for compile throughput and context switch cost.

## Cross-Cutting Work Items

- [ ] Keep docs updated as each milestone lands.
- [ ] Keep debugger and automated tests aligned with current kernel capabilities.
- [ ] Add CI gates that run unit/smoke tests on every change.
- [ ] Track backward compatibility of kernel interfaces where practical.
- [ ] Maintain a "known limitations" section per milestone.
