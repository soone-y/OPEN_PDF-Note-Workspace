#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <vector>
#include <string>

// Forward declarations
struct Annotation;
extern void MarkAnnotsDirty(HWND owner);

class AnnotationEditGuard {
public:
    explicit AnnotationEditGuard(std::vector<Annotation>& a) : annots(a), before(a) {}

    void Commit(HWND owner, const wchar_t* /*reason*/) {
        committed = true;
        MarkAnnotsDirty(owner);
    }

    ~AnnotationEditGuard() {
        if (!committed) {
            annots = before;
        }
    }

private:
    std::vector<Annotation>& annots;
    std::vector<Annotation> before;
    bool committed = false;
};
