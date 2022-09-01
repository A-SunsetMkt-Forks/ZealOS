#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <lib.h>

static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

static volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST,
    .revision = 0
};

struct CZXE {
    uint16_t jmp;
    uint8_t module_align_bits;
    uint8_t reserved;
    uint32_t signature;
    int64_t org;
    int64_t patch_table_offset;
    int64_t file_size;
} __attribute__((packed));

struct CDate {
    uint32_t time;
    int32_t date;
} __attribute__((packed));

#define MEM_E820_ENTRIES_NUM 48

#define MEM_E820T_USABLE 1
#define MEM_E820T_RESERVED 2
#define MEM_E820T_ACPI 3
#define MEM_E820T_ACPI_NVS 4
#define MEM_E820T_BAD_MEM 5
#define MEM_E820T_PERM_MEM 7

struct CMemE820 {
    uint8_t *base;
    int64_t len;
    uint8_t type, pad[3];
} __attribute__((packed));

struct CGDTEntry {
    uint64_t lo, hi;
} __attribute__((packed));

#define MP_PROCESSORS_NUM 128

struct CGDT {
    struct CGDTEntry null;
    struct CGDTEntry boot_ds;
    struct CGDTEntry boot_cs;
    struct CGDTEntry cs32;
    struct CGDTEntry cs64;
    struct CGDTEntry cs64_ring3;
    struct CGDTEntry ds;
    struct CGDTEntry ds_ring3;
    struct CGDTEntry tr[MP_PROCESSORS_NUM];
    struct CGDTEntry tr_ring3[MP_PROCESSORS_NUM];
} __attribute__((packed));

struct CSysLimitBase {
    uint16_t limit;
    uint8_t *base;
} __attribute__((packed));

struct CKernel {
    struct CZXE h;
    uint32_t jmp;
    uint32_t boot_src;
    uint32_t boot_blk;
    uint32_t boot_patch_table_base;
    uint32_t sys_run_level;
    struct CDate compile_time;
    // U0 start
    uint32_t boot_base;
    uint16_t mem_E801[2];
    struct CMemE820 mem_E820[MEM_E820_ENTRIES_NUM];
    uint64_t mem_physical_space;
    struct CSysLimitBase sys_gdt_ptr;
    uint16_t sys_pci_buses;
    struct CGDT sys_gdt __attribute__((aligned(16)));
    uint64_t sys_framebuffer_addr;
    uint64_t sys_framebuffer_width;
    uint64_t sys_framebuffer_height;
    uint64_t sys_framebuffer_pitch;
    uint8_t sys_framebuffer_bpp;
    uint64_t sys_smbios_entry;
	uint32_t sys_disk_uuid_a;
	uint16_t sys_disk_uuid_b;
	uint16_t sys_disk_uuid_c;
	uint8_t sys_disk_uuid_d[8];
	uint32_t sys_boot_stack;
} __attribute__((packed));

#define BOOT_SRC_RAM 2
#define BOOT_SRC_HDD 3
#define BOOT_SRC_DVD 4
#define RLF_16BIT 0b001
#define RLF_VESA  0b010
#define RLF_32BIT 0b100

extern symbol trampoline, trampoline_end;

struct E801 {
    size_t lowermem;
    size_t uppermem;
};

static struct E801 get_E801(void) {
    struct E801 E801 = {0};

    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            if (entry->base == 0x100000) {
                if (entry->length > 0xf00000) {
                    E801.lowermem = 0x3c00;
                } else {
                    E801.lowermem = entry->length / 1024;
                }
            }
            if (entry->base <= 0x1000000 && entry->base + entry->length > 0x1000000) {
                E801.uppermem = ((entry->length - (0x1000000 - entry->base)) / 1024) / 64;
            }
        }
    }

    return E801;
}

void _start(void) {
    struct limine_file *module_kernel = module_request.response->modules[0];
    struct CKernel *kernel = module_kernel->address;

    size_t trampoline_size = (uintptr_t)trampoline_end - (uintptr_t)trampoline;

    size_t boot_stack_size = 32768;

    uintptr_t final_address = (uintptr_t)-1;
    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->length >= ALIGN_UP(module_kernel->size + trampoline_size, 16) + boot_stack_size) {
            final_address = entry->base;
            break;
        }
    }
    if (final_address == (uintptr_t)-1) {
        // TODO: Panic. Show something?
        for (;;);
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    kernel->sys_framebuffer_pitch = fb->pitch;
    kernel->sys_framebuffer_width = fb->width;
    kernel->sys_framebuffer_height = fb->height;
    kernel->sys_framebuffer_bpp = fb->bpp;
    kernel->sys_framebuffer_addr = (uintptr_t)fb->address - hhdm_request.response->offset;

    void *entry_point; // to CORE0_32BIT_INIT
    for (uint64_t *p = (uint64_t *)kernel; ; p++) {
        if (*p != 0xaa23c08ed10bd4d7) {
            continue;
        }
        p++;
        if (*p != 0xf6ceba7d4b74179a) {
            continue;
        }
        p++;
        entry_point = p;
        break;
    }

    entry_point -= (uintptr_t)module_kernel->address;
    entry_point += final_address;

    if (module_kernel->media_type == LIMINE_MEDIA_TYPE_OPTICAL)
        kernel->boot_src = BOOT_SRC_DVD;
    else if (module_kernel->media_type == LIMINE_MEDIA_TYPE_GENERIC)
        kernel->boot_src = BOOT_SRC_HDD;
    else
        kernel->boot_src = BOOT_SRC_RAM;
    kernel->boot_blk = 0;
    kernel->boot_patch_table_base = (uintptr_t)kernel + kernel->h.patch_table_offset;
    kernel->boot_patch_table_base -= (uintptr_t)module_kernel->address;
    kernel->boot_patch_table_base += final_address;

    kernel->sys_run_level = RLF_VESA | RLF_16BIT | RLF_32BIT;

    kernel->boot_base = (uintptr_t)&kernel->jmp - (uintptr_t)module_kernel->address;
    kernel->boot_base += final_address;

    kernel->sys_gdt_ptr.limit = sizeof(kernel->sys_gdt) - 1;
    kernel->sys_gdt_ptr.base = (void *)&kernel->sys_gdt - (uintptr_t)module_kernel->address;
    kernel->sys_gdt_ptr.base += final_address;

    kernel->sys_pci_buses = 256;

    struct E801 E801 = get_E801();
    kernel->mem_E801[0] = E801.lowermem;
    kernel->mem_E801[1] = E801.uppermem;

    kernel->mem_physical_space = 0;

    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];

        int our_type;
        switch (entry->type) {
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            case LIMINE_MEMMAP_KERNEL_AND_MODULES:
            case LIMINE_MEMMAP_USABLE:
                our_type = MEM_E820T_USABLE; break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
                our_type = MEM_E820T_ACPI; break;
            case LIMINE_MEMMAP_ACPI_NVS:
                our_type = MEM_E820T_ACPI_NVS; break;
            case LIMINE_MEMMAP_BAD_MEMORY:
                our_type = MEM_E820T_BAD_MEM; break;
            case LIMINE_MEMMAP_RESERVED:
            default:
                our_type = MEM_E820T_RESERVED; break;
        }

        kernel->mem_E820[i].base = (void *)entry->base;
        kernel->mem_E820[i].len = entry->length;
        kernel->mem_E820[i].type = our_type;

        if (kernel->mem_physical_space < entry->base + entry->length) {
            kernel->mem_physical_space = entry->base + entry->length;
        }
    }

    kernel->mem_E820[memmap_request.response->entry_count].type = 0;

    kernel->mem_physical_space = ALIGN_UP(kernel->mem_physical_space, 0x200000);

    void *sys_gdt_ptr = (void *)&kernel->sys_gdt_ptr - (uintptr_t)module_kernel->address;
    sys_gdt_ptr += final_address;

    void *sys_smbios_entry = smbios_request.response->entry_32;
    if (sys_smbios_entry != NULL) {
        kernel->sys_smbios_entry = (uintptr_t)sys_smbios_entry - hhdm_request.response->offset;
    }

	kernel->sys_disk_uuid_a = module_kernel->gpt_disk_uuid.a;
	kernel->sys_disk_uuid_b = module_kernel->gpt_disk_uuid.b;
	kernel->sys_disk_uuid_c = module_kernel->gpt_disk_uuid.c;
	memcpy(kernel->sys_disk_uuid_d, module_kernel->gpt_disk_uuid.d, 8);

    void *trampoline_phys = (void *)final_address + module_kernel->size;

    uintptr_t boot_stack = ALIGN_UP(final_address + module_kernel->size + trampoline_size, 16) + boot_stack_size;

	kernel->sys_boot_stack = boot_stack;

    memcpy(trampoline_phys, trampoline, trampoline_size);
    memcpy((void *)final_address, kernel, module_kernel->size);

    asm volatile (
        "jmp *%0"
        :
        : "a"(trampoline_phys), "b"(entry_point),
          "c"(sys_gdt_ptr), "d"(boot_stack),
          "S"(kernel->boot_patch_table_base), "D"(kernel->boot_base)
        : "memory");

    __builtin_unreachable();
}
