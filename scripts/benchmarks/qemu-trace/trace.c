/* SPDX-License-Identifier: ISC */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

#if QEMU_PLUGIN_VERSION != 7
#error This plugin requires QEMU plugin API 7
#endif

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
  uint64_t pc;
  size_t size;
  char bytes[31];
  char* disas;
} Instruction;

static FILE* output;
static GPtrArray* instructions;
static GByteArray* value;
static struct qemu_plugin_register *rax, *rsp, *cr3, *flags;
static struct qemu_plugin_scoreboard* scoreboard;
static qemu_plugin_u64 enabled;
static uint64_t start, stop, skip = 128, requested = 8, limit = 100000;
static uint64_t syscall_number = UINT64_MAX;
static uint64_t seen, trace, sequence, return_pc, user_rsp, user_cr3;
static bool active;

static uint64_t read_reg(struct qemu_plugin_register* reg) {
  g_byte_array_set_size(value, 0);
  if (!qemu_plugin_read_register(reg, value) || !value->len || value->len > 8) {
    fprintf(stderr, "pedigree-trace: register read failed\n");
    abort();
  }
  uint64_t result = 0;
  for (unsigned i = 0; i < value->len; ++i)
    result |= (uint64_t)value->data[i] << (8 * i);
  return result;
}

static void finish(unsigned cpu, const char* status, uint64_t pc, uint64_t result) {
  fprintf(output, "E\t%" PRIu64 "\t%s\t%" PRIu64 "\t%" PRIx64 "\t%" PRIx64 "\n", trace, status,
          sequence, pc, result);
  fflush(output);
  active = false;
  qemu_plugin_u64_set(enabled, cpu, 0);
}

static void begin(unsigned cpu, void* userdata) {
  Instruction* insn = userdata;
  if (active || trace >= requested)
    return;
  if (syscall_number != UINT64_MAX && read_reg(rax) != syscall_number)
    return;
  if (seen++ < skip)
    return;
  ++trace;
  sequence = 0;
  active = true;
  return_pc = stop ? stop : insn->pc + insn->size;
  user_rsp = read_reg(rsp);
  user_cr3 = read_reg(cr3);
  fprintf(output, "B\t%" PRIu64 "\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\n", trace,
          insn->pc, return_pc, user_rsp, user_cr3);
  qemu_plugin_u64_set(enabled, cpu, 1);
}

static void execute(unsigned cpu, void* userdata) {
  Instruction* insn = userdata;
  uint64_t stack = read_reg(rsp), address_space = read_reg(cr3);
  /* A different thread's SYSRET must not close the originating call. */
  if (insn->pc == return_pc && sequence &&
      (syscall_number == UINT64_MAX || (stack == user_rsp && address_space == user_cr3))) {
    finish(cpu, "complete", insn->pc, read_reg(rax));
    return;
  }
  if (sequence == limit) {
    finish(cpu, "limit", insn->pc, read_reg(rax));
    return;
  }
  fprintf(
      output,
      "I\t%" PRIu64 "\t%" PRIu64 "\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\t%" PRIx64 "\t%s\t%s\n",
      trace, ++sequence, insn->pc, stack, address_space, read_reg(flags), insn->bytes, insn->disas);
}

static void discontinuity(unsigned cpu, enum qemu_plugin_discon_type type, uint64_t from,
                          uint64_t to, void* userdata) {
  (void)cpu;
  (void)userdata;
  if (active)
    fprintf(output, "D\t%" PRIu64 "\t%" PRIu64 "\t%d\t%" PRIx64 "\t%" PRIx64 "\n", trace, sequence,
            type, from, to);
}

static void translate(struct qemu_plugin_tb* tb, void* userdata) {
  (void)userdata;
  for (size_t i = 0; i < qemu_plugin_tb_n_insns(tb); ++i) {
    struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
    Instruction* entry = g_new0(Instruction, 1);
    uint8_t bytes[15];
    entry->pc = qemu_plugin_insn_vaddr(insn);
    entry->size = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
    for (size_t j = 0; j < entry->size; ++j)
      snprintf(entry->bytes + 2 * j, 3, "%02x", bytes[j]);
    entry->disas = qemu_plugin_insn_disas(insn);
    g_ptr_array_add(instructions, entry);
    if (entry->pc == start) {
      if (syscall_number != UINT64_MAX &&
          (entry->size != 2 || bytes[0] != 0x0f || bytes[1] != 0x05)) {
        fprintf(stderr, "pedigree-trace: start is not SYSCALL\n");
        abort();
      }
      qemu_plugin_register_vcpu_insn_exec_cb(insn, begin, QEMU_PLUGIN_CB_R_REGS, entry);
    }
    qemu_plugin_register_vcpu_insn_exec_cond_cb(insn, execute, QEMU_PLUGIN_CB_R_REGS,
                                                QEMU_PLUGIN_COND_NE, enabled, 0, entry);
  }
}

static void init(unsigned cpu, void* userdata) {
  (void)cpu;
  (void)userdata;
  GArray* registers = qemu_plugin_get_registers();
  unsigned found = 0;
  for (unsigned i = 0; i < registers->len; ++i) {
    qemu_plugin_reg_descriptor* reg = &g_array_index(registers, qemu_plugin_reg_descriptor, i);
    fprintf(output, "# register %s\n", reg->name);
    if (!strcmp(reg->name, "rax")) {
      rax = reg->handle;
      ++found;
    } else if (!strcmp(reg->name, "rsp")) {
      rsp = reg->handle;
      ++found;
    } else if (!strcmp(reg->name, "cr3")) {
      cr3 = reg->handle;
      ++found;
    } else if (!strcmp(reg->name, "eflags")) {
      flags = reg->handle;
      ++found;
    }
  }
  g_array_free(registers, true);
  fflush(output);
  if (found != 4) {
    fprintf(stderr, "pedigree-trace: required x86 registers unavailable\n");
    abort();
  }
}

static void release(void* data) {
  Instruction* insn = data;
  g_free(insn->disas);
  g_free(insn);
}

static void exited(void* userdata) {
  (void)userdata;
  if (active)
    finish(0, "exit", 0, 0);
  fprintf(output,
          "# finished traces=%" PRIu64 " requested=%" PRIu64 " matching_starts=%" PRIu64 "\n",
          trace, requested, seen);
  fclose(output);
  g_ptr_array_free(instructions, true);
  g_byte_array_free(value, true);
  qemu_plugin_scoreboard_free(scoreboard);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc,
                                           char** argv) {
  const char* path = NULL;
  if (strcmp(info->target_name, "x86_64") || !info->system_emulation ||
      info->system.max_vcpus != 1) {
    fprintf(stderr, "pedigree-trace: requires x86_64 system TCG, one vCPU\n");
    return -1;
  }
  for (int i = 0; i < argc; ++i) {
    char* equal = strchr(argv[i], '=');
    if (!equal)
      return -1;
    size_t key_length = equal - argv[i];
    const char* arg = equal + 1;
    if (key_length == 6 && !strncmp(argv[i], "output", 6)) {
      path = arg;
      continue;
    }
    char* end;
    uint64_t number = g_ascii_strtoull(arg, &end, 0);
    if (!*arg || *end || *arg == '-')
      return -1;
#define OPTION(name, variable)                                               \
  if (key_length == sizeof(name) - 1 && !strncmp(argv[i], name, key_length)) \
  variable = number
    OPTION("start", start);
    else OPTION("stop", stop);
    else OPTION("skip", skip);
    else OPTION("count", requested);
    else OPTION("limit", limit);
    else OPTION("syscall", syscall_number);
    else return -1;
#undef OPTION
  }
  if (!path || !start || !requested || !limit || (syscall_number == UINT64_MAX && !stop))
    return -1;
  /* Refuse to silently replace the evidence from an earlier run. */
  output = fopen(path, "wx");
  if (!output) {
    perror("pedigree-trace output");
    return -1;
  }
  setvbuf(output, NULL, _IOFBF, 1024 * 1024);
  fprintf(output,
          "# pedigree-qemu-trace-v1\n# api=%d start=%" PRIx64 " skip=%" PRIu64 " count=%" PRIu64
          " limit=%" PRIu64 " syscall=%" PRIu64 "\n",
          QEMU_PLUGIN_VERSION, start, skip, requested, limit, syscall_number);
  fflush(output);
  instructions = g_ptr_array_new_with_free_func(release);
  value = g_byte_array_new();
  scoreboard = qemu_plugin_scoreboard_new(sizeof(uint64_t));
  enabled = qemu_plugin_scoreboard_u64(scoreboard);
  qemu_plugin_register_vcpu_init_cb(id, init, NULL);
  qemu_plugin_register_vcpu_tb_trans_cb(id, translate, NULL);
  qemu_plugin_register_vcpu_discon_cb(id, QEMU_PLUGIN_DISCON_ALL, discontinuity, NULL);
  qemu_plugin_register_atexit_cb(id, exited, NULL);
  return 0;
}
