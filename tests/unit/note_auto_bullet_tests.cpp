#include "note/note_auto_bullet.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
int checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

void Prefix(const std::wstring& text, const std::wstring& prefix,
            const std::wstring& continuation, bool structural, bool content,
            size_t padding = 0, bool policy = true) {
    // Parsing a physical line must not depend on the caret's column.
    for (size_t caret = 0; caret <= text.size(); ++caret) {
        const auto result = note::AnalyzeAutoBulletLine(text, caret, policy);
        Check(result.valid, "valid prefix");
        Check(result.lineStart == 0 && result.lineEnd == text.size(), "line bounds");
        Check(result.prefix == prefix && result.prefixEnd == prefix.size(), "original prefix");
        Check(result.continuationPrefix == continuation, "continuation prefix");
        Check(result.structuralPrefix == structural && result.hasContent == content, "prefix classification");
        Check(result.orderedPaddingCount == padding, "ordered alignment padding");
        Check(result.markerPos <= result.prefixEnd, "marker lies within prefix");
    }
}

void RunChecks() {
    Prefix(L"- text", L"- ", L"- ", true, true);
    Prefix(L"*\ttext", L"*\t", L"*\t", true, true);
    Prefix(L"+   text", L"+   ", L"+   ", true, true);
    Prefix(L"  - item", L"  - ", L"  - ", true, true);
    Prefix(L"- ", L"- ", L"- ", true, false);
    Prefix(L"\t  ", L"\t  ", L"\t  ", false, false);
    Prefix(L"  ordinary", L"  ", L"  ", false, true);
    Prefix(L"> quote", L"> ", L"> ", true, true);
    Prefix(L">", L">", L">", true, false);
    Prefix(L" > > - item", L" > > - ", L" > > - ", true, true);
    Prefix(L"> \t", L"> \t", L"> \t", true, false);
    Prefix(L"- [x] task", L"- [x] ", L"- [ ] ", true, true);
    Prefix(L"+ [X]\ttask", L"+ [X]\t", L"+ [ ] ", true, true);
    Prefix(L"* [ ] ", L"* [ ] ", L"* [ ] ", true, false);
    Prefix(L"- [q] text", L"- ", L"- ", true, true);
    Prefix(L"- [x]", L"- ", L"- ", true, true);
    Prefix(L"1. item", L"1. ", L"2. ", true, true);
    Prefix(L"8) item", L"8) ", L"9) ", true, true);
    Prefix(L"9. item", L"9. ", L"10. ", true, true, 1);
    Prefix(L"99) item", L"99) ", L"100) ", true, true, 1);
    Prefix(L"9.   item", L"9.   ", L"10.  ", true, true);
    Prefix(L"009. item", L"009. ", L"10.  ", true, true);
    Prefix(L"0. item", L"0. ", L"2. ", true, true);
    Prefix(L"9999999999. item", L"9999999999. ", L"1000000000. ", true, true);
    Prefix(L"> 9. [X] item", L"> 9. [X] ", L"> 10. [ ] ", true, true, 1);
    Prefix(L"1.\titem", L"1.\t", L"2.\t", true, true);
    Prefix(L"1. \t item", L"1. \t ", L"2. \t ", true, true);
    // Preserve the existing caller-controlled suppression, including checklist behavior.
    Prefix(L"1.\titem", L"1.\t", L"", true, true, 0, false);
    Prefix(L"1.\t[x] item", L"1.\t[x] ", L"[ ] ", true, true, 0, false);
    Prefix(L"9. item", L"9. ", L"10. ", true, true, 1, false);
    Prefix(L"- \u3000", L"- ", L"- ", true, true);
    Prefix(L"- \u65e5\u672c\U0001f600", L"- ", L"- ", true, true);
    for (const auto* text : {L"", L"plain", L"-", L"*text", L"-\u3000text", L"\u3000- item",
                             L"1.text", L"1.", L"1] text", L"[x] item", L"\uff11. item"}) {
        const auto result = note::AnalyzeAutoBulletLine(text, std::wstring(text).size(), true);
        Check(!result.valid, "malformed or plain line rejected");
    }
    const auto aligned = note::AnalyzeAutoBulletLine(L"> 9. item", 9, true);
    Check(aligned.markerPos == 2 && aligned.orderedPaddingInsertPos == 5, "quoted ordered insertion positions");
    for (const auto* newline : {L"\r", L"\n", L"\r\n"}) {
        const std::wstring text = std::wstring(L"first") + newline + L"  - item";
        const size_t start = 5 + std::wstring(newline).size();
        for (size_t caret = start; caret <= text.size() + 2; ++caret) {
            const auto result = note::AnalyzeAutoBulletLine(text, caret, true);
            Check(result.valid && result.lineStart == start && result.lineEnd == text.size(), "multiline bounds and end clamp");
            Check(result.markerPos == start + 2 && result.prefixEnd == start + 4, "absolute prefix positions");
            Check(note::FindNoteLineStart(text, caret) == start && note::FindNoteLineEnd(text, caret) == text.size(), "line helpers");
        }
    }
    const std::wstring crlf = L"- a\r\n+ b";
    Check(note::AnalyzeAutoBulletLine(crlf, 3, true).prefix == L"- ", "caret on CR belongs to prior line");
    Check(!note::AnalyzeAutoBulletLine(crlf, 4, true).valid, "caret between CR and LF is empty");
    Check(note::AnalyzeAutoBulletLine(crlf, 5, true).prefix == L"+ ", "caret after CRLF belongs to next line");
    Check(!note::AnalyzeAutoBulletLine(L"- a\n", 4, true).valid, "trailing empty line");
    Check(note::AnalyzeAutoBulletLine(L"- a", std::numeric_limits<size_t>::max(), true).valid, "oversized caret clamped");
    Check(note::FindNoteLineStart(L"", 99) == 0 && note::FindNoteLineEnd(L"", 99) == 0, "empty line helpers");
    for (const auto* text : {L"- item", L"\t* item", L"  + ", L"0. item", L"9999999999) item"})
        Check(note::IsMarkdownListLine(text, 0, std::wstring(text).size()), "direct list for indentation");
    for (const auto* text : {L"plain", L"", L"> - item", L"-", L"1.item", L"\u3000- item"})
        Check(!note::IsMarkdownListLine(text, 0, std::wstring(text).size()), "non-direct list for indentation");
    for (wchar_t ch : std::wstring(L" \t\u3000-+*0129x\n")) {
        Check(note::IsNoteBulletIndentChar(ch) == (ch == L' ' || ch == L'\t'), "indent alphabet");
        Check(note::IsMarkdownBulletMarker(ch) == (ch == L'-' || ch == L'+' || ch == L'*'), "bullet alphabet");
        Check(note::IsMarkdownOrderedListDigit(ch) == (ch >= L'0' && ch <= L'9'), "digit alphabet");
    }
}
}

int main() {
    try {
        RunChecks();
        std::cout << "PASS note auto bullet: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL check " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
