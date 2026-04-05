.global i386_pae_trampoline
.type i386_pae_trampoline, @function
i386_pae_trampoline:
	# cdecl arguments:
	#   [esp + 4]  = pdpt physical address
	#   [esp + 8]  = low identity-mapped stack pointer
	mov 4(%esp), %eax
	mov 8(%esp), %ecx
	# Preserve the real call-entry stack (with return address + args) so `ret`
	# unwinds correctly after paging mode changes.
	mov %esp, %edx

	# Switch to an identity-mapped stack before disabling paging.
	mov %ecx, %esp

	# Disable paging (CR0.PG=0) so CR4.PAE can be toggled safely.
	mov %cr0, %ecx
	andl $0x7FFFFFFF, %ecx
	mov %ecx, %cr0

	# Enable PAE and load the PAE top-level pointer.
	mov %cr4, %ecx
	orl $0x20, %ecx
	mov %ecx, %cr4
	mov %eax, %cr3

	# Re-enable paging using the PAE hierarchy.
	mov %cr0, %ecx
	orl $0x80000000, %ecx
	mov %ecx, %cr0

	# Return on the original call-entry stack.
	mov %edx, %esp
	ret
