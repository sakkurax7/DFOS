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
boot_page_directory:
	.skip 4096
boot_page_table1:
	.skip 4096
# We may need more if kernel > 3MiB

# Kernel entry point
.section .text
.global _start
.type _start, @function
_start:
	# Physical address of boot_page_table1
	movl $(boot_page_table1 - 0xC0000000), %edi
	# First address to map is 0
	movl $0, %esi
	# Map 1023 pages, with 1024 being VGA text buffer
	movl $1023, %ecx

1:
	# Only map kernel
	cmpl $(_kernel_start - 0xC0000000), %esi
	jl 2f
	cmpl $(_kernel_end - 0xC0000000), %esi
	jge 3f

	# Map physical addresses as present, writable.
	# We need to map .text and .rodata as non-writable
	movl %esi, %edx
	orl $0x003, %edx
	movl %edx, (%edi)

2:
	# Size of page is 4096 bytes
	addl $4096, %esi
	# Size of entries in boot_page_table1 is 4 bytes.
	addl $4, %edi
	# Loop to the next entry if we haven't finished
	loop 1b

3:
	# Map VGA memory to 0xC03FF000 as present, writable
	movl $(0x000B8000 | 0x003), boot_page_table1 - 0xC0000000 + 1023 * 4

	# Map page table to 0x0... and 0xC0...
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
	lea 4f, %ecx
	jmp *%ecx

4:
	# Paging is now setup fully and enabled
	# Unmap identity mapping
	movl $0, boot_page_directory + 0
	
	# Reload crc3 to force a TLB flush
	movl %cr3, %ecx
	movl %ecx, %cr3

	# Set up the stack
	mov $stack_top, %esp

	call kernel_main

	cli
1:	hlt
	jmp 1b