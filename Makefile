.PHONY: headers build iso run debug boot-layout clean check

headers:
	sh scripts/headers.sh

build: headers
	sh scripts/build.sh

iso: build
	sh scripts/iso.sh

run: iso
	sh scripts/qemu.sh

debug: iso
	sh scripts/qemu-debug.sh

boot-layout: build
	sh scripts/check-boot-layout.sh

clean:
	sh scripts/clean.sh

check:
	cc -target i386-unknown-elf -ffreestanding -fstack-protector-strong -fsyntax-only \
		-D__is_libc -D__is_libk -Ilibc/include -Ikernel/include \
		libc/stdio/printf.c libc/stdio/putchar.c libc/stdio/puts.c libc/stdlib/abort.c \
		libc/string/memcmp.c libc/string/memcpy.c libc/string/memmove.c libc/string/memset.c \
		libc/string/strlen.c libc/string/strcmp.c libc/string/strncmp.c
	cc -target i386-unknown-elf -ffreestanding -fstack-protector-strong -fsyntax-only \
		-Ikernel/include -Ilibc/include \
		kernel/kernel/console.c kernel/kernel/heap.c kernel/kernel/input.c \
		kernel/kernel/irq.c kernel/kernel/kdebug.c kernel/kernel/kernel.c \
		kernel/kernel/paging.c kernel/kernel/panic.c kernel/kernel/pmm.c \
		kernel/kernel/scheduler.c kernel/kernel/stack_protector.c \
		kernel/kernel/timer.c kernel/kernel/vfs.c \
		kernel/arch/i386/cpu.c kernel/arch/i386/gdt.c kernel/arch/i386/interrupts.c \
		kernel/arch/i386/pic.c kernel/arch/i386/pit_timer.c \
		kernel/arch/i386/platform.c kernel/arch/i386/ps2_keyboard.c \
		kernel/arch/i386/vga_console.c
