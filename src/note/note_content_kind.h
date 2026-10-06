#pragma once

#include <cstdint>

namespace note {

// Source interpretation is an input policy shared by the canonical core,
// syntax snapshot, persistence, and view adapters. It is intentionally not a
// LocalNoteKernel-owned type: immutable snapshots must be constructible and
// verifiable without depending on mutable kernel implementation details.
enum class NoteContentKind : uint8_t {
    PlainText,
    TeXSource,
    Markdown,
};

} // namespace note
