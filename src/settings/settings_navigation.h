#pragma once

#include <array>

// Navigation has no HWNDs or persistence side effects. Existing page numbers
// remain stable for public shortcuts and the pointer-free navigation message.
namespace settings_navigation {
enum Page : int {
    Environment = 0, Note = 1, Markup = 2, Annotation = 3, Palette = 4,
    Pdf = 5, Save = 6, QuickPdf = 7, Files = 8, Diagnostics = 9,
    NoteFiles = 10, NoteInput = 11, AnnotationInput = 12,
    Recovery = 13, QuickNote = 14, BottomPane = 15, Count = 16
};
enum Group : int { EnvironmentGroup, PdfGroup, NoteGroup, AnnotationGroup, SaveGroup, OutputGroup, GroupCount };
struct Entry { Page page; Group group; const wchar_t* label; };
inline constexpr std::array<const wchar_t*, GroupCount> groups = {
    L"settings.group.environment", L"settings.group.pdf", L"settings.group.note",
    L"settings.group.annotation", L"settings.group.save", L"settings.group.output"
};
inline constexpr std::array<Entry, Count> entries = {{
    {Environment, EnvironmentGroup, L"settings.page.environment"},
    {Note, NoteGroup, L"settings.page.note"},
    {Markup, NoteGroup, L"settings.page.markup"},
    {Annotation, AnnotationGroup, L"settings.page.annotation"},
    {Palette, EnvironmentGroup, L"settings.page.palette"},
    {Pdf, PdfGroup, L"settings.page.pdf"},
    {Save, SaveGroup, L"settings.page.save"},
    {QuickPdf, OutputGroup, L"settings.page.quick_pdf"},
    {Files, EnvironmentGroup, L"settings.page.files"},
    {Diagnostics, EnvironmentGroup, L"settings.page.diagnostics"},
    {NoteFiles, NoteGroup, L"settings.page.note_files"},
    {NoteInput, NoteGroup, L"settings.page.note_input"},
    {AnnotationInput, AnnotationGroup, L"settings.page.annotation_input"},
    {Recovery, SaveGroup, L"settings.page.recovery"},
    {QuickNote, OutputGroup, L"settings.page.quick_note"},
    {BottomPane, EnvironmentGroup, L"settings.page.bottom_pane"}
}};
inline constexpr std::array<Page, GroupCount> firstPages = {Environment, Pdf, Note, Annotation, Save, QuickPdf};
inline constexpr std::array<Page, Count> sidebarOrder = {Environment, BottomPane, Palette, Files, Diagnostics, Pdf,
    Note, NoteInput, Markup, NoteFiles, Annotation, AnnotationInput, Save, Recovery, QuickPdf, QuickNote};
} // namespace settings_navigation
