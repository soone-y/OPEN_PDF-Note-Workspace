/* Test-only MD4C allocation injection; never linked into the application. */
#define main md4c_allocator_fixture_main
#include "md4c_allocation_failure_tests.c"
#undef main

void note_md4c_fault_reset(size_t fail_at) { reset(fail_at, 0); }
size_t note_md4c_fault_allocations(void) { return allocation_count; }
size_t note_md4c_fault_live(void) { return live_count; }
int note_md4c_fault_injected(void) { return injected; }
