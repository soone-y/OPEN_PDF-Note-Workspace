#include "note/note_render_final_display_run.h"

#include "note/note_markdown_escape.h"

#include <exception>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool IsValidSpan(Span span, size_t sourceLength) noexcept {
    return span.start <= span.end && span.end.value <= sourceLength;
}

[[nodiscard]] bool SpanContains(Span outer, Span inner) noexcept {
    return outer.start <= inner.start && inner.end <= outer.end;
}

[[nodiscard]] bool AppendBoundary(
    std::vector<NoteRenderFinalDisplayBoundary>* boundaries,
    Utf16CodeUnitOffset sourceOffset,
    size_t displayOffset) {
    if (!boundaries) return false;
    if (!boundaries->empty()) {
        NoteRenderFinalDisplayBoundary& previous = boundaries->back();
        if (sourceOffset < previous.source_offset || displayOffset < previous.display_offset) {
            return false;
        }
        if (sourceOffset == previous.source_offset) {
            previous.display_offset = displayOffset;
            return true;
        }
    }
    boundaries->push_back({sourceOffset, displayOffset});
    return true;
}

[[nodiscard]] bool AppendVisibleText(
    std::wstring_view raw,
    Utf16CodeUnitOffset rawStart,
    bool decodeMarkdownEscapes,
    NoteRenderFinalDisplayRun* out) {
    if (!out) return false;
    size_t displayOffset = out->display_text.size();
    if (!AppendBoundary(&out->boundaries, rawStart, displayOffset)) return false;
    for (size_t index = 0; index < raw.size();) {
        if (rawStart.value > static_cast<size_t>(-1) - index) return false;
        const Utf16CodeUnitOffset sourceBefore{rawStart.value + index};
        if (decodeMarkdownEscapes && IsMarkdownBackslashEscapeAt(raw, index)) {
            if (!AppendBoundary(&out->boundaries, sourceBefore, displayOffset) ||
                !AppendBoundary(&out->boundaries, {sourceBefore.value + 1}, displayOffset)) {
                return false;
            }
            out->display_text.push_back(raw[index + 1]);
            ++displayOffset;
            index += 2;
            if (!AppendBoundary(&out->boundaries, {sourceBefore.value + 2}, displayOffset)) {
                return false;
            }
            continue;
        }
        if (!AppendBoundary(&out->boundaries, sourceBefore, displayOffset)) return false;
        out->display_text.push_back(raw[index]);
        ++displayOffset;
        ++index;
        if (!AppendBoundary(&out->boundaries, {sourceBefore.value + 1}, displayOffset)) {
            return false;
        }
    }
    return true;
}

} // namespace

NoteRenderFinalDisplayRunBuildResult BuildNoteRenderFinalDisplayRun(
    const NoteSyntaxSnapshot& syntax,
    const NoteRenderSourceRun& sourceRun,
    NoteRenderFinalDisplayRun* out) noexcept {
    if (!out) return NoteRenderFinalDisplayRunBuildResult::InvalidOutput;
    if (!syntax.valid()) return NoteRenderFinalDisplayRunBuildResult::InvalidSyntax;
    const size_t sourceLength = syntax.source_root().text_length();
    if (!IsValidSpan(sourceRun.source_span, sourceLength) ||
        !IsValidSpan(sourceRun.display_source_span, sourceLength) ||
        !SpanContains(sourceRun.source_span, sourceRun.display_source_span) ||
        sourceRun.source_span.end <= sourceRun.source_span.start) {
        return NoteRenderFinalDisplayRunBuildResult::InvalidSourceRun;
    }
    try {
        NoteRenderFinalDisplayRun candidate;
        // Markdown syntax markers intentionally have no glyphs in a
        // structured row, but they remain source-addressable. Keep their two
        // endpoint boundaries here instead of leaving GDI/input code to
        // rediscover a hidden prefix or delimiter from raw text.
        if (sourceRun.kind == NoteRenderSourceRunKind::HiddenSyntax) {
            if (sourceRun.display_source_span.start != sourceRun.source_span.start ||
                sourceRun.display_source_span.end != sourceRun.source_span.end ||
                sourceRun.decodes_markdown_escapes ||
                !AppendBoundary(&candidate.boundaries, sourceRun.source_span.start, 0) ||
                !AppendBoundary(&candidate.boundaries, sourceRun.source_span.end, 0)) {
                return NoteRenderFinalDisplayRunBuildResult::InconsistentSource;
            }
            *out = std::move(candidate);
            return NoteRenderFinalDisplayRunBuildResult::Built;
        }
        const std::wstring visibleRaw = syntax.CopySourceRange(
            sourceRun.display_source_span.start,
            sourceRun.display_source_span.end.value - sourceRun.display_source_span.start.value);
        if (visibleRaw.size() !=
            sourceRun.display_source_span.end.value - sourceRun.display_source_span.start.value) {
            return NoteRenderFinalDisplayRunBuildResult::InconsistentSource;
        }
        // Math delimiters are source-addressable but intentionally display at
        // the first/last measured body boundary. Other source runs normally
        // have identical source and display spans, yet this handles any future
        // hidden syntax through the same explicit contract.
        if (!AppendBoundary(&candidate.boundaries, sourceRun.source_span.start, 0) ||
            !AppendBoundary(&candidate.boundaries, sourceRun.display_source_span.start, 0) ||
            !AppendVisibleText(visibleRaw, sourceRun.display_source_span.start,
                               sourceRun.decodes_markdown_escapes, &candidate) ||
            !AppendBoundary(&candidate.boundaries, sourceRun.display_source_span.end,
                            candidate.display_text.size()) ||
            !AppendBoundary(&candidate.boundaries, sourceRun.source_span.end,
                            candidate.display_text.size()) ||
            candidate.boundaries.empty()) {
            return NoteRenderFinalDisplayRunBuildResult::InconsistentSource;
        }
        *out = std::move(candidate);
        return NoteRenderFinalDisplayRunBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalDisplayRunBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalDisplayRunBuildResult::InconsistentSource;
    }
}

} // namespace note
