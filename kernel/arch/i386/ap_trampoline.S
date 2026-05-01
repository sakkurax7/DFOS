.set AP_TRAMPOLINE_BASE, 0x7000

.code16

.global i386_ap_trampoline_start
.global i386_ap_trampoline_end
.global i386_ap_trampoline_gdt
.global i386_ap_trampoline_gdt_base
.global i386_ap_trampoline_cr0_value
.global i386_ap_trampoline_cr3_value
.global i386_ap_trampoline_cr4_value
.global i386_ap_trampoline_stack_top
.global i386_ap_trampoline_entry

i386_ap_trampoline_start:
	cli
	cld

	xorw %ax, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss

	lgdt AP_TRAMPOLINE_BASE + (i386_ap_trampoline_gdt_descriptor - i386_ap_trampoline_start)

	movl %cr0, %eax
	orl $0x1, %eax
	movl %eax, %cr0
	ljmpl $0x08, $AP_TRAMPOLINE_BASE + (i386_ap_trampoline_protected - i386_ap_trampoline_start)

.code32
i386_ap_trampoline_protected:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	movw %ax, %ss

	movl AP_TRAMPOLINE_BASE + (i386_ap_trampoline_cr4_value - i386_ap_trampoline_start), %eax
	movl %eax, %cr4
	movl AP_TRAMPOLINE_BASE + (i386_ap_trampoline_cr3_value - i386_ap_trampoline_start), %eax
	movl %eax, %cr3
	movl AP_TRAMPOLINE_BASE + (i386_ap_trampoline_cr0_value - i386_ap_trampoline_start), %eax
	movl %eax, %cr0

	movl AP_TRAMPOLINE_BASE + (i386_ap_trampoline_stack_top - i386_ap_trampoline_start), %esp
	movl AP_TRAMPOLINE_BASE + (i386_ap_trampoline_entry - i386_ap_trampoline_start), %eax
	jmp *%eax

.align 8
i386_ap_trampoline_gdt:
	.word 0x0000, 0x0000, 0x0000, 0x0000
	.word 0xFFFF, 0x0000, 0x9A00, 0x00CF
	.word 0xFFFF, 0x0000, 0x9200, 0x00CF
i386_ap_trampoline_gdt_end:

i386_ap_trampoline_gdt_descriptor:
	.word i386_ap_trampoline_gdt_end - i386_ap_trampoline_gdt - 1
i386_ap_trampoline_gdt_base:
	.long AP_TRAMPOLINE_BASE + (i386_ap_trampoline_gdt - i386_ap_trampoline_start)

.align 4
i386_ap_trampoline_cr0_value:
	.long 0
i386_ap_trampoline_cr3_value:
	.long 0
i386_ap_trampoline_cr4_value:
	.long 0
i386_ap_trampoline_stack_top:
	.long 0
i386_ap_trampoline_entry:
	.long 0

i386_ap_trampoline_end:
