ENTRY(_start)

SECTIONS
{

	. = 0xC0100000;
	_kernel_start = .;
	.text ALIGN (4K) : AT (ADDR (.text) - 0xC0000000)
	{
		_text_start = .;
		*(.multiboot)
		*(.text)
		_text_end = .;
	}
	.rodata ALIGN (4K) : AT (ADDR (.rodata) - 0xC0000000)
	{
		_rodata_start = .;
		*(.rodata)
		_rodata_end = .;
	}
	.data ALIGN (4K) : AT (ADDR (.data) - 0xC0000000)
	{
		*(.data)
	}
	.bootstrap_data ALIGN (4K) : AT (ADDR (.bootstrap_data) - 0xC0000000)
	{
		*(.bootstrap_data)
	}
	.bootstrap_paging ALIGN (4K) : AT (ADDR (.bootstrap_paging) - 0xC0000000)
	{
		*(.bootstrap_paging)
	}
	.bss ALIGN (4K) : AT (ADDR (.bss) - 0xC0000000)
	{
		*(COMMON)
		*(.bss)
	}
	.bootstrap_stack ALIGN (16) : AT (ADDR (.bootstrap_stack) - 0xC0000000)
	{
		*(.bootstrap_stack)
	}
	_kernel_end = .;
}
