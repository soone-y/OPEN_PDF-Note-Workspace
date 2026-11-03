// Function-pointer/global registration and an unreachable recursive component.
static void FragmentCallback();
#include "unused_fragment.cppinc"
void InvokeFragment() { FragmentCallback(); }
static void TableCallback() {}
void (*table_callback)() = &TableCallback;
static void PassedCallback() {}
void Register(void (*)());
void PlatformEntry() { Register(&PassedCallback); }
static void UnusedLeaf() {}
static void DeadSecond();
static void DeadFirst() { DeadSecond(); }
static void DeadSecond() { DeadFirst(); }
static void UsedLeaf() {}
void PublicApi() { UsedLeaf(); }
struct DispatchBase {
    virtual void Method() {}
    virtual ~DispatchBase() {}
};
template<class T> void UninstantiatedTemplate(T) {}
