/* SPDX-License-Identifier: ISC */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

#if QEMU_PLUGIN_VERSION != 7
#error This plugin requires QEMU plugin API 7
#endif

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* Stable offsets keep translated inline operations valid after retranslation. */
#define MAX_INSTRUCTIONS 524288
typedef struct {
  uint64_t pc;
  char bytes[31];
  char* disas;
  qemu_plugin_u64 counter;
} Instruction;

static FILE* output;
static GHashTable *indexed, *syscalls, *discontinuities;
static GPtrArray* instructions;
static GByteArray* value;
static struct qemu_plugin_register *rax, *rsp, *cr3;
static struct qemu_plugin_scoreboard* scoreboard;
static qemu_plugin_u64 user_counter;
static uint64_t start, stop, origin_cr3, origin_rsp, skip, seen;
static uint64_t kernel_base = 0xffff800000000000ULL;
static uint64_t syscall_number = UINT64_MAX;
static bool active, finished;

static uint64_t read_reg(struct qemu_plugin_register* reg) {
  g_byte_array_set_size(value, 0);
  if (!qemu_plugin_read_register(reg, value) || !value->len || value->len > 8)
    abort();
  uint64_t result = 0;
  for (unsigned i = 0; i < value->len; ++i)
    result |= (uint64_t)value->data[i] << (8 * i);
  return result;
}

static void increment(GHashTable* table, char* key) {
  uint64_t* count = g_hash_table_lookup(table, key);
  if (count) {
    ++*count;
    g_free(key);
  } else {
    count = g_new(uint64_t, 1);
    *count = 1;
    g_hash_table_insert(table, key, count);
  }
}

static void dump(unsigned cpu, const char* status) {
  active = false;
  finished = true;
  uint64_t kernel = 0, user = qemu_plugin_u64_get(user_counter, cpu);
  for (unsigned i = 0; i < instructions->len; ++i) {
    Instruction* insn = g_ptr_array_index(instructions, i);
    uint64_t count = qemu_plugin_u64_get(insn->counter, cpu);
    if (count) {
      fprintf(output, "P\t%" PRIx64 "\t%" PRIu64 "\t%s\t%s\n", insn->pc, count, insn->bytes,
              insn->disas);
      kernel += count;
    }
  }
  fprintf(output, "U\t%" PRIu64 "\n", user);
  GHashTableIter iter;
  void *key, *count;
  g_hash_table_iter_init(&iter, syscalls);
  while (g_hash_table_iter_next(&iter, &key, &count))
    fprintf(output, "S\t%s\t%" PRIu64 "\n", (char*)key, *(uint64_t*)count);
  g_hash_table_iter_init(&iter, discontinuities);
  while (g_hash_table_iter_next(&iter, &key, &count))
    fprintf(output, "D\t%s\t%" PRIu64 "\n", (char*)key, *(uint64_t*)count);
  fprintf(output, "E\t%s\t%" PRIu64 "\t%" PRIu64 "\n", status, kernel, user);
  fflush(output);
}

static void begin(unsigned cpu, void* userdata) {
  (void)userdata;
  if (active || finished || (syscall_number != UINT64_MAX && read_reg(rax) != syscall_number))
    return;
  if (seen++ < skip)
    return;
  /* One vCPU: reset at execution time, including counters from cached TBs. */
  memset(qemu_plugin_scoreboard_find(scoreboard, cpu), 0,
         (MAX_INSTRUCTIONS + 1) * sizeof(uint64_t));
  origin_cr3 = read_reg(cr3);
  origin_rsp = read_reg(rsp);
  active = true;
  fprintf(output, "B\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\n", start, origin_cr3, origin_rsp);
  fflush(output);
}

static void end(unsigned cpu, void* userdata) {
  (void)userdata;
  if (active && read_reg(cr3) == origin_cr3 && read_reg(rsp) == origin_rsp)
    dump(cpu, "complete");
}

static void syscall_exec(unsigned cpu, void* userdata) {
  (void)cpu;
  (void)userdata;
  if (active)
    increment(syscalls, g_strdup_printf("%" PRIu64, read_reg(rax)));
}

static void discon(unsigned cpu, enum qemu_plugin_discon_type type, uint64_t from, uint64_t to,
                   void* userdata) {
  (void)cpu;
  (void)userdata;
  if (active)
    increment(discontinuities, g_strdup_printf("%d\t%" PRIx64 "\t%" PRIx64, type, from, to));
}

static void translate(struct qemu_plugin_tb* tb, void* userdata) {
  (void)userdata;
  for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); ++i) {
    struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
    uint64_t pc = qemu_plugin_insn_vaddr(insn);
    if (pc == start)
      qemu_plugin_register_vcpu_insn_exec_cb(insn, begin, QEMU_PLUGIN_CB_R_REGS, NULL);
    if (pc == stop)
      qemu_plugin_register_vcpu_insn_exec_cb(insn, end, QEMU_PLUGIN_CB_R_REGS, NULL);
    uint8_t bytes[15];
    size_t size = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
    qemu_plugin_u64 counter = user_counter;
    if (pc >= kernel_base) {
      char hex[31] = {0};
      for (size_t j = 0; j < size; ++j)
        snprintf(hex + j * 2, 3, "%02x", bytes[j]);
      char* key = g_strdup_printf("%" PRIx64 ":%s", pc, hex);
      Instruction* entry = g_hash_table_lookup(indexed, key);
      if (!entry) {
        if (instructions->len == MAX_INSTRUCTIONS) {
          fprintf(stderr, "pedigree-profile: instruction capacity exceeded\n");
          abort();
        }
        entry = g_new0(Instruction, 1);
        entry->pc = pc;
        memcpy(entry->bytes, hex, sizeof(hex));
        entry->disas = qemu_plugin_insn_disas(insn);
        entry->counter = (qemu_plugin_u64){scoreboard, instructions->len * sizeof(uint64_t)};
        g_ptr_array_add(instructions, entry);
        g_hash_table_insert(indexed, key, entry);
      } else {
        g_free(key);
      }
      counter = entry->counter;
    } else if (size == 2 && bytes[0] == 0x0f && bytes[1] == 0x05) {
      qemu_plugin_register_vcpu_insn_exec_cb(insn, syscall_exec, QEMU_PLUGIN_CB_R_REGS, NULL);
    }
    qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(insn, QEMU_PLUGIN_INLINE_ADD_U64, counter,
                                                        1);
  }
}

static void init(unsigned cpu, void* userdata) {
  (void)cpu;
  (void)userdata;
  GArray* registers = qemu_plugin_get_registers();
  unsigned found = 0;
  for (unsigned i = 0; i < registers->len; ++i) {
    qemu_plugin_reg_descriptor* reg = &g_array_index(registers, qemu_plugin_reg_descriptor, i);
    if (!strcmp(reg->name, "rax")) {
      rax = reg->handle;
      ++found;
    }
    if (!strcmp(reg->name, "rsp")) {
      rsp = reg->handle;
      ++found;
    }
    if (!strcmp(reg->name, "cr3")) {
      cr3 = reg->handle;
      ++found;
    }
  }
  g_array_free(registers, true);
  if (found != 3)
    abort();
}

static void release(void* data) {
  Instruction* entry = data;
  g_free(entry->disas);
  g_free(entry);
}

static void exited(void* userdata) {
  (void)userdata;
  if (active)
    dump(0, "exit");
  fclose(output);
  g_hash_table_destroy(indexed);
  g_hash_table_destroy(syscalls);
  g_hash_table_destroy(discontinuities);
  g_ptr_array_free(instructions, true);
  g_byte_array_free(value, true);
  qemu_plugin_scoreboard_free(scoreboard);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc,
                                           char** argv) {
  const char* path = NULL;
  if (strcmp(info->target_name, "x86_64") || !info->system_emulation || info->system.max_vcpus != 1)
    return -1;
  for (int i = 0; i < argc; ++i) {
    char** option = g_strsplit(argv[i], "=", 2);
    if (!option[1]) {
      g_strfreev(option);
      return -1;
    }
    if (!strcmp(option[0], "output")) {
      path = strchr(argv[i], '=') + 1;
    } else {
      char* endptr;
      uint64_t n = g_ascii_strtoull(option[1], &endptr, 0);
      if (!*option[1] || *option[1] == '-' || *endptr) {
        g_strfreev(option);
        return -1;
      }
      if (!strcmp(option[0], "start"))
        start = n;
      else if (!strcmp(option[0], "stop"))
        stop = n;
      else if (!strcmp(option[0], "skip"))
        skip = n;
      else if (!strcmp(option[0], "syscall"))
        syscall_number = n;
      else if (!strcmp(option[0], "kernel-base"))
        kernel_base = n;
      else {
        g_strfreev(option);
        return -1;
      }
    }
    g_strfreev(option);
  }
  if (!path || !start || !stop || start == stop)
    return -1;
  output = fopen(path, "wx");
  if (!output) {
    perror("pedigree-profile output");
    return -1;
  }
  fprintf(output,
          "# pedigree-qemu-profile-v1\n# start=%" PRIx64 " stop=%" PRIx64 " kernel_base=%" PRIx64
          " skip=%" PRIu64 "\n",
          start, stop, kernel_base, skip);
  instructions = g_ptr_array_new_with_free_func(release);
  indexed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  syscalls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  discontinuities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  value = g_byte_array_new();
  scoreboard = qemu_plugin_scoreboard_new((MAX_INSTRUCTIONS + 1) * sizeof(uint64_t));
  user_counter = (qemu_plugin_u64){scoreboard, MAX_INSTRUCTIONS * sizeof(uint64_t)};
  qemu_plugin_register_vcpu_init_cb(id, init, NULL);
  qemu_plugin_register_vcpu_tb_trans_cb(id, translate, NULL);
  qemu_plugin_register_vcpu_discon_cb(id, QEMU_PLUGIN_DISCON_ALL, discon, NULL);
  qemu_plugin_register_atexit_cb(id, exited, NULL);
  return 0;
}
