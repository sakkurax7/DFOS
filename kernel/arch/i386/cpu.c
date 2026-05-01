#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/cpu.h>
#include <kernel/gdt.h>
#include <kernel/interrupts.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/x86.h>

#define IA32_APIC_BASE_MSR       0x1Bu
#define IA32_APIC_BASE_ENABLE    (1ull << 11)
#define IA32_APIC_BASE_ADDR_MASK 0xFFFFF000u

#define LAPIC_REG_ID             0x020u
#define LAPIC_REG_TPR            0x080u
#define LAPIC_REG_EOI            0x0B0u
#define LAPIC_REG_SVR            0x0F0u
#define LAPIC_REG_ICR_LOW        0x300u
#define LAPIC_REG_ICR_HIGH       0x310u

#define LAPIC_SVR_ENABLE         (1u << 8)
#define LAPIC_SPURIOUS_VECTOR    0xFFu

#define LAPIC_ICR_DELIVERY_FIXED   (0u << 8)
#define LAPIC_ICR_DELIVERY_INIT    (5u << 8)
#define LAPIC_ICR_DELIVERY_STARTUP (6u << 8)
#define LAPIC_ICR_LEVEL_ASSERT     (1u << 14)
#define LAPIC_ICR_TRIGGER_LEVEL    (1u << 15)
#define LAPIC_ICR_STATUS_PENDING   (1u << 12)

#define CPU_IPI_RESCHEDULE_VECTOR 49u
#define CPU_IPI_WAKEUP_VECTOR     50u

#define AP_TRAMPOLINE_PHYS        0x7000u
#define AP_BOOT_STACK_PAGES       4u
#define AP_STARTUP_TIMEOUT_SPINS  5000000u
#define CPU_ID_INVALID            0xFFu

typedef struct mp_floating_pointer {
	char signature[4];
	uint32_t config_table_phys;
	uint8_t length;
	uint8_t spec_revision;
	uint8_t checksum;
	uint8_t feature1;
	uint8_t feature2;
	uint8_t feature3;
	uint8_t feature4;
	uint8_t feature5;
} __attribute__((packed)) mp_floating_pointer_t;

typedef struct mp_config_table_header {
	char signature[4];
	uint16_t base_table_length;
	uint8_t spec_revision;
	uint8_t checksum;
	char oem_id[8];
	char product_id[12];
	uint32_t oem_table_pointer;
	uint16_t oem_table_size;
	uint16_t entry_count;
	uint32_t local_apic_address;
	uint16_t extended_table_length;
	uint8_t extended_table_checksum;
	uint8_t reserved;
} __attribute__((packed)) mp_config_table_header_t;

typedef struct mp_processor_entry {
	uint8_t type;
	uint8_t local_apic_id;
	uint8_t local_apic_version;
	uint8_t cpu_flags;
	uint32_t cpu_signature;
	uint32_t feature_flags;
	uint32_t reserved0;
	uint32_t reserved1;
} __attribute__((packed)) mp_processor_entry_t;

typedef struct smp_cpu_state {
	uint8_t apic_id;
	bool present;
	volatile bool started;
	void* startup_stack_base;
	uint32_t startup_stack_top;
} smp_cpu_state_t;

extern uint8_t i386_ap_trampoline_start[];
extern uint8_t i386_ap_trampoline_end[];
extern uint8_t i386_ap_trampoline_gdt[];
extern uint32_t i386_ap_trampoline_gdt_base;
extern uint32_t i386_ap_trampoline_cr0_value;
extern uint32_t i386_ap_trampoline_cr3_value;
extern uint32_t i386_ap_trampoline_cr4_value;
extern uint32_t i386_ap_trampoline_stack_top;
extern uint32_t i386_ap_trampoline_entry;

static smp_cpu_state_t smp_cpus[SCHEDULER_MAX_CPUS];
static uint8_t apic_to_cpu[256];
static uint8_t smp_cpu_count = 1;
static uint8_t smp_boot_cpu = 0;

static uint32_t lapic_base_phys = 0xFEE00000u;
static volatile uint32_t* lapic_base_virt;

static bool lapic_ready;
static bool smp_started;
static volatile bool smp_release_secondaries;

static void i386_ap_startup_entry(void);

static uint32_t symbol_offset_from_trampoline(const void* symbol) {
	return (uint32_t) ((uintptr_t) symbol - (uintptr_t) i386_ap_trampoline_start);
}

static void cpu_reset_topology(void) {
	for (uint32_t i = 0; i < SCHEDULER_MAX_CPUS; i++) {
		smp_cpus[i].apic_id = 0;
		smp_cpus[i].present = false;
		smp_cpus[i].started = false;
		smp_cpus[i].startup_stack_base = NULL;
		smp_cpus[i].startup_stack_top = 0;
	}

	for (uint32_t i = 0; i < 256; i++)
		apic_to_cpu[i] = CPU_ID_INVALID;

	smp_cpu_count = 1;
	smp_boot_cpu = 0;
	smp_started = false;
	smp_release_secondaries = false;
}

static uint8_t cpu_bootstrap_apic_id(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	x86_cpuid(1, &eax, &ebx, &ecx, &edx);
	(void) eax;
	(void) ecx;
	(void) edx;
	return (uint8_t) ((ebx >> 24) & 0xFFu);
}

static bool cpu_has_local_apic(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	x86_cpuid(1, &eax, &ebx, &ecx, &edx);
	(void) eax;
	(void) ebx;
	(void) ecx;
	return (edx & (1u << 9)) != 0;
}

static uint8_t byte_checksum(const void* data, uint32_t length) {
	const uint8_t* bytes = (const uint8_t*) data;
	uint8_t sum = 0;
	for (uint32_t i = 0; i < length; i++)
		sum = (uint8_t) (sum + bytes[i]);
	return sum;
}

static const mp_floating_pointer_t* mp_scan_range(uint32_t start_phys, uint32_t length) {
	if (length < sizeof(mp_floating_pointer_t))
		return NULL;

	const uint32_t end_phys = start_phys + length;
	if (end_phys < start_phys)
		return NULL;

	for (uint32_t phys = start_phys; phys + sizeof(mp_floating_pointer_t) <= end_phys; phys += 16u) {
		const mp_floating_pointer_t* mp =
			(const mp_floating_pointer_t*) paging_phys_to_virt(phys);
		if (mp->signature[0] != '_' || mp->signature[1] != 'M' ||
			mp->signature[2] != 'P' || mp->signature[3] != '_')
			continue;

		const uint32_t table_length = (uint32_t) mp->length * 16u;
		if (table_length < sizeof(mp_floating_pointer_t))
			continue;

		if (byte_checksum(mp, table_length) == 0)
			return mp;
	}

	return NULL;
}

static const mp_floating_pointer_t* mp_find_floating_pointer(void) {
	const uint16_t ebda_segment = *(const uint16_t*) paging_phys_to_virt(0x40Eu);
	if (ebda_segment != 0) {
		const uint32_t ebda_phys = (uint32_t) ebda_segment << 4;
		const mp_floating_pointer_t* mp = mp_scan_range(ebda_phys, 1024u);
		if (mp != NULL)
			return mp;
	}

	const uint16_t base_mem_kib = *(const uint16_t*) paging_phys_to_virt(0x413u);
	if (base_mem_kib >= 1u) {
		const uint32_t base_mem_phys = (uint32_t) base_mem_kib * 1024u;
		const uint32_t search_start = base_mem_phys - 1024u;
		const mp_floating_pointer_t* mp = mp_scan_range(search_start, 1024u);
		if (mp != NULL)
			return mp;
	}

	return mp_scan_range(0xF0000u, 0x10000u);
}

static bool mp_collect_cpu_ids(const mp_floating_pointer_t* mp, uint8_t* apic_ids,
	uint32_t* apic_count, uint8_t* bsp_apic_id_out) {
	if (mp == NULL || apic_ids == NULL || apic_count == NULL || bsp_apic_id_out == NULL)
		return false;

	if (mp->config_table_phys == 0 || mp->feature1 != 0)
		return false;

	const mp_config_table_header_t* header =
		(const mp_config_table_header_t*) paging_phys_to_virt(mp->config_table_phys);
	if (header->signature[0] != 'P' || header->signature[1] != 'C' ||
		header->signature[2] != 'M' || header->signature[3] != 'P')
		return false;

	if (header->base_table_length < sizeof(mp_config_table_header_t))
		return false;

	if (byte_checksum(header, header->base_table_length) != 0)
		return false;

	lapic_base_phys = header->local_apic_address;

	bool seen[256];
	for (uint32_t i = 0; i < 256; i++)
		seen[i] = false;

	uint32_t count = 0;
	uint8_t bsp_apic = cpu_bootstrap_apic_id();
	const uint8_t* cursor = (const uint8_t*) (header + 1);
	const uint8_t* end = (const uint8_t*) header + header->base_table_length;

	for (uint32_t entry = 0; entry < header->entry_count && cursor < end; entry++) {
		const uint8_t type = cursor[0];
		if (type == 0) {
			if ((size_t) (end - cursor) < sizeof(mp_processor_entry_t))
				return false;

			const mp_processor_entry_t* processor =
				(const mp_processor_entry_t*) cursor;
			const bool enabled = (processor->cpu_flags & 0x01u) != 0;
			const bool is_bsp = (processor->cpu_flags & 0x02u) != 0;
			if (enabled) {
				if (is_bsp)
					bsp_apic = processor->local_apic_id;
				if (!seen[processor->local_apic_id]) {
					seen[processor->local_apic_id] = true;
					apic_ids[count++] = processor->local_apic_id;
				}
			}

			cursor += sizeof(mp_processor_entry_t);
			continue;
		}

		if ((size_t) (end - cursor) < 8u)
			return false;
		cursor += 8u;
	}

	if (count == 0)
		return false;

	*apic_count = count;
	*bsp_apic_id_out = bsp_apic;
	return true;
}

static void cpu_set_single_cpu_fallback(void) {
	cpu_reset_topology();
	const uint8_t apic_id = cpu_bootstrap_apic_id();
	smp_cpus[0].apic_id = apic_id;
	smp_cpus[0].present = true;
	smp_cpus[0].started = true;
	apic_to_cpu[apic_id] = 0;
}

static void cpu_build_topology_from_apic_ids(
	const uint8_t* apic_ids, uint32_t apic_count, uint8_t bsp_apic_id) {
	cpu_reset_topology();

	uint32_t logical_count = 0;

	bool bsp_present = false;
	for (uint32_t i = 0; i < apic_count; i++) {
		if (apic_ids[i] == bsp_apic_id) {
			bsp_present = true;
			break;
		}
	}

	if (!bsp_present && apic_count < SCHEDULER_MAX_CPUS) {
		smp_cpus[logical_count].apic_id = bsp_apic_id;
		smp_cpus[logical_count].present = true;
		smp_cpus[logical_count].started = true;
		apic_to_cpu[bsp_apic_id] = (uint8_t) logical_count;
		logical_count++;
	}

	for (uint32_t i = 0; i < apic_count && logical_count < SCHEDULER_MAX_CPUS; i++) {
		const uint8_t apic_id = apic_ids[i];
		if (apic_id != bsp_apic_id)
			continue;

		smp_cpus[logical_count].apic_id = apic_id;
		smp_cpus[logical_count].present = true;
		smp_cpus[logical_count].started = true;
		apic_to_cpu[apic_id] = (uint8_t) logical_count;
		smp_boot_cpu = (uint8_t) logical_count;
		logical_count++;
		break;
	}

	if (logical_count == 0) {
		smp_cpus[0].apic_id = bsp_apic_id;
		smp_cpus[0].present = true;
		smp_cpus[0].started = true;
		apic_to_cpu[bsp_apic_id] = 0;
		logical_count = 1;
	}

	for (uint32_t i = 0; i < apic_count && logical_count < SCHEDULER_MAX_CPUS; i++) {
		const uint8_t apic_id = apic_ids[i];
		if (apic_id == bsp_apic_id)
			continue;

		if (apic_to_cpu[apic_id] != CPU_ID_INVALID)
			continue;

		smp_cpus[logical_count].apic_id = apic_id;
		smp_cpus[logical_count].present = true;
		smp_cpus[logical_count].started = false;
		apic_to_cpu[apic_id] = (uint8_t) logical_count;
		logical_count++;
	}

	smp_cpu_count = (uint8_t) logical_count;
}

static bool lapic_map_mmio(void) {
	if (lapic_base_virt != NULL)
		return true;

	void* reserved_page = paging_alloc_pages(1);
	if (reserved_page == NULL)
		return false;

	const uint32_t allocated_frame = paging_virt_to_phys(reserved_page);
	paging_unmap_page(reserved_page);
	pmm_free_frame(allocated_frame);

	if (!paging_map_page(reserved_page, lapic_base_phys & 0xFFFFF000u, PAGING_FLAG_WRITABLE))
		return false;

	lapic_base_virt =
		(volatile uint32_t*) ((uintptr_t) reserved_page + (lapic_base_phys & 0xFFFu));
	return true;
}

static uint32_t lapic_read(uint32_t offset) {
	return lapic_base_virt[offset / sizeof(uint32_t)];
}

static void lapic_write(uint32_t offset, uint32_t value) {
	lapic_base_virt[offset / sizeof(uint32_t)] = value;
	(void) lapic_read(LAPIC_REG_ID);
}

static void lapic_eoi(void) {
	if (!lapic_ready)
		return;

	lapic_write(LAPIC_REG_EOI, 0);
}

static bool lapic_enable_local_controller(void) {
	if (!cpu_has_local_apic())
		return false;

	uint64_t apic_base = x86_rdmsr(IA32_APIC_BASE_MSR);
	apic_base |= IA32_APIC_BASE_ENABLE;
	x86_wrmsr(IA32_APIC_BASE_MSR, apic_base);
	lapic_base_phys = (uint32_t) (apic_base & IA32_APIC_BASE_ADDR_MASK);

	if (!lapic_map_mmio())
		return false;

	lapic_write(LAPIC_REG_TPR, 0);
	lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
	lapic_ready = true;
	return true;
}

static uint8_t lapic_current_apic_id(void) {
	if (!lapic_ready)
		return cpu_bootstrap_apic_id();

	return (uint8_t) ((lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu);
}

static void lapic_wait_icr_idle(void) {
	for (uint32_t spins = 0; spins < AP_STARTUP_TIMEOUT_SPINS; spins++) {
		if ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_STATUS_PENDING) == 0)
			return;
		x86_pause();
	}
}

static void lapic_send_ipi(uint8_t apic_id, uint32_t icr_low) {
	if (!lapic_ready)
		return;

	lapic_wait_icr_idle();
	lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t) apic_id << 24);
	lapic_write(LAPIC_REG_ICR_LOW, icr_low);
	lapic_wait_icr_idle();
}

static void apic_delay(uint32_t iterations) {
	for (uint32_t i = 0; i < iterations; i++)
		x86_io_wait();
}

static bool cpu_prepare_ap_trampoline(uint32_t stack_top) {
	const uint32_t trampoline_size =
		(uint32_t) ((uintptr_t) i386_ap_trampoline_end - (uintptr_t) i386_ap_trampoline_start);
	if (trampoline_size == 0 || trampoline_size > PAGE_SIZE)
		return false;

	uint8_t* dst = (uint8_t*) paging_phys_to_virt(AP_TRAMPOLINE_PHYS);
	memcpy(dst, i386_ap_trampoline_start, trampoline_size);

	const uint32_t gdt_base_offset = symbol_offset_from_trampoline(&i386_ap_trampoline_gdt_base);
	const uint32_t gdt_offset = symbol_offset_from_trampoline(i386_ap_trampoline_gdt);
	const uint32_t cr0_offset = symbol_offset_from_trampoline(&i386_ap_trampoline_cr0_value);
	const uint32_t cr3_offset = symbol_offset_from_trampoline(&i386_ap_trampoline_cr3_value);
	const uint32_t cr4_offset = symbol_offset_from_trampoline(&i386_ap_trampoline_cr4_value);
	const uint32_t stack_offset = symbol_offset_from_trampoline(&i386_ap_trampoline_stack_top);
	const uint32_t entry_offset = symbol_offset_from_trampoline(&i386_ap_trampoline_entry);

	*(uint32_t*) (dst + gdt_base_offset) = AP_TRAMPOLINE_PHYS + gdt_offset;
	*(uint32_t*) (dst + cr0_offset) = x86_read_cr0() | 0x80000001u;
	*(uint32_t*) (dst + cr3_offset) = x86_read_cr3();
	*(uint32_t*) (dst + cr4_offset) = x86_read_cr4();
	*(uint32_t*) (dst + stack_offset) = stack_top;
	*(uint32_t*) (dst + entry_offset) = (uint32_t) (uintptr_t) i386_ap_startup_entry;
	return true;
}

static bool cpu_wait_for_ap_online(uint8_t cpu_id) {
	for (uint32_t spins = 0; spins < AP_STARTUP_TIMEOUT_SPINS; spins++) {
		if (smp_cpus[cpu_id].started)
			return true;
		x86_pause();
	}

	return false;
}

static bool cpu_start_one_ap(uint8_t cpu_id) {
	if (cpu_id >= smp_cpu_count || !smp_cpus[cpu_id].present)
		return false;
	if (smp_cpus[cpu_id].apic_id == smp_cpus[smp_boot_cpu].apic_id)
		return true;

	if (smp_cpus[cpu_id].startup_stack_base == NULL) {
		void* stack = paging_alloc_pages(AP_BOOT_STACK_PAGES);
		if (stack == NULL)
			return false;

		smp_cpus[cpu_id].startup_stack_base = stack;
		smp_cpus[cpu_id].startup_stack_top =
			(uint32_t) (uintptr_t) stack + AP_BOOT_STACK_PAGES * PAGE_SIZE;
	}

	if (!scheduler_bootstrap_secondary_current(
		cpu_id, smp_cpus[cpu_id].startup_stack_top, "cpu-idle"))
		return false;

	if (!cpu_prepare_ap_trampoline(smp_cpus[cpu_id].startup_stack_top))
		return false;

	smp_cpus[cpu_id].started = false;
	const uint8_t vector = (uint8_t) (AP_TRAMPOLINE_PHYS >> 12);
	const uint8_t apic_id = smp_cpus[cpu_id].apic_id;

	lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT |
		LAPIC_ICR_TRIGGER_LEVEL);
	apic_delay(20000u);
	lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_TRIGGER_LEVEL);
	apic_delay(10000u);
	lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_STARTUP | vector);
	apic_delay(2000u);
	lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_STARTUP | vector);

	return cpu_wait_for_ap_online(cpu_id);
}

static interrupt_frame_t* cpu_on_ipi_reschedule(interrupt_frame_t* frame) {
	interrupt_frame_t* next = scheduler_on_yield(frame);
	lapic_eoi();
	return next;
}

static interrupt_frame_t* cpu_on_ipi_wakeup(interrupt_frame_t* frame) {
	interrupt_frame_t* next = scheduler_on_yield(frame);
	lapic_eoi();
	return next;
}

static void i386_ap_startup_entry(void) {
	idt_load_current_cpu();

	const uint32_t cpu_id = cpu_current_id();
	if (cpu_id < smp_cpu_count) {
		gdt_set_kernel_stack(smp_cpus[cpu_id].startup_stack_top);
		smp_cpus[cpu_id].started = true;
	}

	while (!smp_release_secondaries)
		x86_pause();

	interrupts_enable();
	while (true)
		x86_hlt();
}

bool cpu_has_pae(void) {
	return x86_cpu_has_pae();
}

uint32_t cpu_current_id(void) {
	const uint8_t apic_id = lapic_current_apic_id();
	const uint8_t cpu_id = apic_to_cpu[apic_id];
	if (cpu_id == CPU_ID_INVALID)
		return 0;
	return cpu_id;
}

bool cpu_smp_available(void) {
	return smp_started;
}

bool cpu_smp_prepare_topology(scheduler_topology_config_t* config) {
	if (config == NULL)
		return false;

	cpu_set_single_cpu_fallback();
	lapic_ready = false;

	if (cpu_has_local_apic()) {
		const mp_floating_pointer_t* mp = mp_find_floating_pointer();
		uint8_t apic_ids[256];
		uint32_t apic_count = 0;
		uint8_t bsp_apic_id = cpu_bootstrap_apic_id();
		if (mp_collect_cpu_ids(mp, apic_ids, &apic_count, &bsp_apic_id))
			cpu_build_topology_from_apic_ids(apic_ids, apic_count, bsp_apic_id);
	}

	if (!lapic_enable_local_controller())
		cpu_set_single_cpu_fallback();

	register_interrupt_handler(CPU_IPI_RESCHEDULE_VECTOR, cpu_on_ipi_reschedule);
	register_interrupt_handler(CPU_IPI_WAKEUP_VECTOR, cpu_on_ipi_wakeup);

	config->cpu_count = smp_cpu_count;
	config->boot_cpu_id = smp_boot_cpu;
	config->node_count = 1;

	for (uint32_t i = 0; i < SCHEDULER_MAX_CPUS; i++) {
		config->cpu_to_node[i] = 0;
		config->cpu_online[i] = i < smp_cpu_count && smp_cpus[i].present;
	}

	return true;
}

bool cpu_smp_start_secondary_cores(void) {
	if (!lapic_ready || smp_cpu_count <= 1)
		return true;

	smp_release_secondaries = false;

	for (uint8_t cpu_id = 0; cpu_id < smp_cpu_count; cpu_id++) {
		if (cpu_id == smp_boot_cpu)
			continue;
		if (!cpu_start_one_ap(cpu_id))
			return false;
	}

	return true;
}

void cpu_smp_release_secondary_cores(void) {
	if (!lapic_ready || smp_cpu_count <= 1)
		return;

	smp_release_secondaries = true;
	smp_started = true;
}

void cpu_send_reschedule_ipi(uint32_t cpu_id) {
	if (!smp_started || !lapic_ready || cpu_id >= smp_cpu_count)
		return;
	if (!smp_cpus[cpu_id].started)
		return;

	lapic_send_ipi(smp_cpus[cpu_id].apic_id,
		LAPIC_ICR_DELIVERY_FIXED | CPU_IPI_RESCHEDULE_VECTOR);
}

void cpu_send_wakeup_ipi(uint32_t cpu_id) {
	if (!smp_started || !lapic_ready || cpu_id >= smp_cpu_count)
		return;
	if (!smp_cpus[cpu_id].started)
		return;

	lapic_send_ipi(smp_cpus[cpu_id].apic_id,
		LAPIC_ICR_DELIVERY_FIXED | CPU_IPI_WAKEUP_VECTOR);
}

void cpu_halt(void) {
	x86_hlt();
}
