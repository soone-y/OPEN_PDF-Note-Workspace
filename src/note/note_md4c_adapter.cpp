#include "note/note_md4c_adapter.h"

#include "note/note_markdown_escape.h"

#include "core/app_core.h"

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <vector>

#define MD4C_USE_UTF16
extern "C" {
#include "md4c.h"
}

namespace note {
namespace {

constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

struct RangeAccumulator {
    bool has_span = false;
    Span span{};

    void Absorb(Span value) {
        if (value.end < value.start) {
            value.end = value.start;
        }
        if (!has_span) {
            span = value;
            has_span = true;
            return;
        }
        span.start = std::min(span.start, value.start);
        span.end = std::max(span.end, value.end);
    }
};

struct BlockFrame {
    MD_BLOCKTYPE type = MD_BLOCK_DOC;
    size_t block_index = kInvalidIndex;
    size_t open_cursor = 0;
    RangeAccumulator content{};
};

struct SpanFrame {
    MD_SPANTYPE type = MD_SPAN_EM;
    size_t parent_block = kInvalidIndex;
    size_t open_cursor = 0;
    std::wstring target;
    RangeAccumulator content{};
};

struct ParseContext {
    const NoteTextModel* model = nullptr;
    NoteDocument* out = nullptr;
    size_t cursor = 0;
    bool used_fallback_locator = false;
    std::vector<BlockFrame> block_stack;
    std::vector<SpanFrame> span_stack;
};

std::wstring AttributeToWString(const MD_ATTRIBUTE* attr) {
    if (!attr || !attr->text || attr->size == 0) {
        return L"";
    }
    return std::wstring(attr->text, attr->size);
}

size_t CurrentBlockParent(const ParseContext& ctx) {
    for (auto it = ctx.block_stack.rbegin(); it != ctx.block_stack.rend(); ++it) {
        if (it->block_index != kInvalidIndex) {
            return it->block_index;
        }
    }
    return kInvalidIndex;
}

void AbsorbRange(ParseContext* ctx, Span span) {
    if (!ctx) return;
    for (auto& frame : ctx->block_stack) {
        if (frame.block_index != kInvalidIndex) {
            frame.content.Absorb(span);
        }
    }
    for (auto& frame : ctx->span_stack) {
        frame.content.Absorb(span);
    }
}

void PushTextInline(NoteDocument* out, size_t parentBlock, Span span) {
    if (!out) return;
    if (span.end < span.start) {
        span.end = span.start;
    }
    if (!out->inlines.empty()) {
        InlineNode& tail = out->inlines.back();
        if (tail.kind == InlineKind::Text &&
            tail.parent_block == parentBlock &&
            tail.target.empty() &&
            tail.span.end == span.start) {
            tail.span.end = span.end;
            return;
        }
    }

    InlineNode node;
    node.kind = InlineKind::Text;
    node.span = span;
    node.parent_block = parentBlock;
    out->inlines.push_back(std::move(node));
}

void PushInlineOverlay(NoteDocument* out,
                       InlineKind kind,
                       size_t parentBlock,
                       Span span,
                       std::wstring target = L"") {
    if (!out) return;
    if (span.end < span.start) {
        span.end = span.start;
    }
    InlineNode node;
    node.kind = kind;
    node.span = span;
    node.parent_block = parentBlock;
    node.target = std::move(target);
    out->inlines.push_back(std::move(node));
}

void PushDiagnostic(NoteDocument* out,
                    std::wstring code,
                    std::wstring message,
                    Span span,
                    DiagnosticSeverity severity) {
    if (!out) return;
    Diagnostic diag;
    diag.code = std::move(code);
    diag.message = std::move(message);
    diag.span = span;
    diag.severity = severity;
    out->diagnostics.push_back(std::move(diag));
}

size_t FindNextLineBreak(const std::wstring& raw, size_t cursor) {
    size_t cr = raw.find(L'\r', cursor);
    size_t lf = raw.find(L'\n', cursor);
    if (cr == std::wstring::npos) return lf;
    if (lf == std::wstring::npos) return cr;
    return std::min(cr, lf);
}

size_t SkipLineBreak(const std::wstring& raw, size_t pos) {
    if (pos < raw.size() && raw[pos] == L'\r') {
        if (pos + 1 < raw.size() && raw[pos + 1] == L'\n') return pos + 2;
        return pos + 1;
    }
    if (pos < raw.size() && raw[pos] == L'\n') return pos + 1;
    return pos;
}

bool IsThematicBreakLine(std::wstring_view line) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t')) ++pos;
    if (pos >= line.size()) return false;
    const wchar_t marker = line[pos];
    if (marker != L'-' && marker != L'_' && marker != L'*') return false;

    int count = 0;
    while (pos < line.size()) {
        const wchar_t ch = line[pos++];
        if (ch == marker) {
            ++count;
            continue;
        }
        if (ch == L' ' || ch == L'\t') continue;
        return false;
    }
    return count >= 3;
}

Span FindThematicBreakSpan(ParseContext* ctx) {
    if (!ctx || !ctx->model) return {};
    const std::wstring& raw = ctx->model->raw;
    size_t pos = std::min(ctx->cursor, raw.size());
    for (int guard = 0; guard < 8 && pos <= raw.size(); ++guard) {
        size_t lineEnd = FindNextLineBreak(raw, pos);
        if (lineEnd == std::wstring::npos) lineEnd = raw.size();
        if (IsThematicBreakLine(std::wstring_view(raw).substr(pos, lineEnd - pos))) {
            size_t end = SkipLineBreak(raw, lineEnd);
            ctx->cursor = end;
            return Span{pos, end};
        }
        if (lineEnd >= raw.size()) break;
        pos = SkipLineBreak(raw, lineEnd);
    }
    ctx->used_fallback_locator = true;
    pos = std::min(ctx->cursor, raw.size());
    return Span{pos, pos};
}

TableCellAlign ConvertAlign(MD_ALIGN align) {
    switch (align) {
    case MD_ALIGN_LEFT:
        return TableCellAlign::Left;
    case MD_ALIGN_CENTER:
        return TableCellAlign::Center;
    case MD_ALIGN_RIGHT:
        return TableCellAlign::Right;
    default:
        return TableCellAlign::Default;
    }
}

Span FindTextSpan(ParseContext* ctx,
                  MD_TEXTTYPE type,
                  const MD_CHAR* text,
                  MD_SIZE size) {
    Span span{};
    if (!ctx || !ctx->model) {
        return span;
    }

    const std::wstring& raw = ctx->model->raw;
    const size_t cursor = std::min(ctx->cursor, raw.size());

    if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR) {
        size_t pos = FindNextLineBreak(raw, cursor);
        if (pos == std::wstring::npos) {
            ctx->used_fallback_locator = true;
            span.start = {cursor};
            span.end = {cursor};
            return span;
        }
        span.start = {pos};
        span.end = {pos + 1};
        if (raw[pos] == L'\r' && pos + 1 < raw.size() && raw[pos + 1] == L'\n') {
            span.end = {pos + 2};
        }
        ctx->cursor = span.end.value;
        return span;
    }

    if (size == 0 || !text) {
        span.start = {cursor};
        span.end = {cursor};
        return span;
    }

    const std::wstring_view needle(text, size);
    size_t pos = cursor;
    size_t end = cursor;
    bool found = false;
    for (; pos <= raw.size(); ++pos) {
        if (MatchMarkdownEscapedTextAt(raw, pos, needle, &end)) {
            found = true;
            break;
        }
    }
    if (!found) {
        ctx->used_fallback_locator = true;
        pos = raw.find(needle, cursor);
        if (pos == std::wstring::npos) pos = cursor;
        end = std::min(pos + static_cast<size_t>(size), raw.size());
    }

    span.start = {pos};
    span.end = {end};
    ctx->cursor = span.end.value;
    return span;
}

bool IsSupportedBlock(MD_BLOCKTYPE type) {
    switch (type) {
    case MD_BLOCK_DOC:
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
    case MD_BLOCK_H:
    case MD_BLOCK_CODE:
    case MD_BLOCK_TABLE:
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
    case MD_BLOCK_TR:
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
    case MD_BLOCK_P:
        return true;
    default:
        return false;
    }
}

bool IsSupportedSpan(MD_SPANTYPE type) {
    switch (type) {
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
    case MD_SPAN_A:
    case MD_SPAN_IMG:
    case MD_SPAN_CODE:
    case MD_SPAN_DEL:
        return true;
    default:
        return false;
    }
}

void BeginBlock(ParseContext* ctx, MD_BLOCKTYPE type, void* detail) {
    if (!ctx || !ctx->out || !ctx->model) return;

    BlockFrame frame;
    frame.type = type;
    frame.open_cursor = std::min(ctx->cursor, ctx->model->raw.size());

    if (type == MD_BLOCK_DOC) {
        ctx->block_stack.push_back(std::move(frame));
        return;
    }

    if (!IsSupportedBlock(type)) {
        PushDiagnostic(ctx->out,
                       L"NOTE-W-MD4C-BLOCK",
                       L"Unsupported Markdown block was ignored during MD4C note parsing.",
                       Span{frame.open_cursor, frame.open_cursor},
                       DiagnosticSeverity::Warning);
        ctx->block_stack.push_back(std::move(frame));
        return;
    }

    BlockNode node;
    node.origin = BlockOrigin::Markdown;
    node.span = Span{frame.open_cursor, frame.open_cursor};
    node.loc = ResolveLineColumn(*ctx->model, frame.open_cursor);
    node.first_inline = ctx->out->inlines.size();
    node.parent = CurrentBlockParent(*ctx);

    switch (type) {
    case MD_BLOCK_QUOTE:
        node.kind = BlockKind::Quote;
        break;
    case MD_BLOCK_UL:
        node.kind = BlockKind::List;
        node.ordered = false;
        break;
    case MD_BLOCK_OL: {
        node.kind = BlockKind::List;
        node.ordered = true;
        const auto* ol = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
        if (ol) node.start_number = static_cast<int>(ol->start);
        break;
    }
    case MD_BLOCK_LI:
        node.kind = BlockKind::ListItem;
        if (const auto* li = static_cast<const MD_BLOCK_LI_DETAIL*>(detail)) {
            node.task_item = (li->is_task != 0);
            node.task_checked = node.task_item && (li->task_mark == L'x' || li->task_mark == L'X');
        }
        break;
    case MD_BLOCK_HR:
        node.kind = BlockKind::HorizontalRule;
        node.span = FindThematicBreakSpan(ctx);
        node.loc = ResolveLineColumn(*ctx->model, node.span.start.value);
        break;
    case MD_BLOCK_H: {
        node.kind = BlockKind::Heading;
        const auto* heading = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
        if (heading) node.level = static_cast<int>(heading->level);
        break;
    }
    case MD_BLOCK_CODE: {
        node.kind = BlockKind::CodeBlock;
        const auto* code = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
        if (code && code->lang.text && code->lang.size > 0) {
            node.info_string = AttributeToWString(&code->lang);
        } else if (code && code->info.text && code->info.size > 0) {
            node.info_string = AttributeToWString(&code->info);
        }
        break;
    }
    case MD_BLOCK_TABLE: {
        node.kind = BlockKind::Table;
        const auto* table = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail);
        if (table) node.table_column_count = static_cast<int>(table->col_count);
        break;
    }
    case MD_BLOCK_THEAD:
        node.kind = BlockKind::TableHead;
        break;
    case MD_BLOCK_TBODY:
        node.kind = BlockKind::TableBody;
        break;
    case MD_BLOCK_TR:
        node.kind = BlockKind::TableRow;
        break;
    case MD_BLOCK_TH: {
        node.kind = BlockKind::TableHeaderCell;
        const auto* cell = static_cast<const MD_BLOCK_TD_DETAIL*>(detail);
        if (cell) node.table_cell_align = ConvertAlign(cell->align);
        break;
    }
    case MD_BLOCK_TD: {
        node.kind = BlockKind::TableCell;
        const auto* cell = static_cast<const MD_BLOCK_TD_DETAIL*>(detail);
        if (cell) node.table_cell_align = ConvertAlign(cell->align);
        break;
    }
    case MD_BLOCK_P:
        node.kind = BlockKind::Paragraph;
        break;
    default:
        break;
    }

    ctx->out->blocks.push_back(std::move(node));
    frame.block_index = ctx->out->blocks.size() - 1;
    ctx->block_stack.push_back(std::move(frame));
}

int EnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    BeginBlock(static_cast<ParseContext*>(userdata), type, detail);
    return 0;
}

int LeaveBlock(MD_BLOCKTYPE, void*, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (!ctx || ctx->block_stack.empty() || !ctx->out || !ctx->model) return 0;

    BlockFrame frame = ctx->block_stack.back();
    ctx->block_stack.pop_back();

    if (frame.block_index == kInvalidIndex) {
        return 0;
    }

    BlockNode& node = ctx->out->blocks[frame.block_index];
    Span blockSpan = frame.content.has_span
                         ? frame.content.span
                         : Span{frame.open_cursor, std::min(ctx->cursor, ctx->model->raw.size())};
    if (blockSpan.end < blockSpan.start) {
        blockSpan.end = blockSpan.start;
    }
    node.span = blockSpan;
    node.loc = ResolveLineColumn(*ctx->model, blockSpan.start.value);
    node.inline_count = ctx->out->inlines.size() - node.first_inline;
    return 0;
}

int EnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (!ctx || !ctx->out || !ctx->model) return 0;

    SpanFrame frame;
    frame.type = type;
    frame.parent_block = CurrentBlockParent(*ctx);
    frame.open_cursor = std::min(ctx->cursor, ctx->model->raw.size());

    if (!IsSupportedSpan(type)) {
        PushDiagnostic(ctx->out,
                       L"NOTE-W-MD4C-SPAN",
                       L"Unsupported Markdown span was ignored during MD4C note parsing.",
                       Span{frame.open_cursor, frame.open_cursor},
                       DiagnosticSeverity::Warning);
        ctx->span_stack.push_back(std::move(frame));
        return 0;
    }

    if (type == MD_SPAN_A) {
        const auto* link = static_cast<const MD_SPAN_A_DETAIL*>(detail);
        if (link) {
            frame.target = AttributeToWString(&link->href);
        }
    } else if (type == MD_SPAN_IMG) {
        const auto* image = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
        if (image) {
            frame.target = AttributeToWString(&image->src);
        }
    }

    ctx->span_stack.push_back(std::move(frame));
    return 0;
}

int LeaveSpan(MD_SPANTYPE, void*, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (!ctx || ctx->span_stack.empty() || !ctx->model || !ctx->out) return 0;

    SpanFrame frame = ctx->span_stack.back();
    ctx->span_stack.pop_back();

    if (!IsSupportedSpan(frame.type)) {
        return 0;
    }

    Span span = frame.content.has_span
                    ? frame.content.span
                    : Span{frame.open_cursor, std::min(ctx->cursor, ctx->model->raw.size())};
    if (span.end < span.start) {
        span.end = span.start;
    }

    switch (frame.type) {
    case MD_SPAN_EM:
        PushInlineOverlay(ctx->out, InlineKind::Emphasis, frame.parent_block, span);
        break;
    case MD_SPAN_STRONG:
        PushInlineOverlay(ctx->out, InlineKind::Strong, frame.parent_block, span);
        break;
    case MD_SPAN_A:
        PushInlineOverlay(ctx->out, InlineKind::Link, frame.parent_block, span, std::move(frame.target));
        break;
    case MD_SPAN_IMG:
        PushInlineOverlay(ctx->out, InlineKind::Image, frame.parent_block, span, std::move(frame.target));
        break;
    case MD_SPAN_CODE:
        PushInlineOverlay(ctx->out, InlineKind::Code, frame.parent_block, span);
        break;
    case MD_SPAN_DEL:
        PushInlineOverlay(ctx->out, InlineKind::Strike, frame.parent_block, span);
        break;
    default:
        break;
    }

    return 0;
}

int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (!ctx || !ctx->out) return 0;

    const size_t parentBlock = CurrentBlockParent(*ctx);
    const Span span = FindTextSpan(ctx, type, text, size);
    if (span.end > span.start) {
        PushTextInline(ctx->out, parentBlock, span);
        AbsorbRange(ctx, span);
    }
    return 0;
}

void DebugLog(const char* msg, void* userdata) {
    auto* ctx = static_cast<ParseContext*>(userdata);
    if (!ctx || !ctx->out || !msg) return;

    std::string messageUtf8(msg);
    PushDiagnostic(ctx->out,
                   L"NOTE-W-MD4C-DEBUG",
                   std::wstring(L"MD4C: ") + UTF8ToWide(messageUtf8),
                   Span{std::min(ctx->cursor, ctx->model ? ctx->model->raw.size() : 0),
                        std::min(ctx->cursor, ctx->model ? ctx->model->raw.size() : 0)},
                   DiagnosticSeverity::Warning);
}

} // namespace

NoteDocument ParseNoteDocumentWithMd4c(const NoteTextModel& model) {
    NoteDocument out;
    out.meta = model.meta;
    SetNoteDocumentSourceRevision(&out, model.revision);

    ParseContext ctx;
    ctx.model = &model;
    ctx.out = &out;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_NOHTML |
                   MD_FLAG_NOINDENTEDCODEBLOCKS |
                   MD_FLAG_STRIKETHROUGH |
                   MD_FLAG_TABLES |
                   MD_FLAG_TASKLISTS;
    parser.enter_block = EnterBlock;
    parser.leave_block = LeaveBlock;
    parser.enter_span = EnterSpan;
    parser.leave_span = LeaveSpan;
    parser.text = Text;
    parser.debug_log = DebugLog;
    parser.syntax = nullptr;

    const int rc = md_parse(model.raw.data(),
                            static_cast<MD_SIZE>(model.raw.size()),
                            &parser,
                            &ctx);

    if (rc != 0) {
        PushDiagnostic(&out,
                       L"NOTE-E-MD4C-PARSE",
                       L"MD4C parsing did not complete successfully.",
                       Span{0, 0},
                       DiagnosticSeverity::Error);
    }

    if (ctx.used_fallback_locator) {
        PushDiagnostic(&out,
                       L"NOTE-W-MD4C-RANGE",
                       L"Some Markdown source ranges were reconstructed with fallback matching.",
                       Span{0, 0},
                       DiagnosticSeverity::Warning);
    }

    return out;
}

} // namespace note
