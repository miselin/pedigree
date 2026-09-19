extern "C" __attribute__((noreturn)) void _start() {
  __asm__ volatile("syscall" : : "a"(60L), "D"(0L) : "rcx", "r11", "memory");
  __builtin_unreachable();
}
