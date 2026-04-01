.PHONY: headers build iso run debug boot-layout clean check

headers:
	sh headers.sh

build: headers
	sh build.sh

iso: build
	sh iso.sh

run: iso
	sh qemu.sh

debug: iso
	sh qemu-debug.sh

boot-layout: build
	sh check-boot-layout.sh

clean:
	sh clean.sh

check:
	cc -target i386-unknown-elf -ffreestanding -fstack-protector-strong -fsyntax-only \
		-D__is_libc -D__is_libk -Ilibc/include -Ikernel/include \
		libc/stdio/printf.c libc/stdio/putchar.c libc/stdio/puts.c libc/stdlib/abort.c \
		libc/string/memcmp.c libc/string/memcpy.c libc/string/memmove.c libc/string/memset.c \
		libc/string/strlen.c libc/string/strcmp.c libc/string/strncmp.c
	cc -target i386-unknown-elf -ffreestanding -fstack-protector-strong -fsyntax-only \
		-Ikernel/include -Ilibc/include \
		kernel/kernel/kernel.c kernel/kernel/paging.c kernel/kernel/panic.c \
		kernel/kernel/pmm.c kernel/kernel/heap.c kernel/kernel/scheduler.c \
		kernel/kernel/stack_protector.c kernel/kernel/vfs.c kernel/kernel/kdebug.c \
		kernel/arch/i386/gdt.c kernel/arch/i386/interrupts.c kernel/arch/i386/pit.c \
		kernel/arch/i386/tty.c kernel/arch/i386/keyboard.c
