#!/bin/sh
set -eu

KERNEL_IMAGE=${1:-sysroot/boot/dfos.kernel}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required tool '$1' was not found in PATH" >&2
    exit 1
  fi
}

symbol_address() {
  objdump -t "$KERNEL_IMAGE" | awk -v symbol="$1" '$NF == symbol { print $1; exit }'
}

require_command objdump

if [ ! -f "$KERNEL_IMAGE" ]; then
  echo "error: kernel image '$KERNEL_IMAGE' was not found" >&2
  exit 1
fi

BOOT_PAGE_DIRECTORY_HEX=$(symbol_address boot_page_directory)
BOOT_PAGE_TABLE1_HEX=$(symbol_address boot_page_table1)
BOOT_MULTIBOOT_MAGIC_HEX=$(symbol_address boot_multiboot_magic)
BOOT_MULTIBOOT_INFO_HEX=$(symbol_address boot_multiboot_info)

if [ -z "$BOOT_PAGE_DIRECTORY_HEX" ] || [ -z "$BOOT_PAGE_TABLE1_HEX" ]; then
  echo "error: bootstrap paging symbols were not found in '$KERNEL_IMAGE'" >&2
  exit 1
fi

BOOT_PAGE_DIRECTORY=$((0x$BOOT_PAGE_DIRECTORY_HEX))
BOOT_PAGE_TABLE1=$((0x$BOOT_PAGE_TABLE1_HEX))
BOOT_MULTIBOOT_MAGIC=$((0x${BOOT_MULTIBOOT_MAGIC_HEX:-0}))
BOOT_MULTIBOOT_INFO=$((0x${BOOT_MULTIBOOT_INFO_HEX:-0}))

printf 'boot_multiboot_magic  0x%08x\n' "$BOOT_MULTIBOOT_MAGIC"
printf 'boot_multiboot_info   0x%08x\n' "$BOOT_MULTIBOOT_INFO"
printf 'boot_page_directory   0x%08x\n' "$BOOT_PAGE_DIRECTORY"
printf 'boot_page_table1      0x%08x\n' "$BOOT_PAGE_TABLE1"

if [ $((BOOT_PAGE_DIRECTORY % 4096)) -ne 0 ]; then
  echo "error: boot_page_directory is not 4 KiB aligned" >&2
  exit 1
fi

if [ $((BOOT_PAGE_TABLE1 % 4096)) -ne 0 ]; then
  echo "error: boot_page_table1 is not 4 KiB aligned" >&2
  exit 1
fi

if [ "$BOOT_PAGE_TABLE1" -lt "$BOOT_PAGE_DIRECTORY" ]; then
  echo "error: boot_page_table1 unexpectedly precedes boot_page_directory" >&2
  exit 1
fi

echo "bootstrap paging layout looks aligned"
