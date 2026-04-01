# Declare constants for the multiboot header.
.set ALIGN,    1<<0            
.set MEMINFO,  1<<1             
.set FLAGS,    ALIGN | MEMINFO  
.set MAGIC,    0x1BADB002      
.set CHECKSUM, -(MAGIC + FLAGS) 

# Declare a header as in the Multiboot Standard.
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

# Start the inital stack
.section .bootstrap_stack, "aw", @nobits
stack_bottom:
.skip 16384
stack_top:

# Preallocate pages. Do not hard code addresses as bootloader 
# could have potientially wrote over them
.section .bss, "aw", @nobits
	.align 4096
.global boot_multiboot_magic
boot_multiboot_magic:
	.skip 4
.global boot_multiboot_info
boot_multiboot_info:
	.skip 4
.global boot_page_directory
boot_page_directory:
	.skip 4096
.global boot_page_table1
boot_page_table1:
	.skip 4096
# We may need more if kernel > 3MiB

# Kernel entry point
.section .text
.global _start
.type _start, @function
_start:
	# GRUB leaves much of the CPU state unspecified. Keep maskable interrupts off
	# until the kernel installs its own IDT, and clear DF so C/string code behaves
	# according to the SysV ABI from the first function call onward.
	cli
	cld

	# Preserve the Multiboot handoff values until C initialization is ready.
	movl %eax, boot_multiboot_magic - 0xC0000000
	movl %ebx, boot_multiboot_info - 0xC0000000

	# Start from known-zero paging structures instead of relying on the bootloader
	# to have cleared BSS exactly the way our early paging code expects.
	movl $(boot_page_directory - 0xC0000000), %edi
	xorl %eax, %eax
	movl $1024, %ecx
	rep stosl

	movl $(boot_page_table1 - 0xC0000000), %edi
	movl $1024, %ecx
	rep stosl

	# Physical address of boot_page_table1
	movl $(boot_page_table1 - 0xC0000000), %edi
	# First address to map is 0
	movl $0, %esi
	# Map 1023 low-memory pages and leave the last entry for VGA text memory.
	# The higher-half kernel touches Multiboot structures and early modules via
	# physical-to-virtual translation before a fuller VM layout exists, so the
	# whole bootstrap window must stay present for now.
	movl $1023, %ecx

1:
	# Map physical addresses as present, writable.
	movl %esi, %edx
	orl $0x003, %edx
	movl %edx, (%edi)

	# Size of page is 4096 bytes
	addl $4096, %esi
	# Size of entries in boot_page_table1 is 4 bytes.
	addl $4, %edi
	# Loop to the next entry if we haven't finished
	loop 1b

	# Map VGA memory to 0xC03FF000 as present, writable
	movl $(0x000B8000 | 0x003), boot_page_table1 - 0xC0000000 + 1023 * 4

	# Map page table to 0x0... and 0xC0...
	# Entry 0 keeps the early identity map alive long enough to flip CR0 safely.
	# Entry 768 mirrors the same table into the higher-half kernel base at 0xC0000000.
	movl $(boot_page_table1 - 0xC0000000 + 0x003), boot_page_directory - 0xC0000000 + 0
	movl $(boot_page_table1 - 0xC0000000 + 0x003), boot_page_directory - 0xC0000000 + 768 * 4

	# Set cr3 to address of boot_page_direcotyr
	movl $(boot_page_directory - 0xC0000000), %ecx
	movl %ecx, %cr3

	# Enable paging and write-protect list
	movl %cr0, %ecx
	orl $0x80010000, %ecx
	movl %ecx, %cr0

	# Jump to higher half using absolute jump
	# The absolute jump forces EIP to be reloaded from the higher-half mapping.
	lea 4f, %ecx
	jmp *%ecx

4:
	# Paging is now setup fully and enabled
	# Set up the stack
	mov $stack_top, %esp

	# Restore the Multiboot values as C arguments.
	movl boot_multiboot_magic, %eax
	movl boot_multiboot_info, %ebx
	pushl %ebx
	pushl %eax

	call kernel_main

	cli
1:	hlt
	jmp 1b
