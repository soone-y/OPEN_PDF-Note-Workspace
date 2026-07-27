// file: file_output.h
#pragma once

#include "core/app_core.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// pdf_view TU で実装。services 層からは本ヘッダ経由で参照する。
void InvalidateAnnotHistoryForPath(const std::wstring& path);
bool PromptPasswordAndReopenCurrentPdf(HWND owner, const std::wstring& title,
                                       const std::wstring& blockedMessage);

namespace file_output {

// 数式の処理モード (Math handling mode)
enum class MathMode {
    Raw,          // 生: Keep original delimiters ($...$, $$...$$)
    Placeholder,  // 統一置換: Replace with placeholder string
    Simplified    // 簡易化: Strip delimiters, keep tex content
};

// マークアップの処理モード (Markup handling mode)
enum class MarkupMode {
    Raw,          // 生: Keep original markup syntax
    Placeholder,  // 統一置換: Replace with placeholder
    Simplified    // 簡易化: Strip markup, keep content only
};

// txt出力オプション
struct TextExportOptions {
    MathMode mathMode = MathMode::Raw;
    std::string mathPlaceholder = "[math]";
    // Default is Simplified to preserve historical behavior (export as readable plain text).
    MarkupMode markupMode = MarkupMode::Simplified;
    std::string markupPlaceholder = "[mark]";
    bool includeCommentLines = true;
};

// マークアップ（md/html）出力オプション
struct NoteMarkupExportOptions {
    enum class Format {
        Markdown,
        Html
    };
    Format format = Format::Markdown;

    enum class MathMode {
        Format,       // md: $...$/$$...$$ , html: <span class="math ...">...</span>
        Placeholder   // replace math with plain placeholder string
    };
    MathMode mathMode = MathMode::Format;
    std::string mathPlaceholder = "[math]";
    bool includeCommentLines = true;

    // ノート名を先頭の見出しとして追加する
    bool includeTitleHeading = false;
    // includeTitleHeading のとき、既存の見出しを1段下げる（+1、最大h6）
    bool shiftHeadingLevels = true;
};

struct PdfCropRectPt {
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

struct PdfPageSpec {
    int pageIndex = -1;     // 0-based
    bool withAnnotations = false;
    std::optional<PdfCropRectPt> cropPt;
};

enum class PdfPngStyle {
    PdfLike,
    ViewerLike
};

enum class StagedDiffKind {
    Note,
    Clrop
};

struct StagedNoteRestoreViewState {
    uint64_t contentRevision = 0;
    uint64_t selectionStart = 0;
    uint64_t selectionEnd = 0;
    int scrollX = 0;
    int scrollY = 0;
    int firstVisibleLine = 0;

    bool hasSelection() const { return selectionStart != selectionEnd; }
};

struct StagedDiffEntry {
    StagedDiffKind kind = StagedDiffKind::Note;
    std::filesystem::path stagePath;
    std::filesystem::path metaPath;
    std::filesystem::path destPath;
    std::wstring targetPath;
    std::optional<uint64_t> revision;
    std::optional<StagedNoteRestoreViewState> noteRestoreView;
    bool isLatest = false;
};

struct StagedNoteSnapshotRef {
    std::filesystem::path stagePath;
    uint64_t noteId = 0;
    uint64_t contentRevision = 0;
    uint64_t basePersistenceRevision = 0;
    uint64_t persistenceRevision = 0;
    std::optional<StagedNoteRestoreViewState> restoreView;

    bool valid() const { return !stagePath.empty(); }
};

// Save current note file regardless of dirty state (integrates current editor state to the note file).
bool SaveNoteFile(HWND owner);

// Stage current note and/or annotations into __resource__/__tmp__ if dirty.
// This does NOT overwrite the original note/.clrop; integration is done via IntegrateStaged*.
[[nodiscard]] bool SaveNoteIfDirty(HWND owner);
bool EnsureCurrentNotePathForStage(HWND owner);
[[nodiscard]] bool SaveAnnotationsIfDirty(HWND owner);
void NotifyNoteStageEdit(HWND owner);
void ResetNoteStageSaveTracking(HWND owner);
void ConfigureAutoStageSaveScheduling(HWND owner);
void NotifyAutoStageSaveStep(HWND owner);
void ScheduleAutoStageSaveRetry(HWND owner);
void HandleNoteStageSaveTimer(HWND owner);
void HandleAutoStageSaveTimer(HWND owner);
void HandleDeferredAutoStageSaveTimer(HWND owner);
bool ShouldDeferBackgroundSaveForActiveInput();
bool ShouldAutoIntegrateOnSwitchOrExit();
bool PrepareStagedDiffsForSwitch(HWND owner);
bool MaybeAutoIntegrateOnSwitchOrExit(HWND owner);
void ScheduleIntegrateAfterSwitch(HWND owner);
void ConfigureAutoIntegrateScheduling(HWND owner);
void NotifyAutoIntegrateStageSaved(HWND owner);
void HandleAutoIntegrateTimer(HWND owner);

// Integrate staged note/.clrop diffs into their original files (Ctrl+S / exit / manual).
bool IntegrateStagedNoteAndAnnotations(HWND owner);
bool RunSaveAndIntegrateTransaction(HWND owner);
enum class SaveTransactionStartResult {
    Failed,
    CompletedSynchronously,
    Started,
};
struct BackgroundSaveCompletion {
    bool ok = false;
    bool restarted = false;
    std::wstring error;
    int integratedCount = 0;
};
SaveTransactionStartResult StartBackgroundSaveAndIntegrateTransaction(HWND owner);
BackgroundSaveCompletion CompleteBackgroundSaveAndIntegrateTransaction(HWND owner, void* rawResult);
bool HasAnyStagedDiffs();
bool HasPendingOrStagedDiffsUnderPath(const std::filesystem::path& root);
bool DiscardAllStagedDiffs(HWND owner);
std::vector<StagedDiffEntry> ListStagedDiffEntries();
std::wstring FormatStagedDiffLocationSummary(size_t maxEntries = 5);
bool PromoteStagedDiff(const std::filesystem::path& stagePath);
bool IntegrateStagedDiff(HWND owner, const std::filesystem::path& stagePath, bool forceSkipBackup = false);
bool DiscardStagedDiff(const std::filesystem::path& stagePath);

// Paths under __resource__/__tmp__ used for staging.
std::filesystem::path StagedNotePathFor(const std::wstring& notePath);
std::filesystem::path StagedClropPathForPdf(const std::wstring& pdfPath);
std::optional<StagedNoteSnapshotRef> FindLatestStagedNoteSnapshotFor(
    const std::wstring& notePath);
std::filesystem::path FindLatestStagedNotePathFor(const std::wstring& notePath);
std::filesystem::path FindLatestStagedClropPathForPdf(const std::wstring& pdfPath);
bool LoadResolvedStagedNoteBytes(const std::wstring& notePath,
                                 const std::filesystem::path& stagePath,
                                 std::string* outBytes,
                                 std::wstring* outErr);
bool LoadResolvedStagedAnnotations(const std::wstring& pdfPath,
                                   const std::filesystem::path& stagePath,
                                   std::vector<Annotation>* outAnnotations,
                                   std::wstring* outErr);
void DiscardOtherStagedNoteFilesFor(const std::wstring& notePath, const std::filesystem::path& keepStagePath);
void DiscardOtherStagedClropFilesForPdf(const std::wstring& pdfPath, const std::filesystem::path& keepStagePath);

// Automatic cleanup is allowed only for staged data that resolves exactly to
// the current original file state. Other staged edits are removed only after a
// successful integration or an explicit user discard action.
void DiscardRedundantStagedNoteFilesMatchingOriginal(const std::wstring& notePath);
void DiscardRedundantStagedClropFilesMatchingOriginal(const std::wstring& pdfPath);

// Restore a file from a backup meta file created under __resource__/__escape__/backup.
// Returns the destination path via outDest if provided.
bool RestoreFromBackupMeta(HWND owner, const std::filesystem::path& backupMetaPath, std::filesystem::path* outDest);
bool DeleteBackupMeta(const std::filesystem::path& backupMetaPath, std::wstring* outErr);

// PDF export helpers.
bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations);
bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations, bool standardTextAnnots);
bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations, bool standardTextAnnots, double exportScale);
bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations, bool standardTextAnnots, double exportScale,
                              bool matchPdfPaneTextLayout);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, bool standardTextAnnots);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath, bool standardTextAnnots);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, bool standardTextAnnots, double exportScale);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath, bool standardTextAnnots, double exportScale);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, bool standardTextAnnots, double exportScale,
                    bool matchPdfPaneTextLayout);
bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath,
                    bool standardTextAnnots, double exportScale, bool matchPdfPaneTextLayout);
std::vector<PdfPageSpec> ParsePdfPageSpec(const std::wstring& spec, bool defaultAnnot, std::wstring* err);

// PNG export (single page).
bool ExportPdfPagePng(HWND owner, int pageIndex, PdfPngStyle style, bool includeAnnotations);
bool ExportPdfPagePng(HWND owner, int pageIndex, const std::wstring& outPath, PdfPngStyle style, bool includeAnnotations);
bool ExportPdfPagePng(HWND owner, int pageIndex, PdfPngStyle style, bool includeAnnotations, int outWidthPx, int outHeightPx);
bool ExportPdfPagePng(HWND owner, int pageIndex, const std::wstring& outPath, PdfPngStyle style, bool includeAnnotations, int outWidthPx, int outHeightPx);

// Create a new one-page PDF from a PNG/JPEG without modifying the source image.
// The source pixels and physical DPI determine the PDF page content and size.
bool ConvertImageToPdf(HWND owner, const std::wstring& imagePath, std::wstring* outPath = nullptr);

// Note text exports.
bool ExportNotePlainText(HWND owner, const TextExportOptions& options);
bool ExportNoteMarkup(HWND owner, const NoteMarkupExportOptions& options);
bool ExportNoteHtml(HWND owner);
bool ExportNoteHtml(HWND owner, const NoteMarkupExportOptions& options);
bool ExportNotePlainText(const std::wstring& notePath, const std::wstring& outPath, const TextExportOptions& options);
bool ExportNoteMarkup(const std::wstring& notePath, const std::wstring& outPath, const NoteMarkupExportOptions& options);
bool ExportNoteHtml(const std::wstring& notePath, const std::wstring& outPath);
bool ExportNoteHtml(const std::wstring& notePath, const std::wstring& outPath, const NoteMarkupExportOptions& options);

} // namespace file_output


