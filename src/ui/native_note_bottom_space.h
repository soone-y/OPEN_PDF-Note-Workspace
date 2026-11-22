#pragma once

#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>

#include "note/note_bottom_space.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace ui {
namespace native_note_space_detail {
struct ReleaseTextObject {
    void operator()(IUnknown* object) const noexcept { if (object) object->Release(); }
};
template<class T> using TextObject = std::unique_ptr<T, ReleaseTextObject>;

[[nodiscard]] inline TextObject<ITextDocument> Document(HWND window) noexcept {
    IRichEditOle* object = nullptr;
    SendMessageW(window, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&object));
    TextObject<IRichEditOle> owner(object);
    if (!owner) return {};
    // ITextDocument's published IID. MinGW's tom.h declares it without a
    // corresponding libuuid definition or __mingw_uuidof specialization.
    constexpr IID documentId{0x8cc497c0, 0xa1df, 0x11ce,
        {0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d}};
    ITextDocument* document = nullptr;
    const HRESULT result = owner->QueryInterface(documentId, reinterpret_cast<void**>(&document));
    TextObject<ITextDocument> value(document);
    return SUCCEEDED(result) ? std::move(value) : TextObject<ITextDocument>{};
}

[[nodiscard]] inline TextObject<ITextRange> Range(ITextDocument& document, LONG first, LONG last) noexcept {
    ITextRange* range = nullptr;
    const HRESULT result = document.Range(first, last, &range);
    TextObject<ITextRange> value(range);
    return SUCCEEDED(result) ? std::move(value) : TextObject<ITextRange>{};
}

[[nodiscard]] inline bool SetSpaceAfter(ITextRange& range, float points) noexcept {
    ITextPara* paragraph = nullptr;
    const HRESULT result = range.GetPara(&paragraph);
    TextObject<ITextPara> owner(paragraph);
    return SUCCEEDED(result) && owner && SUCCEEDED(owner->SetSpaceAfter(points));
}

// Display formatting borrows a TOM range, never the user's selection. Preserve
// the editor's modification flag, undo/redo queues, notifications and viewport.
struct ScopedDisplayFormat {
    HWND window;
    ITextDocument& document;
    LRESULT event_mask;
    LRESULT modified;
    POINT scroll{};
    bool has_scroll;
    bool suspended;
    bool frozen;

    ScopedDisplayFormat(HWND target, ITextDocument& text) noexcept
        : window(target), document(text),
          event_mask(SendMessageW(target, EM_GETEVENTMASK, 0, 0)),
          modified(SendMessageW(target, EM_GETMODIFY, 0, 0)),
          has_scroll(SendMessageW(target, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll)) != 0),
          suspended(SUCCEEDED(text.Undo(tomSuspend, nullptr))), frozen(false) {
        SendMessageW(window, EM_SETEVENTMASK, 0, 0);
        LONG count = 0;
        frozen = SUCCEEDED(document.Freeze(&count));
    }
    ~ScopedDisplayFormat() {
        if (has_scroll) {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_RANGE | SIF_PAGE;
            if (GetScrollInfo(window, SB_VERT, &info)) {
                const int64_t maximum = std::max<int64_t>(0,
                    int64_t{info.nMax} - std::max<int64_t>(0, int64_t{info.nPage} - 1));
                scroll.y = static_cast<LONG>(std::clamp<int64_t>(scroll.y, 0, maximum));
            }
            SendMessageW(window, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        }
        if (suspended) document.Undo(tomResume, nullptr);
        SendMessageW(window, EM_SETMODIFY, static_cast<WPARAM>(modified), 0);
        SendMessageW(window, EM_SETEVENTMASK, 0, event_mask);
        if (frozen) {
            LONG count = 0;
            document.Unfreeze(&count);
        }
    }
};
} // namespace native_note_space_detail

class NativeNoteBottomSpace {
public:
    bool updating = false;

    void Invalidate() noexcept { initialized_ = false; }

    // Raw notes persist plain text. Space-after is a display-only property of
    // the last native paragraph, so the entire client remains paintable.
    [[nodiscard]] bool Apply(HWND window, int padding, int dpi) noexcept {
        if (updating) return true;
        padding = std::max(0, padding);
        if (padding == 0 && padding_px_ == 0) return true;
        updating = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{updating};
        auto document = native_note_space_detail::Document(window);
        if (!document) return false;
        GETTEXTLENGTHEX query{GTL_NUMCHARS | GTL_PRECISE, 1200};
        const LRESULT length = SendMessageW(window, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&query), 0);
        if (length < 0 || length > std::numeric_limits<LONG>::max()) return false;
        const LONG end = static_cast<LONG>(length);
        auto last = native_note_space_detail::Range(*document, end, end);
        LONG start = 0, moved = 0;
        if (!last || FAILED(last->StartOf(tomParagraph, tomMove, &moved)) ||
            FAILED(last->GetStart(&start))) return false;
        // Text edits within this same paragraph retain its spacing. Only a
        // changed paragraph boundary or preference requires native formatting.
        if (initialized_ && start == paragraph_start_ && padding == padding_px_ && dpi == dpi_) return true;
        const LONG clearFrom = initialized_ ? std::min(paragraph_start_, start) : 0;
        auto tail = native_note_space_detail::Range(*document, clearFrom, end);
        if (!tail) return false;
        native_note_space_detail::ScopedDisplayFormat format(window, *document);
        if (!format.suspended) return false;
        // New paragraphs inherit the old last paragraph's spacing. Clear that
        // tail before reserving space after its new final paragraph only.
        initialized_ = false;
        if (!native_note_space_detail::SetSpaceAfter(*tail, 0) ||
            !native_note_space_detail::SetSpaceAfter(*last, padding * 72.0f / std::max(1, dpi))) return false;
        paragraph_start_ = start;
        padding_px_ = padding;
        dpi_ = dpi;
        initialized_ = true;
        return true;
    }

    void RevealCaret(HWND window, int padding, HFONT font) const noexcept {
        if (updating || padding <= 0 || !font) return;
        CHARRANGE selection{};
        SendMessageW(window, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        if (selection.cpMin != selection.cpMax) return;
        POINT scroll{};
        RECT client{};
        POINTL caret{};
        if (!GetClientRect(window, &client)) return;
        SendMessageW(window, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&caret), selection.cpMin);
        if (!SendMessageW(window, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll))) return;
        // RichEdit may return no font from WM_GETFONT. Its owner supplies the
        // actual raw font so measuring a stock DC font cannot move the caret.
        HDC dc = GetDC(window);
        if (!dc) return;
        HGDIOBJ previous = SelectObject(dc, font);
        TEXTMETRICW metrics{};
        const bool measured = previous && previous != HGDI_ERROR && GetTextMetricsW(dc, &metrics);
        if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (!measured) return;
        const uint64_t top = static_cast<uint64_t>(std::max<int64_t>(0, int64_t{scroll.y} + caret.y));
        const uint64_t next = note::RevealCaretWithBottomSpace(std::max<LONG>(0, scroll.y), top,
            top + std::max<LONG>(1, metrics.tmHeight), client.bottom - client.top, padding);
        scroll.y = static_cast<LONG>(std::min<uint64_t>(next, std::numeric_limits<LONG>::max()));
        SendMessageW(window, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    }

private:
    bool initialized_ = false;
    LONG paragraph_start_ = 0;
    int padding_px_ = 0;
    int dpi_ = 0;
};
} // namespace ui
