#ifndef SIGNAL_EXEC_CONTRACTS_H
#define SIGNAL_EXEC_CONTRACTS_H

long long test_milliseconds(void);
int test_wait_flag(volatile int* flag);
void test_pause(void);

int signal_contract(const char* name);
int exec_contract(const char* program, const char* name);
int exec_image_contract(int argc, char* argv[]);

#endif
