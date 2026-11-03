#include "mixed.h"
Context first, second;
Context& GetContext(int key) { if (key) return second; return first; }
int& Typing() { return GetContext(0).typing; }
#define g_typing Typing()
#include "editing.cppinc"
#include "display.cppinc"
// Thin orchestration: different callees do not make this two implementation owners.
void Dispatch() { Persist(1); Paint(2); }
void Complex(int n) {
    const char* text = "if && ?";
    if (n > 0 && n < 8) {
        for (int i = 0; i < n; ++i) {
            if (i) Paint(i); else if (n) Edit(n);
        }
    }
    switch(n) { case 1: case 2: Paint(n); break; default: break; }
    auto nested = [] { if (true) Paint(3); };
}
void Objects() { first.scroll = 1; second.scroll = 2; }
void LocalOnly() { Context local; local.scroll = 9; }
int shared_value = 0;
int* const fixed_pointer = &shared_value;
void MayMutate(int& value);
void ReadOnly(const int& value);
void Indirect() { MayMutate(shared_value); ReadOnly(shared_value); *fixed_pointer = 2; }
void (*callback)() = nullptr;
void CallIndirect() { if (callback) callback(); }
#include "observations.cppinc"
