/* ARMv7 UEFI handoff for the freestanding Pedigree ELF kernel. */
#include "pedigree/kernel/processor/armv7/UefiHandoff.h"

#include <stdint.h>

#include "exit_boot_services.h"

typedef uint16_t efi_char16_t;
typedef struct {
  uint32_t a;
  uint16_t b, c;
  uint8_t d[8];
} efi_guid_t;

typedef struct efi_file efi_file_t;
typedef struct efi_system_table efi_system_table_t;
typedef efi_status_t (*efi_handle_protocol_t)(efi_handle_t, efi_guid_t*, void**);
typedef efi_status_t (*efi_allocate_pages_t)(uint32_t, uint32_t, uintptr_t, uint64_t*);

typedef struct {
  uint8_t header[24];
  void* raise_tpl;
  void* restore_tpl;
  efi_allocate_pages_t allocate_pages;
  void* free_pages;
  efi_get_memory_map_t get_memory_map;
  void* allocate_pool;
  void* free_pool;
  uint8_t before_handle_protocol[9 * 4];
  efi_handle_protocol_t handle_protocol;
  void* reserved_after_handle_protocol;
  void* register_protocol_notify;
  void* locate_handle;
  void* locate_device_path;
  void* install_configuration_table;
  void* load_image;
  void* start_image;
  void* exit;
  void* unload_image;
  efi_exit_boot_services_t exit_boot_services;
} efi_boot_services_t;

typedef struct {
  void* reset;
  efi_status_t (*output_string)(void*, efi_char16_t*);
} efi_simple_text_output_t;

struct efi_system_table {
  uint8_t header[24];
  efi_char16_t* firmware_vendor;
  uint32_t firmware_revision;
  efi_handle_t console_in_handle;
  void* con_in;
  efi_handle_t console_out_handle;
  efi_simple_text_output_t* con_out;
  efi_handle_t standard_error_handle;
  void* std_err;
  void* runtime_services;
  efi_boot_services_t* boot_services;
  uint32_t configuration_table_entries;
  void* configuration_table;
};

struct efi_file {
  uint64_t revision;
  efi_status_t (*open)(efi_file_t*, efi_file_t**, efi_char16_t*, uint64_t, uint64_t);
  efi_status_t (*close)(efi_file_t*);
  void* delete_file;
  efi_status_t (*read)(efi_file_t*, uint32_t*, void*);
  void* write;
  void* get_position;
  void* set_position;
  efi_status_t (*get_info)(efi_file_t*, efi_guid_t*, uint32_t*, void*);
};

typedef struct {
  uint64_t revision;
  efi_status_t (*open_volume)(void*, efi_file_t**);
} efi_simple_file_system_t;

typedef struct {
  uint32_t revision;
  efi_handle_t parent_handle;
  efi_system_table_t* system_table;
  efi_handle_t device_handle;
} efi_loaded_image_t;

typedef struct {
  efi_guid_t guid;
  void* table;
} efi_configuration_table_t;

typedef struct {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint32_t entry, program_offset, section_offset;
  uint32_t flags;
  uint16_t header_size, program_size, program_count;
  uint16_t section_size, section_count, section_string_index;
} elf32_header_t;

typedef struct {
  uint32_t type, offset, virtual_address, physical_address;
  uint32_t file_size, memory_size, flags, alignment;
} elf32_program_t;

#define EFI_LOADER_DATA 2
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_FILE_MODE_READ 1
#define MAX_PHYSICAL_ADDRESS 0x7fffffffULL
#define MAX_KERNEL_FILE_SIZE (64ULL * 1024 * 1024)
#define MAX_ROOTFS_SIZE (512ULL * 1024 * 1024)

static const efi_guid_t loaded_image_guid = {
    0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const efi_guid_t file_system_guid = {
    0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const efi_guid_t file_info_guid = {
    0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const efi_guid_t fdt_guid = {
    0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};

static void print(efi_system_table_t* table, efi_char16_t* message) {
  if (table && table->con_out) {
    table->con_out->output_string(table->con_out, message);
  }
}

static int same_guid(const efi_guid_t* a, const efi_guid_t* b) {
  if (a->a != b->a || a->b != b->b || a->c != b->c) {
    return 0;
  }
  for (unsigned i = 0; i < 8; ++i) {
    if (a->d[i] != b->d[i]) {
      return 0;
    }
  }
  return 1;
}

static void* allocate(efi_system_table_t* table, uint64_t bytes) {
  if (!bytes || bytes > MAX_PHYSICAL_ADDRESS - 4095) {
    return 0;
  }
  uint64_t address = MAX_PHYSICAL_ADDRESS;
  const efi_status_t status = table->boot_services->allocate_pages(
      EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, (bytes + 4095) / 4096, &address);
  return status == EFI_SUCCESS ? (void*)address : 0;
}

static void copy(void* destination, const void* source, uint64_t count) {
  uint8_t* to = destination;
  const uint8_t* from = source;
  for (uint64_t i = 0; i < count; ++i) {
    to[i] = from[i];
  }
}

static void zero(void* destination, uint64_t count) {
  uint8_t* bytes = destination;
  for (uint64_t i = 0; i < count; ++i) {
    bytes[i] = 0;
  }
}

static void clean_buffer(uint32_t address, uint32_t size) {
  uint32_t ctr;
  __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
  const uint32_t line_size = 4U << ((ctr >> 16) & 15);
  const uint32_t end = address + size;
  for (uint32_t cursor = address & ~(line_size - 1); cursor < end; cursor += line_size) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(cursor) : "memory");
  }
}

static void* read_file(efi_system_table_t* table, efi_file_t* root, efi_char16_t* path,
                       uint64_t maximum, uint64_t* length) {
  efi_file_t* file = 0;
  if (root->open(root, &file, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
    return 0;
  }
  uint64_t info[32];
  uint32_t info_size = sizeof(info);
  if (file->get_info(file, (efi_guid_t*)&file_info_guid, &info_size, info) != EFI_SUCCESS ||
      info[1] == 0 || info[1] > maximum) {
    print(table, (efi_char16_t*)L"UEFI: file info failed\r\n");
    file->close(file);
    return 0;
  }
  void* buffer = allocate(table, info[1] + 1);
  if (!buffer) {
    print(table, (efi_char16_t*)L"UEFI: file allocation failed\r\n");
    file->close(file);
    return 0;
  }
  uint64_t position = 0;
  while (position < info[1]) {
    uint32_t requested = info[1] - position;
    if (file->read(file, &requested, (uint8_t*)buffer + position) != EFI_SUCCESS || !requested) {
      print(table, (efi_char16_t*)L"UEFI: file read failed\r\n");
      file->close(file);
      return 0;
    }
    position += requested;
  }
  ((uint8_t*)buffer)[position] = 0;
  file->close(file);
  *length = position;
  return buffer;
}

static int load_kernel(efi_system_table_t* table, const uint8_t* data, uint64_t length,
                       uint32_t* entry) {
  if (length < sizeof(elf32_header_t)) {
    return 0;
  }
  const elf32_header_t* header = (const elf32_header_t*)data;
  if (header->ident[0] != 0x7f || header->ident[1] != 'E' || header->ident[2] != 'L' ||
      header->ident[3] != 'F' || header->ident[4] != 1 || header->ident[5] != 1 ||
      header->machine != 40 || header->type != 2 ||
      header->program_size != sizeof(elf32_program_t) || !header->program_count ||
      header->program_offset > length ||
      header->program_count > (length - header->program_offset) / header->program_size) {
    return 0;
  }
  int entry_loaded = 0;
  for (uint16_t i = 0; i < header->program_count; ++i) {
    const elf32_program_t* program =
        (const elf32_program_t*)(data + header->program_offset + i * sizeof(elf32_program_t));
    if (program->type != 1) {
      continue;
    }
    if (!program->memory_size || program->file_size > program->memory_size ||
        program->offset > length || program->file_size > length - program->offset ||
        (program->physical_address & 4095) || program->physical_address > MAX_PHYSICAL_ADDRESS ||
        program->memory_size > MAX_PHYSICAL_ADDRESS - program->physical_address) {
      return 0;
    }
    uint64_t address = program->physical_address;
    if (table->boot_services->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                             (program->memory_size + 4095) / 4096,
                                             &address) != EFI_SUCCESS) {
      return 0;
    }
    copy((void*)address, data + program->offset, program->file_size);
    zero((uint8_t*)address + program->file_size, program->memory_size - program->file_size);
    clean_buffer(address, program->memory_size);
    if (header->entry >= address && header->entry - address < program->memory_size) {
      entry_loaded = 1;
    }
  }
  __asm__ volatile("dsb sy\n mcr p15, 0, %0, c7, c5, 0\n dsb sy\n isb" : : "r"(0) : "memory");
  *entry = header->entry;
  return entry_loaded;
}

static void enter_kernel(uint32_t entry, uint32_t handoff) __attribute__((noreturn));
static void enter_kernel(uint32_t entry, uint32_t handoff) {
  __asm__ volatile(
      "cpsid if\n"
      "dsb sy\n"
      "mrc p15, 0, r12, c1, c0, 0\n"
      "bic r12, r12, #1\n"
      "bic r12, r12, #4\n"
      "bic r12, r12, #0x1000\n"
      "mcr p15, 0, r12, c1, c0, 0\n"
      "isb\n"
      "mov r0, %0\n"
      "mov r1, %1\n"
      "bx %2\n"
      :
      : "r"(ARMV7_UEFI_HANDOFF_MAGIC), "r"(handoff), "r"(entry)
      : "r0", "r1", "r12", "memory");
  for (;;) {
    __asm__ volatile("wfi");
  }
}

efi_status_t efi_main(efi_handle_t image, efi_system_table_t* table) {
  print(table, (efi_char16_t*)L"Pedigree ARMv7 UEFI loader\r\n");

  uint32_t fdt = 0;
  const efi_configuration_table_t* tables = table->configuration_table;
  for (uint32_t i = 0; i < table->configuration_table_entries; ++i) {
    if (same_guid(&tables[i].guid, &fdt_guid)) {
      fdt = (uint32_t)tables[i].table;
    }
  }
  if (!fdt || fdt >= 0x80000000U) {
    print(table, (efi_char16_t*)L"UEFI: no usable FDT\r\n");
    return EFI_LOAD_ERROR;
  }
  const uint8_t* firmware_fdt = (const uint8_t*)fdt;
  const uint32_t fdt_size = ((uint32_t)firmware_fdt[4] << 24) | ((uint32_t)firmware_fdt[5] << 16) |
                            ((uint32_t)firmware_fdt[6] << 8) | firmware_fdt[7];
  if (firmware_fdt[0] != 0xd0 || firmware_fdt[1] != 0x0d || firmware_fdt[2] != 0xfe ||
      firmware_fdt[3] != 0xed || fdt_size < 40 || fdt_size > 2 * 1024 * 1024) {
    print(table, (efi_char16_t*)L"UEFI: invalid FDT\r\n");
    return EFI_LOAD_ERROR;
  }
  void* fdt_copy = allocate(table, fdt_size);
  if (!fdt_copy) {
    print(table, (efi_char16_t*)L"UEFI: FDT allocation failed\r\n");
    return EFI_LOAD_ERROR;
  }
  copy(fdt_copy, firmware_fdt, fdt_size);
  fdt = (uint32_t)fdt_copy;
  print(table, (efi_char16_t*)L"UEFI: FDT platform\r\n");

  efi_loaded_image_t* loaded = 0;
  efi_simple_file_system_t* filesystem = 0;
  if (table->boot_services->handle_protocol(image, (efi_guid_t*)&loaded_image_guid,
                                            (void**)&loaded) != EFI_SUCCESS ||
      table->boot_services->handle_protocol(loaded->device_handle, (efi_guid_t*)&file_system_guid,
                                            (void**)&filesystem) != EFI_SUCCESS) {
    print(table, (efi_char16_t*)L"UEFI: filesystem unavailable\r\n");
    return EFI_LOAD_ERROR;
  }
  efi_file_t* root = 0;
  if (filesystem->open_volume(filesystem, &root) != EFI_SUCCESS) {
    print(table, (efi_char16_t*)L"UEFI: cannot open ESP\r\n");
    return EFI_LOAD_ERROR;
  }

  uint64_t kernel_length = 0, rootfs_length = 0, cmdline_length = 0;
  uint8_t* kernel_file = read_file(table, root, (efi_char16_t*)L"\\EFI\\PEDIGREE\\current\\kernel",
                                   MAX_KERNEL_FILE_SIZE, &kernel_length);
  uint8_t* rootfs = read_file(table, root, (efi_char16_t*)L"\\EFI\\PEDIGREE\\current\\rootfs.img",
                              MAX_ROOTFS_SIZE, &rootfs_length);
  char* cmdline = read_file(table, root, (efi_char16_t*)L"\\EFI\\PEDIGREE\\current\\cmdline", 4095,
                            &cmdline_length);
  root->close(root);
  if (!kernel_file) {
    print(table, (efi_char16_t*)L"UEFI: kernel read failed\r\n");
    return EFI_LOAD_ERROR;
  }
  if (!cmdline) {
    print(table, (efi_char16_t*)L"UEFI: command line read failed\r\n");
    return EFI_LOAD_ERROR;
  }
  for (uint64_t i = 0; i < cmdline_length; ++i) {
    if (!cmdline[i] || cmdline[i] == '\r' || cmdline[i] == '\n') {
      cmdline[i] = 0;
      break;
    }
  }

  uint32_t entry = 0;
  if (!load_kernel(table, kernel_file, kernel_length, &entry)) {
    print(table, (efi_char16_t*)L"UEFI: cannot load ARMv7 kernel\r\n");
    return EFI_LOAD_ERROR;
  }

  void* raw_map = allocate(table, 16 * 4096);
  bootstrap_memory_map_entry_t* normalized = allocate(table, 16 * 4096);
  armv7_uefi_handoff_t* handoff = allocate(table, 4096);
  if (!raw_map || !normalized || !handoff) {
    print(table, (efi_char16_t*)L"UEFI: memory map allocation failed\r\n");
    return EFI_LOAD_ERROR;
  }
  print(table, (efi_char16_t*)L"UEFI: entering kernel\r\n");
  uint32_t normalized_bytes = 0;
  int exit_attempted = 0;
  const efi_status_t status = exit_boot_services_with_map(
      table->boot_services->get_memory_map, table->boot_services->exit_boot_services, image,
      raw_map, 16 * 4096, normalized, 16 * 4096, &normalized_bytes, &exit_attempted);
  if (status != EFI_SUCCESS) {
    if (!exit_attempted) {
      print(table, (efi_char16_t*)L"UEFI: ExitBootServices failed\r\n");
    }
    return status;
  }
  const uint32_t initrd_start = (uint32_t)rootfs;
  *handoff = (armv7_uefi_handoff_t){ARMV7_UEFI_HANDOFF_MAGIC,
                                    fdt,
                                    initrd_start,
                                    initrd_start + (uint32_t)rootfs_length,
                                    (uint32_t)cmdline,
                                    (uint32_t)normalized,
                                    normalized_bytes};
  clean_buffer((uint32_t)handoff, sizeof(*handoff));
  clean_buffer(fdt, fdt_size);
  clean_buffer((uint32_t)normalized, normalized_bytes);
  clean_buffer((uint32_t)cmdline, (uint32_t)cmdline_length + 1);
  if (rootfs) {
    clean_buffer(initrd_start, (uint32_t)rootfs_length);
  }
  __asm__ volatile("dsb sy" : : : "memory");
  enter_kernel(entry, (uint32_t)handoff);
}
