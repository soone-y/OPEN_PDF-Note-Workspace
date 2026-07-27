
// file: file_output.cpp
#include "file_output/file_output.h"
#include "core/font_list.h"
#include "math/math_render.h"
#include "clrop/bridge.h"
#include "file_output/note_snapshot.h"
#include "note/note_export.h"
#include "note/note_workspace_service.h"
#include "note_view/note_view.h"
#include "fpdf_save.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_transformpage.h"
#include "core/atomic_write.h"
#include "core/fault_injection.h"
#include "core/preview_trace.h"
#include "core/ui_notify.h"
#include "core/ui_prompts.h"
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <unordered_map>
#include <wincodec.h>
#include <objbase.h>

namespace {

template <class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

static constexpr int kTextPadPx = 4;
static constexpr double kExportStandardTextMinPt = 9.0;
static constexpr double kMarkerTextAlphaScaleOnText = 0.4;
static constexpr int kMarkerTextDarkMax = 210;

static HWND FileOutputNoticeOwner(HWND owner) {
    return owner ? owner : g_hMainWnd;
}

static constexpr unsigned long kPdfPermissionCopyContent = 0x10u;

static void ShowFileOutputSoftNotice(HWND owner, const std::wstring& text, SoftNoticeKind kind) {
    ShowSoftNotice(FileOutputNoticeOwner(owner), text, kind);
}

static void ShowFileOutputMessageDialog(HWND owner, const std::wstring& title,
                                        const std::wstring& message, SoftNoticeKind kind) {
    ShowSilentMessageDialog(FileOutputNoticeOwner(owner), title, message, kind);
}

static std::wstring ExperimentalExportDialogTitle(const std::wstring& base) {
    if (base.find(L"試験的") != std::wstring::npos ||
        base.find(L"Experimental") != std::wstring::npos) {
        return base;
    }
    return IsEnglishUi() ? (base + L" (Experimental)") : (base + L"（試験的）");
}

static void WarnExportOverwriteOriginal(HWND owner, bool isPdf) {
    ShowFileOutputSoftNotice(
        owner,
        isPdf ? L"元のPDFファイルには上書きできません。別名で保存してください。"
              : L"元のノートファイルには上書きできません。別名で保存してください。",
        SoftNoticeKind::Warning);
}

static std::optional<std::wstring> GetProtectedPdfExportBlockMessage(FPDF_DOCUMENT doc) {
    if (!doc) return std::nullopt;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    const int secRev = FPDF_GetSecurityHandlerRevision(doc);
    if (secRev < 0) return std::nullopt;

    const unsigned long userPerms = FPDF_GetDocUserPermissions(doc);
    const bool canCopy = userPerms == 0xfffffffful || (userPerms & kPdfPermissionCopyContent) != 0;

    if (IsEnglishUi()) {
        std::wstring message =
            L"This PDF has author/publisher protection settings.\n"
            L"To follow the original rights holder's instructions and usage conditions, "
            L"this app does not export copied PDFs from protected source PDFs, including annotated copies.";
        if (!canCopy) {
            message += L"\n\nThe source PDF also disallows content copying.";
        }
        message += L"\n\nIf export is necessary, confirm the rights holder's permission and use a PDF without protection.";
        return message;
    }

    std::wstring message =
        L"このPDFには著作者・配布元が設定した保護があります。\n"
        L"元の権利者の指示と利用条件に従うため、このアプリでは保護付きPDFからのPDF出力"
        L"（注釈を含むコピーを含む）を行いません。";
    if (!canCopy) {
        message += L"\n\n元のPDFでは内容のコピーも禁止されています。";
    }
    message += L"\n\n出力が必要な場合は、権利者の許諾と元PDFの利用条件を確認し、保護のないPDFで作業してください。";
    return message;
}

static std::optional<std::wstring> GetProtectedPdfPngExportBlockMessage(FPDF_DOCUMENT doc) {
    if (!doc) return std::nullopt;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    const int secRev = FPDF_GetSecurityHandlerRevision(doc);
    if (secRev < 0) return std::nullopt;

    const unsigned long userPerms = FPDF_GetDocUserPermissions(doc);
    const bool canCopy = userPerms == 0xfffffffful || (userPerms & kPdfPermissionCopyContent) != 0;

    if (IsEnglishUi()) {
        std::wstring message =
            L"This PDF has author/publisher protection settings.\n"
            L"To follow the original rights holder's instructions and usage conditions, "
            L"this app does not export page images from protected source PDFs.";
        if (!canCopy) {
            message += L"\n\nThe source PDF also disallows content copying.";
        }
        message += L"\n\nIf export is necessary, confirm the rights holder's permission and use a PDF without protection.";
        return message;
    }

    std::wstring message =
        L"このPDFには著作者・配布元が設定した保護があります。\n"
        L"元の権利者の指示と利用条件に従うため、このアプリでは保護付きPDFからのページ画像出力を行いません。";
    if (!canCopy) {
        message += L"\n\n元のPDFでは内容のコピーも禁止されています。";
    }
    message += L"\n\n出力が必要な場合は、権利者の許諾と元PDFの利用条件を確認し、保護のないPDFで作業してください。";
    return message;
}

static bool ConfirmProtectedPdfExportAllowed(HWND owner,
                                             const std::wstring& title,
                                             const std::wstring& blockedMessage,
                                             std::optional<std::wstring> (*recheck)(FPDF_DOCUMENT)) {
    if (!PromptPasswordAndReopenCurrentPdf(owner, title, blockedMessage)) {
        return false;
    }
    FPDF_DOCUMENT currentDoc = CurrentLogicalPdfDocument();
    if (!recheck || !recheck(currentDoc).has_value()) {
        return true;
    }
    ShowFileOutputMessageDialog(owner, title, blockedMessage, SoftNoticeKind::Warning);
    return false;
}

static constexpr int kMarkerTextBrightMin = 250;
static constexpr int kMarkerTextBrightMinChannel = 40;
static constexpr int kMarkerTextSaturationMin = 40;
static constexpr int kMarkerTextBlackMax = 70;
static constexpr int kMarkerTextBlackSatMax = 12;
static std::optional<std::wstring> g_lastPickedSaveDir;

static bool IsExistingDirectoryPath(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::path p(dir);
    return std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec);
}

static void RememberPickedSavePathDirectory(const std::wstring& filePath) {
    if (filePath.empty()) return;
    std::filesystem::path p(filePath);
    std::wstring parent = p.parent_path().wstring();
    if (IsExistingDirectoryPath(parent)) {
        g_lastPickedSaveDir = parent;
    }
}

static double CapAnnotBitmapExportDpi(double widthPt, double heightPt, double desiredDpi) {
    constexpr double kMinDpi = 72.0;
    constexpr double kMaxPixels = 8'000'000.0;
    constexpr double kMaxDimPx = 8192.0;

    widthPt = std::max(0.01, widthPt);
    heightPt = std::max(0.01, heightPt);
    desiredDpi = std::max(kMinDpi, desiredDpi);

    double dpiFromPixels = 72.0 * std::sqrt(kMaxPixels / (widthPt * heightPt));
    double dpiFromDimW = 72.0 * (kMaxDimPx / widthPt);
    double dpiFromDimH = 72.0 * (kMaxDimPx / heightPt);
    double dpi = std::min({ desiredDpi, dpiFromPixels, dpiFromDimW, dpiFromDimH });
    return std::clamp(dpi, kMinDpi, desiredDpi);
}

enum class AnnotationRenderMode {
    PdfLike,
    ViewerLike
};

static bool IsValidPdfExportScale(double scale) {
    return std::isfinite(scale) && scale >= 0.125 && scale <= 8.0;
}

static bool IsValidPdfCropRectPt(const file_output::PdfCropRectPt& crop, double pageWPt, double pageHPt) {
    constexpr double kMinCropSizePt = 1.0;
    if (!std::isfinite(crop.left) || !std::isfinite(crop.bottom) ||
        !std::isfinite(crop.right) || !std::isfinite(crop.top)) {
        return false;
    }
    if (!std::isfinite(pageWPt) || !std::isfinite(pageHPt) || pageWPt <= 0.0 || pageHPt <= 0.0) {
        return false;
    }
    if (crop.left < 0.0 || crop.bottom < 0.0 || crop.right > pageWPt || crop.top > pageHPt) {
        return false;
    }
    return (crop.right - crop.left) >= kMinCropSizePt &&
           (crop.top - crop.bottom) >= kMinCropSizePt;
}

static void ApplyPdfPageCropBoxes(FPDF_PAGE page, const file_output::PdfCropRectPt& crop) {
    if (!page) return;
    const float left = static_cast<float>(crop.left);
    const float bottom = static_cast<float>(crop.bottom);
    const float right = static_cast<float>(crop.right);
    const float top = static_cast<float>(crop.top);
    FPDFPage_SetMediaBox(page, left, bottom, right, top);
    FPDFPage_SetCropBox(page, left, bottom, right, top);
    FPDFPage_SetBleedBox(page, left, bottom, right, top);
    FPDFPage_SetTrimBox(page, left, bottom, right, top);
    FPDFPage_SetArtBox(page, left, bottom, right, top);
}

static void ScalePdfPageBoxes(FPDF_PAGE page, double scale) {
    if (!page || scale == 1.0) return;
    float l = 0, b = 0, r = 0, t = 0;
    if (FPDFPage_GetMediaBox(page, &l, &b, &r, &t)) {
        FPDFPage_SetMediaBox(page,
                             static_cast<float>(l * scale),
                             static_cast<float>(b * scale),
                             static_cast<float>(r * scale),
                             static_cast<float>(t * scale));
    }
    if (FPDFPage_GetCropBox(page, &l, &b, &r, &t)) {
        FPDFPage_SetCropBox(page,
                            static_cast<float>(l * scale),
                            static_cast<float>(b * scale),
                            static_cast<float>(r * scale),
                            static_cast<float>(t * scale));
    }
    if (FPDFPage_GetBleedBox(page, &l, &b, &r, &t)) {
        FPDFPage_SetBleedBox(page,
                             static_cast<float>(l * scale),
                             static_cast<float>(b * scale),
                             static_cast<float>(r * scale),
                             static_cast<float>(t * scale));
    }
    if (FPDFPage_GetTrimBox(page, &l, &b, &r, &t)) {
        FPDFPage_SetTrimBox(page,
                            static_cast<float>(l * scale),
                            static_cast<float>(b * scale),
                            static_cast<float>(r * scale),
                            static_cast<float>(t * scale));
    }
    if (FPDFPage_GetArtBox(page, &l, &b, &r, &t)) {
        FPDFPage_SetArtBox(page,
                           static_cast<float>(l * scale),
                           static_cast<float>(b * scale),
                           static_cast<float>(r * scale),
                           static_cast<float>(t * scale));
    }
}

static bool ScalePdfPageInPlace(FPDF_PAGE page, double scale) {
    if (!page || scale == 1.0) return true;

    FS_MATRIX m{};
    m.a = static_cast<float>(scale);
    m.d = static_cast<float>(scale);

    // Prefer transforming the page as a whole. This tends to be more robust than
    // transforming individual objects (which caused rendering regressions in some PDFs).
    if (!FPDFPage_TransFormWithClip(page, &m, nullptr)) {
        return false;
    }
    ScalePdfPageBoxes(page, scale);
    return true;
}

static std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return L"";
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

static std::wstring DefaultNameFromPath(const std::wstring& path,
                                        const std::wstring& suffix,
                                        const std::wstring& fallback) {
    if (path.empty()) return fallback;
    std::filesystem::path src(path);
    std::wstring stem = src.stem().wstring();
    if (stem.empty()) stem = fallback;
    return stem + suffix;
}

static std::wstring InitialDirForPath(const std::wstring& path) {
    if (g_lastPickedSaveDir.has_value() && IsExistingDirectoryPath(*g_lastPickedSaveDir)) {
        return *g_lastPickedSaveDir;
    }
    if (!path.empty()) {
        std::wstring parent = std::filesystem::path(path).parent_path().wstring();
        if (IsExistingDirectoryPath(parent)) return parent;
    }
    std::wstring exeDir = ExeDirectory();
    if (!exeDir.empty()) return exeDir;
    return L"";
}

static std::optional<std::wstring> ValidatePickedSavePath(HWND owner, std::filesystem::path path) {
    if (IsUncPath(path)) {
        ShowFileOutputSoftNotice(owner,
                                 IsEnglishUi() ? L"UNC/device output paths are not supported."
                                               : L"UNC/デバイスの出力先パスは使用できません。",
                                 SoftNoticeKind::Warning);
        return std::nullopt;
    }
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(path.parent_path(), isReparse) && isReparse) {
        ShowFileOutputSoftNotice(owner,
                                 IsEnglishUi() ? L"Reparse point output folders are not supported."
                                               : L"ジャンクション/シンボリックリンク等の出力先フォルダは使用できません。",
                                 SoftNoticeKind::Warning);
        return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        ShowFileOutputSoftNotice(
            owner,
            IsEnglishUi()
                ? L"An existing file will not be overwritten. Enter a new output file name."
                : L"既存ファイルは上書きしません。新しい出力ファイル名を入力してください。",
            SoftNoticeKind::Warning);
        return std::nullopt;
    }
    if (ec) {
        ShowFileOutputSoftNotice(owner,
                                 IsEnglishUi() ? L"Could not inspect the output path."
                                               : L"出力先パスを確認できません。",
                                 SoftNoticeKind::Warning);
        return std::nullopt;
    }
    RememberPickedSavePathDirectory(path.wstring());
    return path.wstring();
}

static std::optional<std::wstring> PickSavePathWithSystemDialog(HWND owner,
                                                                 const wchar_t* title,
                                                                 const std::wstring& defaultName,
                                                                 const std::wstring& initialDir,
                                                                 const COMDLG_FILTERSPEC* filters,
                                                                 UINT filterCount,
                                                                 const wchar_t* defaultExt) {
    IFileSaveDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
        ShowFileOutputSoftNotice(owner,
                                 IsEnglishUi() ? L"Could not open the system save dialog."
                                               : L"OS標準の保存ダイアログを開けませんでした。",
                                 SoftNoticeKind::Warning);
        return std::nullopt;
    }
    dialog->SetTitle(title ? title : L"");
    if (filters && filterCount > 0) dialog->SetFileTypes(filterCount, filters);
    if (defaultExt && *defaultExt) dialog->SetDefaultExtension(defaultExt);
    dialog->SetFileName(defaultName.c_str());
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN;
        options &= ~FOS_OVERWRITEPROMPT;
        dialog->SetOptions(options);
    }
    if (!initialDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return std::nullopt;
    }
    if (FAILED(shown)) {
        dialog->Release();
        ShowFileOutputSoftNotice(owner,
                                 IsEnglishUi() ? L"The system save dialog failed."
                                               : L"OS標準の保存ダイアログでエラーが発生しました。",
                                 SoftNoticeKind::Warning);
        return std::nullopt;
    }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) {
        dialog->Release();
        return std::nullopt;
    }
    PWSTR rawPath = nullptr;
    const HRESULT gotPath = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    item->Release();
    dialog->Release();
    if (FAILED(gotPath) || !rawPath) return std::nullopt;
    std::filesystem::path path(rawPath);
    CoTaskMemFree(rawPath);
    return ValidatePickedSavePath(owner, std::move(path));
}

static std::optional<std::wstring> PickSavePath(HWND owner,
                                                const wchar_t* title,
                                                const std::wstring& defaultName,
                                                const std::wstring& initialDir,
                                                const COMDLG_FILTERSPEC* filters,
                                                UINT filterCount,
                                                const wchar_t* defaultExt) {
    std::wstring fileName;
    const SavePathPromptResult choice = PromptSavePath(owner, title ? title : L"", initialDir,
                                                        defaultName, fileName);
    if (choice == SavePathPromptResult::Cancel) return std::nullopt;
    if (choice == SavePathPromptResult::OpenSystemDialog) {
        return PickSavePathWithSystemDialog(owner, title, defaultName, initialDir,
                                            filters, filterCount, defaultExt);
    }

    std::filesystem::path path(fileName);
    if (path.has_parent_path() || path.is_absolute()) {
        ShowFileOutputSoftNotice(owner,
                                 IsEnglishUi() ? L"Enter only a file name, or use the system dialog to change folders."
                                               : L"ファイル名のみを入力してください。フォルダを変える場合はOS標準で開くを使います。",
                                 SoftNoticeKind::Warning);
        return std::nullopt;
    }
    if (defaultExt && *defaultExt && !path.has_extension()) {
        path.replace_extension(std::wstring(L".") + defaultExt);
    }
    if (!initialDir.empty()) path = std::filesystem::path(initialDir) / path;
    return ValidatePickedSavePath(owner, std::move(path));
}

static bool EnsureParentDir(const std::filesystem::path& path) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (parent.empty()) return true;
    std::filesystem::create_directories(parent, ec);
    return !ec;
}

static std::wstring GetEditText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    std::wstring text(len, L'\0');
    if (len > 0) {
        GetWindowTextW(hEdit, text.data(), len + 1);
    }
    return text;
}

static bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
    std::filesystem::path p(path);
    if (!EnsureParentDir(p)) return false;
    std::wstring err;
    // Export targets may be outside the workspace; keep temp files next to the destination.
    if (!atomic_write::AtomicWriteUtf8(p, data, /*preferredTempDir=*/p.parent_path(), &err)) {
        return false;
    }
    return true;
}
static bool SavePngWic(const std::wstring& path, const uint8_t* bgra,
                       int w, int h, int stride, double dpi, std::wstring* err) {
    if (!bgra || w <= 0 || h <= 0 || stride <= 0) return false;
    const std::filesystem::path dest(path);
    std::filesystem::path tmp;
    HANDLE tmpHandle = INVALID_HANDLE_VALUE;
    std::wstring writeErr;
    if (!atomic_write::CreateUniqueTempFile(dest, dest.parent_path(), &tmp, &tmpHandle, &writeErr)) {
        if (err) *err = writeErr;
        return false;
    }
    CloseHandle(tmpHandle);
    const auto removeTemp = [&]() {
        std::error_code removeError;
        std::filesystem::remove(tmp, removeError);
    };

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        removeTemp();
        if (err) *err = L"WIC factory creation failed.";
        return false;
    }
    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        factory->Release();
        removeTemp();
        if (err) *err = L"WIC stream creation failed.";
        return false;
    }
    hr = stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        stream->Release();
        factory->Release();
        removeTemp();
        if (err) *err = L"Failed to open output file.";
        return false;
    }
    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        stream->Release();
        factory->Release();
        removeTemp();
        if (err) *err = L"WIC encoder creation failed.";
        return false;
    }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        encoder->Release();
        stream->Release();
        factory->Release();
        removeTemp();
        if (err) *err = L"WIC encoder initialization failed.";
        return false;
    }
    IWICBitmapFrameEncode* frame = nullptr;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr) || !frame) {
        encoder->Release();
        stream->Release();
        factory->Release();
        removeTemp();
        if (err) *err = L"WIC frame creation failed.";
        return false;
    }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        removeTemp();
        if (err) *err = L"WIC frame init failed.";
        return false;
    }
    frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
    if (dpi > 0.0) frame->SetResolution(dpi, dpi);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    // WIC's WritePixels API is non-const but does not modify the buffer.
    auto* pixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bgra));
    hr = frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(stride),
                            static_cast<UINT>(stride * h), pixels);
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        removeTemp();
        if (err) *err = L"WIC write failed.";
        return false;
    }
    hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    frame->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
    if (FAILED(hr)) {
        removeTemp();
        if (err) *err = L"WIC PNG encoding commit failed.";
        return false;
    }
    std::wstring replaceErr;
    if (!atomic_write::AtomicReplaceFile(dest, tmp, dest.parent_path(), &replaceErr)) {
        if (err) *err = replaceErr.empty() ? L"Failed to replace PNG output file." : replaceErr;
        return false;
    }
    return true;
}

static bool IsSamePath(const std::filesystem::path& a, const std::filesystem::path& b);

static note_snapshot::CurrentEditTextSnapshot CurrentEditSnapshotForNotePath(const std::wstring& notePath) {
    note_snapshot::CurrentEditTextSnapshot snapshot;
    if (!g_currentNotePath.empty() && g_hNoteEdit &&
        IsSamePath(std::filesystem::path(notePath), std::filesystem::path(g_currentNotePath))) {
        std::wstring text = GetEditText(g_hNoteEdit);
        snapshot.available = true;
        snapshot.targetPath = notePath;
        snapshot.bytes = WideToUTF8(text);
        snapshot.identity = CaptureCurrentNoteSnapshotIdentity();
    }
    return snapshot;
}

static bool IsPlainTextNotePath(const std::wstring& notePath) {
    if (notePath.empty()) return false;
    std::wstring ext = std::filesystem::path(notePath).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return ext == L".txt" || ext == L".csv";
}

static bool IsSamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ecA;
    std::error_code ecB;
    const auto normA = std::filesystem::weakly_canonical(a, ecA);
    const auto normB = std::filesystem::weakly_canonical(b, ecB);
    if (!ecA && !ecB) {
        return normA == normB;
    }
    auto lower = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(towlower(ch));
        });
        return value;
    };
    return lower(a.lexically_normal().wstring()) == lower(b.lexically_normal().wstring());
}

struct ResolvedNoteExportSource {
    note_snapshot::LatestTextSnapshot snapshot;
    std::wstring raw;
    std::shared_ptr<const note::WorkspaceNoteIndexSnapshot> index;
    bool plain_text = false;
};

static std::optional<ResolvedNoteExportSource> ResolveNoteExportSource(
    const std::wstring& notePath) {
    if (notePath.empty()) return std::nullopt;
    ResolvedNoteExportSource source;
    source.snapshot = note_snapshot::LoadLatestTextSnapshot(
        notePath, CurrentEditSnapshotForNotePath(notePath));
    if (!source.snapshot.ok || !source.snapshot.identity.valid()) {
        return std::nullopt;
    }
    source.raw = UTF8ToWide(source.snapshot.bytes);
    source.plain_text = IsPlainTextNotePath(source.snapshot.targetPath);
    note::NoteMetadata meta;
    meta.file_name =
        std::filesystem::path(source.snapshot.targetPath).filename().wstring();
    meta.title = note::DeriveTitleFromFileName(meta.file_name);
    const note::NoteContentKind contentKind = source.plain_text
        ? note::NoteContentKind::PlainText
        : note::NoteContentKind::Markdown;

    note::NoteWorkspaceService& workspace = note::RuntimeNoteWorkspaceService();
    note::LocalNoteKernel* kernel = workspace.FindKernel(
        source.snapshot.identity.note_id);
    if (kernel && kernel->valid() &&
        kernel->content_kind() == contentKind &&
        kernel->text_core().MatchesRaw(source.raw) &&
        (source.snapshot.identity.content_revision == 0 ||
         source.snapshot.identity.content_revision ==
             kernel->text_core().content_revision())) {
        source.index = workspace.ResolveIndexFromKernel(
            source.snapshot.identity, source.snapshot.targetPath, *kernel);
    }
    if (!source.index) {
        source.index = workspace.ResolveIndex(
            source.snapshot.identity,
            source.snapshot.targetPath,
            std::move(meta),
            source.raw,
            contentKind);
    }
    if (!source.index) return std::nullopt;
    return source;
}

static std::string EscapeHtml(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

static std::string BuildPlainTextHtmlDocument(const std::wstring& rawText,
                                              const std::wstring& title,
                                              bool includeTitleHeading) {
    const std::string titleUtf8 = EscapeHtml(WideToUTF8(title));
    const std::string bodyUtf8 = EscapeHtml(WideToUTF8(rawText));
    std::string out;
    out.reserve(bodyUtf8.size() + 512);
    out += "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>" + titleUtf8 + "</title>\n";
    out += "<style>\n";
    out += "body{font-family:Segoe UI,Meiryo,sans-serif;margin:24px;line-height:1.6;}";
    out += "pre{white-space:pre-wrap;word-break:break-word;margin:0;}";
    out += "h1{font-size:1.5rem;margin:0 0 1rem;}";
    out += "\n</style>\n</head>\n<body>\n";
    if (includeTitleHeading && !titleUtf8.empty()) {
        out += "<h1>" + titleUtf8 + "</h1>\n";
    }
    out += "<pre>";
    out += bodyUtf8;
    out += "</pre>\n</body>\n</html>\n";
    return out;
}

static std::wstring NoteTitleFromPath(const std::wstring& notePath) {
    if (notePath.empty()) return L"note";
    std::filesystem::path p(notePath);
    std::wstring stem = p.stem().wstring();
    if (!stem.empty()) return stem;
    std::wstring name = p.filename().wstring();
    return name.empty() ? L"note" : name;
}

static std::vector<std::wstring> SplitTextByNewlines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    cur.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            lines.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        if (ch == L'\n') {
            lines.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        cur.push_back(ch);
    }
    lines.push_back(std::move(cur));
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

static BYTE AlphaFromNorm(double a) {
    a = std::clamp(a, 0.0, 1.0);
    return static_cast<BYTE>(std::round(a * 255.0));
}

static double PtFromScreenPx(double px) {
    return px * 72.0 / kDpi;
}

static double ComputeArrowHeadLength(double lineLen, double maxHeadLen) {
    constexpr double kArrowHeadLengthRatio = 0.35;
    return std::min(lineLen * kArrowHeadLengthRatio, maxHeadLen);
}


static bool AppendDashedPdfSegment(FPDF_PAGEOBJECT path,
                                   double x1,
                                   double y1,
                                   double x2,
                                   double y2,
                                   const std::vector<double>& dashPt) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double len = std::hypot(dx, dy);
    if (len < 1e-6) return false;
    if (dashPt.empty()) {
        FPDFPath_MoveTo(path, static_cast<float>(x1), static_cast<float>(y1));
        FPDFPath_LineTo(path, static_cast<float>(x2), static_cast<float>(y2));
        return true;
    }
    std::vector<double> pattern;
    pattern.reserve(dashPt.size() == 1 ? 2 : dashPt.size());
    for (double part : dashPt) {
        if (std::isfinite(part) && part > 0.0) {
            pattern.push_back(std::max(0.25, part));
        }
    }
    if (pattern.empty()) {
        FPDFPath_MoveTo(path, static_cast<float>(x1), static_cast<float>(y1));
        FPDFPath_LineTo(path, static_cast<float>(x2), static_cast<float>(y2));
        return true;
    }
    if (pattern.size() == 1) pattern.push_back(pattern.front());
    double ux = dx / len;
    double uy = dy / len;
    double pos = 0.0;
    size_t pat = 0;
    bool draw = true;
    int guard = 0;
    bool hasSegment = false;
    while (pos < len - 1e-6 && guard++ < 4096) {
        double next = std::min(len, pos + pattern[pat % pattern.size()]);
        if (draw && next > pos + 1e-6) {
            double sx = x1 + ux * pos;
            double sy = y1 + uy * pos;
            double ex = x1 + ux * next;
            double ey = y1 + uy * next;
            FPDFPath_MoveTo(path, static_cast<float>(sx), static_cast<float>(sy));
            FPDFPath_LineTo(path, static_cast<float>(ex), static_cast<float>(ey));
            hasSegment = true;
        }
        pos = next;
        ++pat;
        draw = !draw;
    }
    return hasSegment;
}
static void SetStrokeAndWidth(FPDF_PAGEOBJECT obj, COLORREF color, BYTE alpha, double widthPt, bool roundCap) {
    FPDFPageObj_SetStrokeColor(obj, GetRValue(color), GetGValue(color), GetBValue(color), alpha);
    FPDFPageObj_SetStrokeWidth(obj, static_cast<float>(widthPt));
    FPDFPageObj_SetLineJoin(obj, FPDF_LINEJOIN_ROUND);
    if (roundCap) FPDFPageObj_SetLineCap(obj, FPDF_LINECAP_ROUND);
}

struct PdfTextFontCache {
    std::map<std::wstring, FPDF_FONT> fonts;
};

static void ClosePdfTextFonts(PdfTextFontCache& cache) {
    for (auto& it : cache.fonts) {
        if (it.second) FPDFFont_Close(it.second);
    }
    cache.fonts.clear();
}

static std::wstring ToLower(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        out.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return out;
}

static void AppendFontCandidate(std::vector<std::wstring>& out, std::vector<std::wstring>& outLower, const std::wstring& face) {
    if (face.empty()) return;
    std::wstring key = ToLower(face);
    for (const auto& existing : outLower) {
        if (existing == key) return;
    }
    out.push_back(face);
    outLower.push_back(std::move(key));
}

static std::vector<std::wstring> BuildPdfTextFontFallbacksForAnnotation(const std::wstring& preferredFace) {
    std::vector<std::wstring> out;
    std::vector<std::wstring> outLower;
    AppendFontCandidate(out, outLower, preferredFace);
    AppendFontCandidate(out, outLower, g_textFontName);
    AppendFontCandidate(out, outLower, GetDefaultFontFaceName());
    for (const auto& entry : kDefaultFontList) {
        AppendFontCandidate(out, outLower, entry.faceName);
    }
    return out;
}

static bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = ToLower(haystack);
    std::wstring n = ToLower(needle);
    return h.find(n) != std::wstring::npos;
}

static std::wstring WindowsFontsDir() {
    wchar_t winDir[MAX_PATH]{};
    UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    std::wstring out(winDir);
    out += L"\\Fonts";
    return out;
}

static std::wstring ExpandEnvIfNeeded(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    DWORD needed = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (needed == 0 || needed > 32768) return s;
    std::wstring out;
    out.resize(needed);
    DWORD written = ExpandEnvironmentStringsW(s.c_str(), out.data(), needed);
    if (written == 0 || written > needed) return s;
    // Drop trailing NUL added by API.
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

static bool TryGetFontFilePathFromRegistryKey(HKEY root, const std::wstring& faceName, std::wstring& outPath) {
    outPath.clear();
    if (faceName.empty()) return false;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    int bestScore = -1000000;
    std::wstring bestValue;

    DWORD index = 0;
    for (;;) {
        wchar_t valueName[512]{};
        DWORD valueNameLen = static_cast<DWORD>(std::size(valueName));
        wchar_t valueData[1024]{};
        DWORD valueDataSize = static_cast<DWORD>(sizeof(valueData));
        DWORD valueType = 0;
        LONG res = RegEnumValueW(hKey,
                                 index,
                                 valueName,
                                 &valueNameLen,
                                 nullptr,
                                 &valueType,
                                 reinterpret_cast<BYTE*>(valueData),
                                 &valueDataSize);
        if (res == ERROR_NO_MORE_ITEMS) break;
        ++index;
        if (res != ERROR_SUCCESS) continue;
        if (valueType != REG_SZ && valueType != REG_EXPAND_SZ) continue;
        if (valueNameLen == 0 || valueDataSize < sizeof(wchar_t)) continue;

        std::wstring name(valueName, valueNameLen);
        if (!ContainsNoCase(name, faceName)) continue;

        std::wstring data(valueData, valueDataSize / sizeof(wchar_t));
        if (!data.empty() && data.back() == L'\0') data.pop_back();
        if (data.empty()) continue;
        if (valueType == REG_EXPAND_SZ) data = ExpandEnvIfNeeded(data);

        std::wstring nameLower = ToLower(name);
        std::wstring faceLower = ToLower(faceName);

        int score = 1;
        if (nameLower.rfind(faceLower, 0) == 0) score += 1000; // startswith
        if (nameLower.find(L"regular") != std::wstring::npos) score += 200;
        if (nameLower.find(L"bold") != std::wstring::npos) score -= 150;
        if (nameLower.find(L"italic") != std::wstring::npos) score -= 150;
        if (nameLower.find(L"light") != std::wstring::npos) score -= 80;

        if (score > bestScore) {
            bestScore = score;
            bestValue = std::move(data);
        }
    }

    RegCloseKey(hKey);

    if (bestValue.empty()) return false;

    // If the registry already returns an absolute path, use it as-is.
    if (bestValue.find(L':') != std::wstring::npos || (bestValue.size() >= 2 && bestValue[0] == L'\\' && bestValue[1] == L'\\')) {
        outPath = bestValue;
        return true;
    }

    std::wstring fontsDir = WindowsFontsDir();
    if (fontsDir.empty()) return false;
    outPath = fontsDir + L"\\" + bestValue;
    return true;
}

static bool TryGetFontFilePathFromRegistry(const std::wstring& faceName, std::wstring& outPath) {
    if (TryGetFontFilePathFromRegistryKey(HKEY_CURRENT_USER, faceName, outPath)) return true;
    if (TryGetFontFilePathFromRegistryKey(HKEY_LOCAL_MACHINE, faceName, outPath)) return true;
    return false;
}

static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    if (path.empty()) return false;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }
    if (size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<uint32_t>::max())) {
        CloseHandle(hFile);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < out.size()) {
        DWORD chunk = 0;
        DWORD toRead = static_cast<DWORD>(out.size() - total);
        if (!ReadFile(hFile, out.data() + total, toRead, &chunk, nullptr) || chunk == 0) {
            CloseHandle(hFile);
            out.clear();
            return false;
        }
        total += chunk;
    }
    CloseHandle(hFile);
    if (total != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

static bool LoadFontBytesFromGdi(const std::wstring& faceName, std::vector<uint8_t>& out) {
    out.clear();

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(12, static_cast<int>(kDpi), 72);
    lf.lfCharSet = DEFAULT_CHARSET;
    if (!faceName.empty()) {
        wcsncpy_s(lf.lfFaceName, faceName.c_str(), LF_FACESIZE - 1);
    }

    HFONT hFont = CreateFontIndirectW(&lf);
    if (!hFont) {
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old = SelectObject(hdc, hFont);
    DWORD size = GetFontData(hdc, 0, 0, nullptr, 0);
    if (size == GDI_ERROR || size == 0) {
        if (old) SelectObject(hdc, old);
        DeleteObject(hFont);
        DeleteDC(hdc);
        return false;
    }

    out.resize(size);
    DWORD got = GetFontData(hdc, 0, 0, out.data(), size);
    if (got == GDI_ERROR || got != size) {
        out.clear();
        if (old) SelectObject(hdc, old);
        DeleteObject(hFont);
        DeleteDC(hdc);
        return false;
    }

    if (old) SelectObject(hdc, old);
    DeleteObject(hFont);
    DeleteDC(hdc);
    return true;
}

static bool IsFontCollectionPath(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring lower = ToLower(path);
    return lower.size() >= 4 &&
           (lower.rfind(L".ttc") == lower.size() - 4 ||
            lower.rfind(L".otc") == lower.size() - 4);
}

static bool LoadFontBytesFromSystem(const std::wstring& faceName, std::vector<uint8_t>& out, bool* outFromFile) {
    if (outFromFile) *outFromFile = false;
    // Prefer loading from the original font file for single-face fonts.
    // For TTC/OTC collections, the raw file does not identify which face we selected
    // (e.g. Meiryo UI vs Meiryo), and PDFium may bind a different face and garble text.
    // In that case, use GDI so the bytes match the chosen face.
    std::wstring fontPath;
    if (TryGetFontFilePathFromRegistry(faceName, fontPath) &&
        !IsFontCollectionPath(fontPath) &&
        ReadFileBytes(fontPath, out)) {
        if (outFromFile) *outFromFile = true;
        return true;
    }
    return LoadFontBytesFromGdi(faceName, out);
}

static int DetectFontTypeFromBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 4) {
        const uint8_t a = bytes[0], b = bytes[1], c = bytes[2], d = bytes[3];
        if (a == 'O' && b == 'T' && c == 'T' && d == 'O') {
            return FPDF_FONT_TYPE1; // OpenType CFF
        }
        if (a == 't' && b == 't' && c == 'c' && d == 'f') {
            return FPDF_FONT_TRUETYPE; // TrueType collection
        }
        if (a == 0x00 && b == 0x01 && c == 0x00 && d == 0x00) {
            return FPDF_FONT_TRUETYPE; // TrueType
        }
        if (a == 't' && b == 'r' && c == 'u' && d == 'e') {
            return FPDF_FONT_TRUETYPE;
        }
    }
    return FPDF_FONT_TRUETYPE;
}

static FPDF_FONT LoadPdfFontFromBytes(FPDF_DOCUMENT dest, const std::vector<uint8_t>& bytes) {
    if (!dest || bytes.empty() || bytes.size() > std::numeric_limits<uint32_t>::max()) return nullptr;
    int type = DetectFontTypeFromBytes(bytes);
    FPDF_FONT font = FPDFText_LoadFont(dest, bytes.data(),
                                       static_cast<uint32_t>(bytes.size()),
                                       type,
                                       /*cid=*/1);
    if (font) return font;
    // Some OpenType fonts may be detected as CFF but load as TrueType, or vice versa.
    int altType = (type == FPDF_FONT_TRUETYPE) ? FPDF_FONT_TYPE1 : FPDF_FONT_TRUETYPE;
    return FPDFText_LoadFont(dest, bytes.data(),
                             static_cast<uint32_t>(bytes.size()),
                             altType,
                             /*cid=*/1);
}

static std::string GetPdfFontBaseNameUtf8(FPDF_FONT font) {
    if (!font) return {};
    const size_t needed = FPDFFont_GetBaseFontName(font, nullptr, 0);
    if (needed <= 1) return {};
    std::string out;
    out.resize(needed);
    const size_t written = FPDFFont_GetBaseFontName(font, out.data(), out.size());
    if (written <= 1) return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

static bool IsSuspiciousPdfTextFont(FPDF_FONT font) {
    const std::string baseName = GetPdfFontBaseNameUtf8(font);
    if (baseName.empty()) return true;
    return _stricmp(baseName.c_str(), "Untitled") == 0;
}

static FPDF_FONT GetOrLoadPdfTextFont(PdfTextFontCache& cache, FPDF_DOCUMENT dest, const std::wstring& faceName) {
    if (!dest) return nullptr;

    const std::wstring key = faceName.empty() ? L"" : faceName;
    auto it = cache.fonts.find(key);
    if (it != cache.fonts.end()) return it->second;

    std::vector<uint8_t> fontBytes;
    const std::wstring useFace = key.empty() ? g_textFontName : key;
    bool fromFile = false;
    if (!LoadFontBytesFromSystem(useFace, fontBytes, &fromFile) || fontBytes.empty()) {
        cache.fonts.emplace(key, nullptr);
        return nullptr;
    }

    // Use CID font to support Unicode text (e.g., Japanese).
    FPDF_FONT font = LoadPdfFontFromBytes(dest, fontBytes);
    if (!font && fromFile) {
        // Some TTC fonts load the wrong face from raw file bytes. Retry via GDI.
        std::vector<uint8_t> gdiBytes;
        if (LoadFontBytesFromGdi(useFace, gdiBytes) && !gdiBytes.empty()) {
            font = LoadPdfFontFromBytes(dest, gdiBytes);
        }
    } else if (font && !fromFile && IsSuspiciousPdfTextFont(font)) {
        // GDI-selected font bytes can lose naming/cmap data for some faces.
        // Retry with the registry font file, even for TTC/OTC, if available.
        std::wstring fontPath;
        std::vector<uint8_t> fileBytes;
        if (TryGetFontFilePathFromRegistry(useFace, fontPath) &&
            ReadFileBytes(fontPath, fileBytes) &&
            !fileBytes.empty()) {
            FPDF_FONT fileFont = LoadPdfFontFromBytes(dest, fileBytes);
            if (fileFont && !IsSuspiciousPdfTextFont(fileFont)) {
                FPDFFont_Close(font);
                font = fileFont;
            } else if (fileFont) {
                FPDFFont_Close(fileFont);
            }
        }
    }

    cache.fonts.emplace(key, font);
    return font;
}

static std::u16string ToUtf16Le(const std::wstring& s) {
    std::u16string out;
    out.reserve(s.size());
    if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
        for (wchar_t ch : s) {
            out.push_back(static_cast<char16_t>(ch));
        }
        return out;
    }
    for (wchar_t ch : s) {
        uint32_t cp = static_cast<uint32_t>(ch);
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char16_t>(cp));
            continue;
        }
        cp -= 0x10000u;
        out.push_back(static_cast<char16_t>(0xD800u + ((cp >> 10) & 0x3FFu)));
        out.push_back(static_cast<char16_t>(0xDC00u + (cp & 0x3FFu)));
    }
    return out;
}

struct PdfTextFontPick {
    FPDF_FONT font = nullptr;
    std::wstring faceName;
    bool fallbackUsed = false;
};

static PdfTextFontPick ResolvePdfTextFont(PdfTextFontCache& cache, FPDF_DOCUMENT dest,
                                          const std::wstring& preferredFace) {
    PdfTextFontPick out;
    auto candidates = BuildPdfTextFontFallbacksForAnnotation(preferredFace);
    if (candidates.empty()) return out;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& face = candidates[i];
        FPDF_FONT font = GetOrLoadPdfTextFont(cache, dest, face);
        if (!font) continue;
        out.font = font;
        out.faceName = face;
        out.fallbackUsed = (i != 0);
        return out;
    }
    return out;
}

static bool MeasureGdiTextLineWidthsPt(const std::wstring& faceName, double fontPt,
                                      const std::vector<std::wstring>& lines,
                                      TEXTMETRICW& outTm,
                                      std::vector<double>& outWidthsPt) {
    outWidthsPt.assign(lines.size(), 0.0);
    outTm = TEXTMETRICW{};
    if (fontPt <= 0.0) return false;

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(static_cast<int>(std::lround(fontPt)), static_cast<int>(kDpi), 72);
    lf.lfCharSet = DEFAULT_CHARSET;
    if (!faceName.empty()) {
        wcsncpy_s(lf.lfFaceName, faceName.c_str(), LF_FACESIZE - 1);
    }

    HFONT hFont = CreateFontIndirectW(&lf);
    if (!hFont) {
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old = SelectObject(hdc, hFont);
    GetTextMetricsW(hdc, &outTm);

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& s = lines[i];
        if (s.empty()) continue;
        SIZE sz{};
        if (GetTextExtentPoint32W(hdc, s.c_str(), static_cast<int>(s.size()), &sz) != 0) {
            outWidthsPt[i] = static_cast<double>(sz.cx) * 72.0 / kDpi;
        }
    }

    if (old) SelectObject(hdc, old);
    DeleteObject(hFont);
    DeleteDC(hdc);
    return true;
}

static bool RenderTextAnnotBitmap(const Annotation& ann, double widthPt, double heightPt,
                                  double desiredDpi, double padScale,
                                  std::vector<uint8_t>& out, int& wPx, int& hPx, int& stride) {
    const double dpi = CapAnnotBitmapExportDpi(widthPt, heightPt, desiredDpi);
    wPx = std::max(1, static_cast<int>(std::round(widthPt * dpi / 72.0)));
    hPx = std::max(1, static_cast<int>(std::round(heightPt * dpi / 72.0)));
    stride = wPx * 4;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = wPx;
    bmi.bmiHeader.biHeight = hPx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        DeleteDC(hdc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(hdc, hbmp);
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(wPx) * hPx, 0);

    LOGFONTW lf{};
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt * padScale;
    if (fontPt <= 0.0) fontPt = 14.0 * padScale;
    lf.lfHeight = -MulDiv(static_cast<int>(std::round(fontPt)),
                          static_cast<int>(std::lround(dpi)), 72);
    if (!ann.fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, ann.fontName.c_str(), LF_FACESIZE - 1);
    } else {
        wcsncpy_s(lf.lfFaceName, g_textFontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = nullptr;
    if (hFont) oldFont = SelectObject(hdc, hFont);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    int padPx = static_cast<int>(std::round(4.0 * padScale * dpi / kDpi));
    RECT tr{ padPx, padPx, std::max(padPx + 1, wPx - padPx), std::max(padPx + 1, hPx - padPx) };
    const std::vector<std::wstring>* lines = &ann.textLines;
    std::vector<std::wstring> fallbackLines;
    if (lines->empty()) {
        fallbackLines = SplitTextByNewlines(ann.text);
        lines = &fallbackLines;
    }

    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    int lineHeight = std::max(1, static_cast<int>(tm.tmHeight));

    int curY = tr.top;
    for (const auto& lineText : *lines) {
        if (curY + tm.tmHeight > tr.bottom + tm.tmDescent + 2) break; // allow some overhang as in viewer
        if (!lineText.empty()) {
            TextOutW(hdc, tr.left, curY, lineText.c_str(), static_cast<int>(lineText.size()));
        }
        curY += lineHeight;
    }

    if (oldFont) SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBmp);

    out.resize(static_cast<size_t>(stride) * hPx);
    memcpy(out.data(), bits, out.size());

    if (hFont) DeleteObject(hFont);
    DeleteObject(hbmp);
    DeleteDC(hdc);

    DWORD* px = reinterpret_cast<DWORD*>(out.data());
    COLORREF base = ann.color;
    BYTE cr = GetRValue(base), cg = GetGValue(base), cb = GetBValue(base);
    for (int y = 0; y < hPx; ++y) {
        DWORD* row = px + static_cast<size_t>(y) * wPx;
        for (int x = 0; x < wPx; ++x) {
            BYTE b = static_cast<BYTE>(row[x] & 0xFF);
            BYTE g = static_cast<BYTE>((row[x] >> 8) & 0xFF);
            BYTE r = static_cast<BYTE>((row[x] >> 16) & 0xFF);
            BYTE a = std::max<BYTE>(r, std::max<BYTE>(g, b));
            if (a == 0) { row[x] = 0; continue; }
            double af = a / 255.0;
            BYTE rOut = static_cast<BYTE>(std::round(cr * af));
            BYTE gOut = static_cast<BYTE>(std::round(cg * af));
            BYTE bOut = static_cast<BYTE>(std::round(cb * af));
            row[x] = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(rOut) << 16) |
                     (static_cast<DWORD>(gOut) << 8) | bOut;
        }
    }
    return true;
}

static void AddTextAnnotationBitmap(FPDF_DOCUMENT dest, FPDF_PAGE page, const Annotation& ann,
                                    double bitmapDpiScale, double coordScale) {
    double left = std::min(ann.x1, ann.x2);
    double right = std::max(ann.x1, ann.x2);
    double bottom = std::min(ann.y1, ann.y2);
    double top = std::max(ann.y1, ann.y2);
    double wPt = right - left;
    double hPt = top - bottom;
    if (wPt <= 0.1 || hPt <= 0.1) return;
    int wPx = 0, hPx = 0, stride = 0;
    std::vector<uint8_t> buf;
    constexpr double kBaseDpi = 216.0;
    if (!RenderTextAnnotBitmap(ann, wPt, hPt, kBaseDpi * bitmapDpiScale, coordScale,
                               buf, wPx, hPx, stride)) return;

    FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(dest);
    if (!img) return;
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_BGRA, buf.data(), stride);
    if (!bmp) return;
    if (!FPDFImageObj_SetBitmap(&page, 1, img, bmp)) {
        FPDFBitmap_Destroy(bmp);
        return;
    }
    FPDFBitmap_Destroy(bmp);
    FPDFImageObj_SetMatrix(img,
                           static_cast<float>(wPt), 0.0f,
                           0.0f, static_cast<float>(-hPt),
                           static_cast<float>(left), static_cast<float>(bottom + hPt));
    FPDFPage_InsertObject(page, img);
}

static bool ShouldExportTextAsPdfText(bool standardTextAnnots) {
    return standardTextAnnots;
}

static void AddTextAnnotation(PdfTextFontCache& fonts, FPDF_DOCUMENT dest, FPDF_PAGE page,
                              const Annotation& ann, bool standardTextAnnots,
                              bool matchPdfPaneTextLayout, double bitmapDpiScale, double coordScale) {
    double left = std::min(ann.x1, ann.x2);
    double right = std::max(ann.x1, ann.x2);
    double bottom = std::min(ann.y1, ann.y2);
    double top = std::max(ann.y1, ann.y2);
    double wPt = right - left;
    double hPt = top - bottom;
    if (wPt <= 0.1 || hPt <= 0.1) return;

    if (!ShouldExportTextAsPdfText(standardTextAnnots)) {
        AddTextAnnotationBitmap(dest, page, ann, bitmapDpiScale, coordScale);
        return;
    }

    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt * coordScale;
    if (fontPt <= 0.0) fontPt = 14.0 * coordScale;
    std::wstring preferredFace = ann.fontName.empty() ? g_textFontName : ann.fontName;
    PdfTextFontPick pick = ResolvePdfTextFont(fonts, dest, preferredFace);
    if (!pick.font) {
        // Fallback: keep existing behavior (bitmap) if font embedding fails.
        AddTextAnnotationBitmap(dest, page, ann, bitmapDpiScale, coordScale);
        return;
    }
    std::wstring faceName = pick.faceName;
    bool fallbackUsed = pick.fallbackUsed;

    // When selected, match the PDF-pane layout. textLines is created by the
    // same wrapping calculation used to render a TextBox in the PDF pane,
    // including automatic line breaks. Older annotations without cached layout
    // data, and the legacy option, use the safe explicit-newline fallback.
    std::vector<std::wstring> lines;
    if (matchPdfPaneTextLayout) {
        lines = ann.textLines;
    }
    if (lines.empty()) {
        lines = SplitTextByNewlines(ann.text);
    }

    TEXTMETRICW tm{};
    std::vector<double> widthsPt;
    (void)MeasureGdiTextLineWidthsPt(faceName, fontPt, lines, tm, widthsPt);

    double baseAscentPt = fontPt;
    double baseLineHeightPt = fontPt * 1.2;
    if (tm.tmHeight > 0) {
        baseLineHeightPt = static_cast<double>(tm.tmHeight) * 72.0 / kDpi;
    }
    if (tm.tmAscent > 0) {
        baseAscentPt = static_cast<double>(tm.tmAscent) * 72.0 / kDpi;
    }
    if (baseLineHeightPt <= 0.01) baseLineHeightPt = fontPt * 1.2;
    if (baseAscentPt <= 0.01) baseAscentPt = fontPt;

    const double padPt = kTextPadPx * 72.0 / kDpi * coordScale;
    const double innerLeft = left + padPt;
    const double innerTop = top - padPt;
    const double innerBottom = bottom + padPt;

    double innerWPt = std::max(0.01, wPt - padPt * 2.0);
    double innerHPt = std::max(0.01, hPt - padPt * 2.0);
    double maxLineWPt = 0.0;
    for (double w : widthsPt) maxLineWPt = std::max(maxLineWPt, w);
    double totalHPt = baseLineHeightPt * static_cast<double>(lines.size());
    double scaleW = (maxLineWPt > 0.01) ? (innerWPt / maxLineWPt) : 1.0;
    double scaleH = (totalHPt > 0.01) ? (innerHPt / totalHPt) : 1.0;
    double scale = std::min(1.0, std::min(scaleW, scaleH));
    scale = std::clamp(scale, 0.05, 1.0);

    constexpr double kExportStandardTextFallbackMinPt = 6.0;
    const double minTextPt = kExportStandardTextMinPt * coordScale;
    const double minFallbackPt = kExportStandardTextFallbackMinPt * coordScale;
    const double minFontPt = fallbackUsed
                             ? std::min(fontPt, minFallbackPt)
                             : std::min(fontPt, minTextPt);
    const double idealFontPt = fontPt * scale;
    double exportFontPt = std::max(minFontPt, idealFontPt);
    const bool allowDrift = exportFontPt > (idealFontPt + 1e-6);
    double ascentPt = baseAscentPt * (exportFontPt / fontPt);
    double lineHeightPt = baseLineHeightPt * (exportFontPt / fontPt);
    if (lineHeightPt <= 0.01) lineHeightPt = exportFontPt * 1.2;
    if (ascentPt <= 0.01) ascentPt = exportFontPt;

    double drawLeft = innerLeft;
    double drawTop = innerTop;
    double bottomLimit = innerBottom;
    if (allowDrift) {
        bottomLimit = padPt;

        double pageW = FPDF_GetPageWidthF(page);
        double pageH = FPDF_GetPageHeightF(page);
        if (pageW <= 0.0) pageW = right + padPt;
        if (pageH <= 0.0) pageH = top + padPt;

        double maxLineWScaledPt = maxLineWPt * (exportFontPt / fontPt);
        double blockWPt = std::max(0.0, maxLineWScaledPt);
        double blockHPt = std::max(0.0, lineHeightPt * static_cast<double>(lines.size()));

        // Clamp the block within the page so the text doesn't get pushed off-screen
        // when we refuse to shrink further.
        if (blockWPt > 0.0 && pageW > 0.0) {
            double minX = padPt;
            double maxX = std::max(minX, pageW - padPt - blockWPt);
            drawLeft = std::clamp(drawLeft, minX, maxX);
        }
        if (blockHPt > 0.0 && pageH > 0.0) {
            double maxTop = pageH - padPt;
            double minTop = padPt + blockHPt;
            if (minTop > maxTop) {
                drawTop = maxTop;
            } else {
                drawTop = std::clamp(drawTop, minTop, maxTop);
            }
        }
    }

    if (fallbackUsed) {
        double pageW = FPDF_GetPageWidthF(page);
        if (pageW <= 0.0) pageW = right + padPt;

        double maxLineWScaledPt = maxLineWPt * (exportFontPt / fontPt);
        if (maxLineWScaledPt > innerWPt + 1e-6) {
            // Shift left so the right edge stays inside the annotation box if possible.
            double desiredLeft = innerLeft - (maxLineWScaledPt - innerWPt);
            drawLeft = std::min(drawLeft, desiredLeft);

            if (pageW > 0.0) {
                double minX = padPt;
                double maxX = std::max(minX, pageW - padPt - maxLineWScaledPt);
                drawLeft = std::clamp(drawLeft, minX, maxX);
            }

            // If we still can't fit within the page, shrink further.
            double maxW = (pageW > 0.0) ? std::max(0.0, pageW - padPt * 2.0) : innerWPt;
            if (maxW > 0.0 && maxLineWScaledPt > maxW + 1e-6) {
                double shrink = maxW / maxLineWScaledPt;
                double targetPt = exportFontPt * shrink;
                double fallbackMin = std::min(fontPt, minFallbackPt);
                double newFontPt = std::max(fallbackMin, targetPt);
                if (newFontPt + 1e-6 < exportFontPt) {
                    exportFontPt = newFontPt;
                    ascentPt = baseAscentPt * (exportFontPt / fontPt);
                    lineHeightPt = baseLineHeightPt * (exportFontPt / fontPt);
                    maxLineWScaledPt = maxLineWPt * (newFontPt / fontPt);
                    drawLeft = innerLeft;
                    if (maxLineWScaledPt > innerWPt + 1e-6) {
                        double desiredLeft2 = innerLeft - (maxLineWScaledPt - innerWPt);
                        drawLeft = std::min(drawLeft, desiredLeft2);
                    }
                    if (pageW > 0.0) {
                        double minX = padPt;
                        double maxX = std::max(minX, pageW - padPt - maxLineWScaledPt);
                        drawLeft = std::clamp(drawLeft, minX, maxX);
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        double lineTop = drawTop - static_cast<double>(i) * lineHeightPt;
        if (lineTop <= bottomLimit) break;
        double baselineY = lineTop - ascentPt;

        const std::wstring& line = lines[i];
        if (line.empty()) continue;

        FPDF_PAGEOBJECT textObj = FPDFPageObj_CreateTextObj(dest, pick.font, static_cast<float>(exportFontPt));
        if (!textObj) continue;
        std::u16string u16 = ToUtf16Le(line);
        FPDFText_SetText(textObj, reinterpret_cast<FPDF_WIDESTRING>(u16.c_str()));
        FPDFTextObj_SetTextRenderMode(textObj, FPDF_TEXTRENDERMODE_FILL);
        FPDFPageObj_SetFillColor(textObj,
                                 static_cast<unsigned int>(GetRValue(ann.color)),
                                 static_cast<unsigned int>(GetGValue(ann.color)),
                                 static_cast<unsigned int>(GetBValue(ann.color)),
                                 255);
        FS_MATRIX m{};
        m.a = 1.0f;
        m.b = 0.0f;
        m.c = 0.0f;
        m.d = 1.0f;
        m.e = static_cast<float>(drawLeft);
        m.f = static_cast<float>(baselineY);
        FPDFPageObj_SetMatrix(textObj, &m);
        FPDFPage_InsertObject(page, textObj);
    }
}

static std::wstring StripMathDelims(const std::wstring& raw) {
    if (raw.size() >= 4 && raw[0] == L'\\' && raw[1] == L'[' &&
        raw[raw.size() - 2] == L'\\' && raw[raw.size() - 1] == L']') {
        return raw.substr(2, raw.size() - 4);
    }
    if (raw.size() >= 4 && raw[0] == L'\\' && raw[1] == L'(' &&
        raw[raw.size() - 2] == L'\\' && raw[raw.size() - 1] == L')') {
        return raw.substr(2, raw.size() - 4);
    }
    if (raw.size() >= 2 && raw.front() == L'$' && raw.back() == L'$') {
        if (raw.size() >= 4 && raw[1] == L'$' && raw[raw.size() - 2] == L'$') {
            return raw.substr(2, raw.size() - 4);
        }
        return raw.substr(1, raw.size() - 2);
    }
    if (raw.size() >= 9 &&
        (_wcsnicmp(raw.c_str(), L"<math>", 6) == 0) &&
        (_wcsicmp(raw.c_str() + raw.size() - 3, L"</>") == 0)) {
        return raw.substr(6, raw.size() - 9);
    }
    return raw;
}

static std::wstring StripMarkupTags(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool inTag = false;
    for (wchar_t ch : s) {
        if (ch == L'<') {
            inTag = true;
            continue;
        }
        if (ch == L'>' && inTag) {
            inTag = false;
            continue;
        }
        if (!inTag) out.push_back(ch);
    }
    return out;
}

static std::wstring StripLineJoins(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == L'\r') { ++i; continue; }
        if (s[i] == L'\n') { out.push_back(L' '); ++i; continue; }
        if (s[i] == L'<' && i + 3 < s.size()) {
            if (_wcsnicmp(s.c_str() + i, L"<br>", 4) == 0) {
                out.push_back(L' ');
                i += 4;
                continue;
            }
            if (_wcsnicmp(s.c_str() + i, L"<br/>", 5) == 0) {
                out.push_back(L' ');
                i += 5;
                continue;
            }
            if (_wcsnicmp(s.c_str() + i, L"<br />", 6) == 0) {
                out.push_back(L' ');
                i += 6;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

static std::wstring NormalizeMarkupMathText(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t ch = s[i];
        if (ch >= 0xFF01 && ch <= 0xFF5E) {
            ch = static_cast<wchar_t>(ch - 0xFEE0);
        }
        out.push_back(ch);
    }
    std::wstring res;
    res.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (i + 1 < out.size() && out[i] == L'-' && out[i + 1] == L'>') {
            res.push_back(L'\u2192');
            ++i;
            continue;
        }
        res.push_back(out[i]);
    }
    return res;
}

static std::wstring NormalizeMathAnnotText(const Annotation& ann) {
    std::wstring body = StripMathDelims(ann.text);
    if (ann.mathKind == MathKind::Markup) {
        body = StripMarkupTags(body);
        body = StripLineJoins(body);
        body = NormalizeMarkupMathText(body);
    }
    return body;
}

static bool RenderMathAnnotBitmap(const Annotation& ann, double widthPt, double heightPt,
                                  double desiredDpi, double padScale,
                                  std::vector<uint8_t>& out, int& wPx, int& hPx, int& stride) {
    const double dpi = CapAnnotBitmapExportDpi(widthPt, heightPt, desiredDpi);
    wPx = std::max(1, static_cast<int>(std::round(widthPt * dpi / 72.0)));
    hPx = std::max(1, static_cast<int>(std::round(heightPt * dpi / 72.0)));
    stride = wPx * 4;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = wPx;
    bmi.bmiHeader.biHeight = hPx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        DeleteDC(hdc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(hdc, hbmp);

    RECT rc{ 0, 0, wPx, hPx };
    HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, bk);
    DeleteObject(bk);

    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt * padScale;
    int fontPx = std::max(6, static_cast<int>(std::round(fontPt * dpi / 72.0)));
    LOGFONTW lf{};
    lf.lfHeight = -fontPx;
    if (!ann.fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, ann.fontName.c_str(), LF_FACESIZE - 1);
    } else {
        wcsncpy_s(lf.lfFaceName, g_textFontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = hFont ? SelectObject(hdc, hFont) : nullptr;

    std::wstring body = NormalizeMathAnnotText(ann);
    auto node = mathrender::Parse(body);
    const int pad = std::max(2, static_cast<int>(std::round(4.0 * padScale * dpi / kDpi)));
    int availW = std::max(1, wPx - pad * 2);
    int availH = std::max(1, hPx - pad * 2);
    if (node) {
        auto layout = mathrender::Measure(*node, hdc, fontPx, mathrender::RenderStyle::Display);
        int drawX = pad;
        int drawY = pad;
        if (layout.width > availW || layout.height > availH) {
            int shrink = std::max(2, fontPx / 4);
            int fontPx2 = std::max(6, fontPx - shrink);
            layout = mathrender::Measure(*node, hdc, fontPx2, mathrender::RenderStyle::Display);
            mathrender::Draw(*node, layout, hdc, drawX, drawY, fontPx2, RGB(255, 255, 255),
                             mathrender::RenderStyle::Display);
        } else {
            mathrender::Draw(*node, layout, hdc, drawX, drawY, fontPx, RGB(255, 255, 255),
                             mathrender::RenderStyle::Display);
        }
    }

    if (oldFont) SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBmp);

    out.resize(static_cast<size_t>(stride) * hPx);
    memcpy(out.data(), bits, out.size());

    if (hFont) DeleteObject(hFont);
    DeleteObject(hbmp);
    DeleteDC(hdc);

    DWORD* px = reinterpret_cast<DWORD*>(out.data());
    COLORREF base = ann.color;
    BYTE cr = GetRValue(base), cg = GetGValue(base), cb = GetBValue(base);
    for (int y = 0; y < hPx; ++y) {
        DWORD* row = px + static_cast<size_t>(y) * wPx;
        for (int x = 0; x < wPx; ++x) {
            BYTE b = static_cast<BYTE>(row[x] & 0xFF);
            BYTE g = static_cast<BYTE>((row[x] >> 8) & 0xFF);
            BYTE r = static_cast<BYTE>((row[x] >> 16) & 0xFF);
            BYTE a = std::max<BYTE>(r, std::max<BYTE>(g, b));
            if (a == 0) { row[x] = 0; continue; }
            double af = a / 255.0;
            BYTE rOut = static_cast<BYTE>(std::round(cr * af));
            BYTE gOut = static_cast<BYTE>(std::round(cg * af));
            BYTE bOut = static_cast<BYTE>(std::round(cb * af));
            row[x] = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(rOut) << 16) |
                     (static_cast<DWORD>(gOut) << 8) | bOut;
        }
    }
    return true;
}

static void AddMathAnnotation(FPDF_DOCUMENT dest, FPDF_PAGE page, const Annotation& ann,
                              double bitmapDpiScale, double coordScale) {
    double left = std::min(ann.x1, ann.x2);
    double right = std::max(ann.x1, ann.x2);
    double bottom = std::min(ann.y1, ann.y2);
    double top = std::max(ann.y1, ann.y2);
    double wPt = right - left;
    double hPt = top - bottom;
    if (wPt <= 0.1 || hPt <= 0.1) return;
    int wPx = 0, hPx = 0, stride = 0;
    std::vector<uint8_t> buf;
    constexpr double kBaseDpi = 216.0;
    if (!RenderMathAnnotBitmap(ann, wPt, hPt, kBaseDpi * bitmapDpiScale, coordScale,
                               buf, wPx, hPx, stride)) return;

    FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(dest);
    if (!img) return;
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(wPx, hPx, FPDFBitmap_BGRA, buf.data(), stride);
    if (!bmp) return;
    if (!FPDFImageObj_SetBitmap(&page, 1, img, bmp)) {
        FPDFBitmap_Destroy(bmp);
        return;
    }
    FPDFBitmap_Destroy(bmp);
    FPDFImageObj_SetMatrix(img,
                           static_cast<float>(wPt), 0.0f,
                           0.0f, static_cast<float>(-hPt),
                           static_cast<float>(left), static_cast<float>(bottom + hPt));
    FPDFPage_InsertObject(page, img);
}

static COLORREF DarkenColor(COLORREF c, double factor = 0.85) {
    factor = std::clamp(factor, 0.0, 1.0);
    int r = static_cast<int>(std::round(GetRValue(c) * factor));
    int g = static_cast<int>(std::round(GetGValue(c) * factor));
    int b = static_cast<int>(std::round(GetBValue(c) * factor));
    return RGB(r, g, b);
}

static Annotation ScaleAnnotationForExport(const Annotation& src, double coordScale) {
    if (coordScale == 1.0) return src;
    Annotation scaled = src;
    scaled.x1 *= coordScale;
    scaled.y1 *= coordScale;
    scaled.x2 *= coordScale;
    scaled.y2 *= coordScale;
    scaled.width *= coordScale;
    scaled.fontPt *= coordScale;
    for (auto& pt : scaled.path) {
        pt.x *= coordScale;
        pt.y *= coordScale;
    }
    for (double& v : scaled.quads) {
        v *= coordScale;
    }
    return scaled;
}

static void AddAnnotationsToPage(const std::vector<Annotation>& annots,
                                 PdfTextFontCache& fonts,
                                 FPDF_DOCUMENT dest, FPDF_PAGE page, int pageIndex,
                                 bool standardTextAnnots,
                                 bool matchPdfPaneTextLayout,
                                 double coordScale, double bitmapDpiScale) {
    for (const auto& ann : annots) {
        if (ann.pageIndex != pageIndex) continue;
        const Annotation* annPtr = &ann;
        Annotation scaled;
        if (coordScale != 1.0) {
            scaled = ScaleAnnotationForExport(ann, coordScale);
            annPtr = &scaled;
        }
        const Annotation& a = *annPtr;
        switch (a.type) {
        case Annotation::Type::MarkerText: {
            COLORREF col = DarkenColor(a.color, 0.85);
            BYTE alpha = AlphaFromNorm(a.alpha > 0.0 ? a.alpha : kMarkerAlphaDefault);
            auto addRect = [&](double left, double bottom, double right, double top) {
                double w = right - left;
                double h = top - bottom;
                if (w <= 0.01 || h <= 0.01) return;
                FPDF_PAGEOBJECT rect = FPDFPageObj_CreateNewRect(static_cast<float>(left),
                                                                 static_cast<float>(bottom),
                                                                 static_cast<float>(w),
                                                                 static_cast<float>(h));
                if (!rect) return;
                FPDFPageObj_SetFillColor(rect, GetRValue(col), GetGValue(col), GetBValue(col), alpha);
                FPDFPath_SetDrawMode(rect, FPDF_FILLMODE_ALTERNATE, FALSE);
                FPDFPage_InsertObject(page, rect);
            };
            if (!a.quads.empty()) {
                for (size_t qi = 0; qi + 7 < a.quads.size(); qi += 8) {
                    double minX = std::min(std::min(a.quads[qi + 0], a.quads[qi + 2]), std::min(a.quads[qi + 4], a.quads[qi + 6]));
                    double maxX = std::max(std::max(a.quads[qi + 0], a.quads[qi + 2]), std::max(a.quads[qi + 4], a.quads[qi + 6]));
                    double minY = std::min(std::min(a.quads[qi + 1], a.quads[qi + 3]), std::min(a.quads[qi + 5], a.quads[qi + 7]));
                    double maxY = std::max(std::max(a.quads[qi + 1], a.quads[qi + 3]), std::max(a.quads[qi + 5], a.quads[qi + 7]));
                    addRect(minX, minY, maxX, maxY);
                }
            } else {
                addRect(std::min(a.x1, a.x2), std::min(a.y1, a.y2),
                        std::max(a.x1, a.x2), std::max(a.y1, a.y2));
            }
            break;
        }
        case Annotation::Type::TextColor:
            // Exact text recoloring requires changing existing page text content; blocked before PDF export.
            break;
        case Annotation::Type::MarkerFree:
        case Annotation::Type::Freehand: {
            if (a.path.size() < 2) break;
            FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(static_cast<float>(a.path[0].x),
                                                             static_cast<float>(a.path[0].y));
            if (!path) break;
            for (size_t i = 1; i < a.path.size(); ++i) {
                FPDFPath_LineTo(path, static_cast<float>(a.path[i].x), static_cast<float>(a.path[i].y));
            }
            double defAlpha = (a.type == Annotation::Type::MarkerFree) ? kMarkerAlphaDefault : 1.0;
            BYTE alpha = AlphaFromNorm(a.alpha > 0.0 ? a.alpha : defAlpha);
            COLORREF col = (a.type == Annotation::Type::MarkerFree) ? DarkenColor(a.color, 0.85) : a.color;
            SetStrokeAndWidth(path, col, alpha, a.width, true);
            FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, TRUE);
            FPDFPage_InsertObject(page, path);
            break;
        }
        case Annotation::Type::Line:
        case Annotation::Type::Arrow:
        case Annotation::Type::Wave: {
            FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(static_cast<float>(a.x1), static_cast<float>(a.y1));
            if (!path) break;
            double penW = a.width > 0.0 ? a.width : 2.0;
            bool hasSegments = false;

            if (a.type == Annotation::Type::Arrow) {
                double dx = a.x2 - a.x1;
                double dy = a.y2 - a.y1;
                double len = std::hypot(dx, dy);
                if (len > 1e-3) {
                    double ux = dx / len;
                    double uy = dy / len;
                    double headLen = ComputeArrowHeadLength(len, std::max(PtFromScreenPx(8.0), penW * 3.0));
                    double headW = headLen * 0.5;
                    double hx1 = a.x2 - ux * headLen + (-uy) * headW;
                    double hy1 = a.y2 - uy * headLen + (ux) * headW;
                    double hx2 = a.x2 - ux * headLen - (-uy) * headW;
                    double hy2 = a.y2 - uy * headLen - (ux) * headW;

                    hasSegments = AppendDashedPdfSegment(path, a.x1, a.y1, a.x2, a.y2, a.dash);
                    FPDFPath_MoveTo(path, static_cast<float>(hx1), static_cast<float>(hy1));
                    FPDFPath_LineTo(path, static_cast<float>(a.x2), static_cast<float>(a.y2));
                    FPDFPath_MoveTo(path, static_cast<float>(hx2), static_cast<float>(hy2));
                    FPDFPath_LineTo(path, static_cast<float>(a.x2), static_cast<float>(a.y2));
                    if (a.arrowHead == ArrowHead::Double) {
                        const double sx1 = a.x1 + ux * headLen + (-uy) * headW;
                        const double sy1 = a.y1 + uy * headLen + (ux) * headW;
                        const double sx2 = a.x1 + ux * headLen - (-uy) * headW;
                        const double sy2 = a.y1 + uy * headLen - (ux) * headW;
                        FPDFPath_MoveTo(path, static_cast<float>(sx1), static_cast<float>(sy1));
                        FPDFPath_LineTo(path, static_cast<float>(a.x1), static_cast<float>(a.y1));
                        FPDFPath_MoveTo(path, static_cast<float>(sx2), static_cast<float>(sy2));
                        FPDFPath_LineTo(path, static_cast<float>(a.x1), static_cast<float>(a.y1));
                    }
                    hasSegments = true;
                } else {
                    hasSegments = AppendDashedPdfSegment(path, a.x1, a.y1, a.x2, a.y2, a.dash);
                }
            } else if (a.type == Annotation::Type::Wave) {
                double dx = a.x2 - a.x1;
                double dy = a.y2 - a.y1;
                double len = std::hypot(dx, dy);
                if (len > 1e-3) {
                    double ux = dx / len;
                    double uy = dy / len;
                    // PDF coordinates use Y-up, while viewer pixel coordinates use Y-down.
                    // Flip the normal to keep wave crest/trough direction consistent in export.
                    double nx = uy;
                    double ny = -ux;
                    double amplitude = std::max(PtFromScreenPx(1.0), penW * 0.45);
                    double wavelength = std::max(PtFromScreenPx(8.0), penW * 6.0);
                    double wl = std::max(PtFromScreenPx(4.0), wavelength);
                    double amp = std::max(PtFromScreenPx(1.0), amplitude);
                    int steps = static_cast<int>(std::max(2.0, std::ceil(len / (wl / 4.0))));
                    constexpr double kPi = 3.14159265358979323846;
                    std::vector<double> dashPattern;
                    if (!a.dash.empty()) {
                        dashPattern.reserve(a.dash.size() == 1 ? 2 : a.dash.size());
                        for (double part : a.dash) {
                            if (std::isfinite(part) && part > 0.0) {
                                dashPattern.push_back(std::max(PtFromScreenPx(1.0), part));
                            }
                        }
                        if (dashPattern.size() == 1) dashPattern.push_back(dashPattern.front());
                    }
                    size_t dashIndex = 0;
                    bool dashDraw = true;
                    double dashRemaining = dashPattern.empty() ? std::numeric_limits<double>::infinity()
                                                               : dashPattern.front();
                    double prevX = a.x1;
                    double prevY = a.y1;
                    for (int i = 1; i <= steps; ++i) {
                        double t = len * static_cast<double>(i) / static_cast<double>(steps);
                        double phase = (2.0 * kPi * t) / wl;
                        double offset = std::sin(phase) * amp;
                        double x = a.x1 + ux * t + nx * offset;
                        double y = a.y1 + uy * t + ny * offset;
                        if (dashPattern.empty()) {
                            FPDFPath_LineTo(path, static_cast<float>(x), static_cast<float>(y));
                        } else {
                            double segDx = x - prevX;
                            double segDy = y - prevY;
                            double segLen = std::hypot(segDx, segDy);
                            if (segLen > 1e-6) {
                                double segUx = segDx / segLen;
                                double segUy = segDy / segLen;
                                double pos = 0.0;
                                while (pos < segLen - 1e-6) {
                                    double step = std::min(dashRemaining, segLen - pos);
                                    double sx = prevX + segUx * pos;
                                    double sy = prevY + segUy * pos;
                                    double ex = prevX + segUx * (pos + step);
                                    double ey = prevY + segUy * (pos + step);
                                    if (dashDraw && step > 1e-6) {
                                        FPDFPath_MoveTo(path, static_cast<float>(sx), static_cast<float>(sy));
                                        FPDFPath_LineTo(path, static_cast<float>(ex), static_cast<float>(ey));
                                    }
                                    pos += step;
                                    dashRemaining -= step;
                                    if (dashRemaining <= 1e-6) {
                                        ++dashIndex;
                                        dashDraw = !dashDraw;
                                        dashRemaining = dashPattern[dashIndex % dashPattern.size()];
                                    }
                                }
                            }
                        }
                        prevX = x;
                        prevY = y;
                    }
                    hasSegments = true;
                } else {
                    AppendDashedPdfSegment(path, a.x1, a.y1, a.x2, a.y2, a.dash);
                    hasSegments = true;
                }
            } else {
                hasSegments = AppendDashedPdfSegment(path, a.x1, a.y1, a.x2, a.y2, a.dash);
            }

            if (!hasSegments) {
                break;
            }

            BYTE alpha = AlphaFromNorm(a.alpha > 0.0 ? a.alpha : 1.0);
            SetStrokeAndWidth(path, a.color, alpha, penW, true);
            FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, TRUE);
            FPDFPage_InsertObject(page, path);
            break;
        }
        case Annotation::Type::Shape: {
            double left = std::min(a.x1, a.x2);
            double right = std::max(a.x1, a.x2);
            double bottom = std::min(a.y1, a.y2);
            double top = std::max(a.y1, a.y2);
            BYTE alpha = AlphaFromNorm(a.alpha > 0.0 ? a.alpha : 0.35);
            const bool outline = a.shapeDrawMode == ShapeDrawMode::Outline;
            const double strokeW = a.width > 0.0 ? a.width : 2.5;
            auto applyShapeStyle = [&](FPDF_PAGEOBJECT obj) {
                if (!obj) return;
                if (outline) {
                    SetStrokeAndWidth(obj, a.color, alpha, strokeW, true);
                    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_NONE, TRUE);
                } else {
                    FPDFPageObj_SetFillColor(obj, GetRValue(a.color), GetGValue(a.color), GetBValue(a.color), alpha);
                    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_ALTERNATE, FALSE);
                }
                FPDFPage_InsertObject(page, obj);
            };
            if (a.shapeKind == ShapeKind::Rectangle || a.shapeKind == ShapeKind::Square) {
                double w = right - left;
                double h = top - bottom;
                if (w <= 0.01 || h <= 0.01) break;
                applyShapeStyle(FPDFPageObj_CreateNewRect(static_cast<float>(left),
                                                          static_cast<float>(bottom),
                                                          static_cast<float>(w),
                                                          static_cast<float>(h)));
                break;
            }

            const double midX = (left + right) * 0.5;
            const double midY = (bottom + top) * 0.5;
            FPDF_PAGEOBJECT path = nullptr;
            if (a.shapeKind == ShapeKind::Ellipse || a.shapeKind == ShapeKind::Circle ||
                a.shapeKind == ShapeKind::RotatedEllipse) {
                constexpr int kSegs = 32;
                const double rx = (right - left) * 0.5;
                const double ry = (top - bottom) * 0.5;
                const bool rotated = a.shapeKind == ShapeKind::RotatedEllipse;
                const double visualAngle = std::isfinite(a.shapeRotation) ? a.shapeRotation : kDefaultRotatedEllipseAngleRad;
                const double pdfAngle = rotated ? -visualAngle : 0.0;
                const double c = rotated ? std::cos(pdfAngle) : 1.0;
                const double s = rotated ? std::sin(pdfAngle) : 0.0;
                const double extentX = std::sqrt((rx * c) * (rx * c) + (ry * s) * (ry * s));
                const double extentY = std::sqrt((rx * s) * (rx * s) + (ry * c) * (ry * c));
                const double fit = rotated
                    ? std::min(rx / std::max(0.5, extentX), ry / std::max(0.5, extentY))
                    : 1.0;
                auto ellipseX = [&](double t) {
                    const double x = rx * fit * std::cos(t);
                    const double y = ry * fit * std::sin(t);
                    return midX + x * c - y * s;
                };
                auto ellipseY = [&](double t) {
                    const double x = rx * fit * std::cos(t);
                    const double y = ry * fit * std::sin(t);
                    return midY + x * s + y * c;
                };
                path = FPDFPageObj_CreateNewPath(static_cast<float>(ellipseX(0.0)),
                                                 static_cast<float>(ellipseY(0.0)));
                if (path) {
                    for (int i = 1; i <= kSegs; ++i) {
                        double t = (2.0 * 3.14159265358979323846 * i) / kSegs;
                        FPDFPath_LineTo(path, static_cast<float>(ellipseX(t)), static_cast<float>(ellipseY(t)));
                    }
                    FPDFPath_Close(path);
                }
            } else if (a.shapeKind == ShapeKind::Diamond) {
                path = FPDFPageObj_CreateNewPath(static_cast<float>(midX), static_cast<float>(top));
                if (path) {
                    FPDFPath_LineTo(path, static_cast<float>(right), static_cast<float>(midY));
                    FPDFPath_LineTo(path, static_cast<float>(midX), static_cast<float>(bottom));
                    FPDFPath_LineTo(path, static_cast<float>(left), static_cast<float>(midY));
                    FPDFPath_Close(path);
                }
            } else {
                path = FPDFPageObj_CreateNewPath(static_cast<float>(midX), static_cast<float>(top));
                if (path) {
                    FPDFPath_LineTo(path, static_cast<float>(right), static_cast<float>(bottom));
                    FPDFPath_LineTo(path, static_cast<float>(left), static_cast<float>(bottom));
                    FPDFPath_Close(path);
                }
            }
            applyShapeStyle(path);
            break;
        }
        case Annotation::Type::TextBox:
            AddTextAnnotation(fonts, dest, page, a, standardTextAnnots, matchPdfPaneTextLayout,
                              bitmapDpiScale, coordScale);
            break;
        case Annotation::Type::MathBox:
            AddMathAnnotation(dest, page, a, bitmapDpiScale, coordScale);
            break;
        case Annotation::Type::LinkMarker: {
            double radius = (a.width > 0.0) ? a.width : 6.0;
            double cx = a.x1;
            double cy = a.y1;
            constexpr int kSegs = 16;
            auto addCirclePath = [&](double r, bool fill) {
                FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(static_cast<float>(cx + r),
                                                                 static_cast<float>(cy));
                if (!path) return;
                for (int i = 1; i <= kSegs; ++i) {
                    double t = (2.0 * 3.14159265358979323846 * i) / kSegs;
                    double px = cx + r * std::cos(t);
                    double py = cy + r * std::sin(t);
                    FPDFPath_LineTo(path, static_cast<float>(px), static_cast<float>(py));
                }
                FPDFPath_Close(path);
                BYTE alpha = AlphaFromNorm(1.0);
                if (fill) {
                    FPDFPageObj_SetFillColor(path, GetRValue(a.color), GetGValue(a.color), GetBValue(a.color), alpha);
                    FPDFPath_SetDrawMode(path, FPDF_FILLMODE_ALTERNATE, FALSE);
                } else {
                    SetStrokeAndWidth(path, a.color, alpha, std::max(1.0, radius / 3.0), true);
                    FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, TRUE);
                }
                FPDFPage_InsertObject(page, path);
            };
            addCirclePath(radius, false);
            addCirclePath(std::max(1.0, radius / 3.0), true);
            break;
        }
        default:
            break;
        }
    }
}

static void FillRectAlpha(HDC hdc, const RECT& r, COLORREF color, BYTE alpha) {
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    double a = alpha / 255.0;
    BYTE rC = static_cast<BYTE>(std::round(GetRValue(color) * a));
    BYTE gC = static_cast<BYTE>(std::round(GetGValue(color) * a));
    BYTE bC = static_cast<BYTE>(std::round(GetBValue(color) * a));
    DWORD premul = (static_cast<DWORD>(alpha) << 24) | (rC << 16) | (gC << 8) | bC;
    for (int y = 0; y < h; ++y) {
        DWORD* row = buf.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            row[x] = premul;
        }
    }
    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static bool IsTextPixel(BYTE r, BYTE g, BYTE b) {
    int maxc = std::max(r, std::max(g, b));
    int minc = std::min(r, std::min(g, b));
    if (maxc <= kMarkerTextDarkMax) return true;
    if (maxc >= kMarkerTextBrightMin) return minc <= kMarkerTextBrightMinChannel;
    return (maxc - minc) >= kMarkerTextSaturationMin;
}

static bool IsBlackTextPixel(BYTE r, BYTE g, BYTE b) {
    int maxc = std::max(r, std::max(g, b));
    int minc = std::min(r, std::min(g, b));
    if (maxc > kMarkerTextBlackMax) return false;
    return (maxc - minc) <= kMarkerTextBlackSatMax;
}

static void FillRectAlphaMaskedByText(HDC hdc, const RECT& r, COLORREF color, BYTE alpha,
                                      const uint8_t* pagePixels, int pageStride,
                                      int pageW, int pageH,
                                      const std::vector<RECT>& textRects, double textAlphaScale) {
    if (!pagePixels || pageStride <= 0 || pageW <= 0 || pageH <= 0 || textRects.empty() || alpha == 0 || textAlphaScale >= 1.0) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    double scale = std::clamp(textAlphaScale, 0.0, 1.0);
    BYTE textAlpha = static_cast<BYTE>(std::round(alpha * scale));
    if (textAlpha == alpha) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }

    RECT pageRect{ r.left, r.top, r.right, r.bottom };
    RECT pageBounds{ 0, 0, pageW, pageH };
    RECT clippedPageRect{};
    clippedPageRect.left = std::clamp(pageRect.left, pageBounds.left, pageBounds.right);
    clippedPageRect.right = std::clamp(pageRect.right, pageBounds.left, pageBounds.right);
    clippedPageRect.top = std::clamp(pageRect.top, pageBounds.top, pageBounds.bottom);
    clippedPageRect.bottom = std::clamp(pageRect.bottom, pageBounds.top, pageBounds.bottom);
    if (clippedPageRect.left >= clippedPageRect.right || clippedPageRect.top >= clippedPageRect.bottom) {
        FillRectAlpha(hdc, r, color, alpha);
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    double baseA = alpha / 255.0;
    BYTE rBase = static_cast<BYTE>(std::round(GetRValue(color) * baseA));
    BYTE gBase = static_cast<BYTE>(std::round(GetGValue(color) * baseA));
    BYTE bBase = static_cast<BYTE>(std::round(GetBValue(color) * baseA));
    DWORD basePremul = (static_cast<DWORD>(alpha) << 24) | (rBase << 16) | (gBase << 8) | bBase;

    double textA = textAlpha / 255.0;
    BYTE rText = static_cast<BYTE>(std::round(GetRValue(color) * textA));
    BYTE gText = static_cast<BYTE>(std::round(GetGValue(color) * textA));
    BYTE bText = static_cast<BYTE>(std::round(GetBValue(color) * textA));
    DWORD textPremul = (static_cast<DWORD>(textAlpha) << 24) | (rText << 16) | (gText << 8) | bText;

    std::fill(buf.begin(), buf.end(), basePremul);
    for (const auto& tr : textRects) {
        RECT inter{};
        if (!IntersectRect(&inter, &tr, &clippedPageRect)) continue;
        for (int py = inter.top; py < inter.bottom; ++py) {
            const uint8_t* src = pagePixels + static_cast<size_t>(py) * pageStride;
            for (int px = inter.left; px < inter.right; ++px) {
                size_t idx = static_cast<size_t>(px) * 4;
                BYTE b = src[idx + 0];
                BYTE g = src[idx + 1];
                BYTE rC = src[idx + 2];
                if (IsTextPixel(rC, g, b)) {
                    int row = py - clippedPageRect.top;
                    int col = px - clippedPageRect.left;
                    size_t dstIdx = static_cast<size_t>(row) * w + static_cast<size_t>(col);
                    buf[dstIdx] = IsBlackTextPixel(rC, g, b) ? 0 : textPremul;
                }
            }
        }
    }

    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static void TintRectTextPixels(HDC hdc, const RECT& r, COLORREF color,
                               const uint8_t* pagePixels, int pageStride,
                               int pageW, int pageH,
                               const std::vector<RECT>& textRects) {
    if (!pagePixels || pageStride <= 0 || pageW <= 0 || pageH <= 0 || textRects.empty()) return;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    RECT bounds{ 0, 0, pageW, pageH };
    RECT clipped{};
    if (!IntersectRect(&clipped, &r, &bounds)) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<DWORD> overlay(static_cast<size_t>(w) * h, 0);
    for (const auto& textRect : textRects) {
        RECT inter{};
        if (!IntersectRect(&inter, &textRect, &clipped)) continue;
        for (int py = inter.top; py < inter.bottom; ++py) {
            const uint8_t* src = pagePixels + static_cast<size_t>(py) * pageStride;
            DWORD* dst = overlay.data() + static_cast<size_t>(py - r.top) * w;
            for (int px = inter.left; px < inter.right; ++px) {
                const size_t srcIndex = static_cast<size_t>(px) * 4;
                const BYTE b = src[srcIndex + 0];
                const BYTE g = src[srcIndex + 1];
                const BYTE red = src[srcIndex + 2];
                if (!IsTextPixel(red, g, b)) continue;
                const int maxc = std::max<int>(red, std::max<int>(g, b));
                const int minc = std::min<int>(red, std::min<int>(g, b));
                const BYTE alpha = static_cast<BYTE>(
                    std::clamp(IsBlackTextPixel(red, g, b) ? 255 - maxc : 255 - minc, 1, 255));
                const double a = alpha / 255.0;
                const BYTE outR = static_cast<BYTE>(std::lround(GetRValue(color) * a));
                const BYTE outG = static_cast<BYTE>(std::lround(GetGValue(color) * a));
                const BYTE outB = static_cast<BYTE>(std::lround(GetBValue(color) * a));
                dst[px - r.left] = (static_cast<DWORD>(alpha) << 24) |
                    (static_cast<DWORD>(outR) << 16) |
                    (static_cast<DWORD>(outG) << 8) |
                    static_cast<DWORD>(outB);
            }
        }
    }
    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, overlay.data(), overlay.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static std::vector<RECT> BuildTextRectsForPage(const PageCache& page, double dpi) {
    std::vector<RECT> rects;
    if (dpi <= 0.0 || page.charBoxes.empty()) return rects;
    double scale = dpi / 72.0;
    int widthPx = static_cast<int>(std::round(page.widthPt * scale));
    int heightPx = static_cast<int>(std::round(page.heightPt * scale));
    if (widthPx <= 0 || heightPx <= 0) return rects;
    rects.reserve(page.charBoxes.size());
    for (const auto& cb : page.charBoxes) {
        double left = cb.left * scale;
        double right = cb.right * scale;
        double top = (page.heightPt - cb.top) * scale;
        double bottom = (page.heightPt - cb.bottom) * scale;
        RECT rc{};
        rc.left = static_cast<int>(std::round(std::min(left, right)));
        rc.right = static_cast<int>(std::round(std::max(left, right)));
        rc.top = static_cast<int>(std::round(std::min(top, bottom)));
        rc.bottom = static_cast<int>(std::round(std::max(top, bottom)));
        rc.left = static_cast<int>(std::clamp<long>(rc.left, 0L, static_cast<long>(widthPx)));
        rc.right = static_cast<int>(std::clamp<long>(rc.right, 0L, static_cast<long>(widthPx)));
        rc.top = static_cast<int>(std::clamp<long>(rc.top, 0L, static_cast<long>(heightPx)));
        rc.bottom = static_cast<int>(std::clamp<long>(rc.bottom, 0L, static_cast<long>(heightPx)));
        if (rc.left >= rc.right || rc.top >= rc.bottom) continue;
        rects.push_back(rc);
    }
    return rects;
}

static std::vector<POINT> BuildWavePointsPx(const POINT& p1, const POINT& p2, double amplitude, double wavelength) {
    std::vector<POINT> pts;
    double dx = static_cast<double>(p2.x - p1.x);
    double dy = static_cast<double>(p2.y - p1.y);
    double len = std::hypot(dx, dy);
    if (len < 1e-3) {
        pts.push_back(p1);
        pts.push_back(p2);
        return pts;
    }
    double ux = dx / len;
    double uy = dy / len;
    double nx = -uy;
    double ny = ux;
    double wl = std::max(4.0, wavelength);
    double amp = std::max(1.0, amplitude);
    int steps = static_cast<int>(std::max(2.0, std::ceil(len / (wl / 4.0))));
    pts.reserve(static_cast<size_t>(steps) + 1);
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i <= steps; ++i) {
        double t = len * static_cast<double>(i) / static_cast<double>(steps);
        double phase = (2.0 * kPi * t) / wl;
        double offset = std::sin(phase) * amp;
        double x = static_cast<double>(p1.x) + ux * t + nx * offset;
        double y = static_cast<double>(p1.y) + uy * t + ny * offset;
        pts.push_back(POINT{ static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)) });
    }
    return pts;
}

static void DrawPolylineAlphaPx(HDC hdc, const std::vector<POINT>& pts, int penW, COLORREF color, BYTE alpha);

static void DrawArrowAlphaPx(HDC hdc, const POINT& p1, const POINT& p2, int penW, COLORREF color, BYTE alpha,
                             ArrowHead arrowHead) {
    double dx = static_cast<double>(p2.x - p1.x);
    double dy = static_cast<double>(p2.y - p1.y);
    double len = std::hypot(dx, dy);
    if (len < 1e-3) return;
    double ux = dx / len;
    double uy = dy / len;
    double headLen = ComputeArrowHeadLength(len, std::max(8.0, static_cast<double>(penW) * 3.0));
    double headW = headLen * 0.5;
    double hx1 = static_cast<double>(p2.x) - ux * headLen + (-uy) * headW;
    double hy1 = static_cast<double>(p2.y) - uy * headLen + (ux) * headW;
    double hx2 = static_cast<double>(p2.x) - ux * headLen - (-uy) * headW;
    double hy2 = static_cast<double>(p2.y) - uy * headLen - (ux) * headW;

    POINT ph1{ static_cast<LONG>(std::lround(hx1)), static_cast<LONG>(std::lround(hy1)) };
    POINT ph2{ static_cast<LONG>(std::lround(hx2)), static_cast<LONG>(std::lround(hy2)) };

    DrawPolylineAlphaPx(hdc, {p1, p2}, penW, color, alpha);
    DrawPolylineAlphaPx(hdc, {ph1, p2}, penW, color, alpha);
    DrawPolylineAlphaPx(hdc, {ph2, p2}, penW, color, alpha);
    if (arrowHead == ArrowHead::Double) {
        POINT start1{ static_cast<LONG>(std::lround(static_cast<double>(p1.x) + ux * headLen + (-uy) * headW)),
                      static_cast<LONG>(std::lround(static_cast<double>(p1.y) + uy * headLen + (ux) * headW)) };
        POINT start2{ static_cast<LONG>(std::lround(static_cast<double>(p1.x) + ux * headLen - (-uy) * headW)),
                      static_cast<LONG>(std::lround(static_cast<double>(p1.y) + uy * headLen - (ux) * headW)) };
        DrawPolylineAlphaPx(hdc, {start1, p1}, penW, color, alpha);
        DrawPolylineAlphaPx(hdc, {start2, p1}, penW, color, alpha);
    }
}

static void DrawWaveAlphaPx(HDC hdc, const POINT& p1, const POINT& p2, int penW, COLORREF color, BYTE alpha) {
    double amplitude = std::max(1.0, static_cast<double>(penW) * 0.45);
    double wavelength = std::max(8.0, static_cast<double>(penW) * 6.0);
    auto pts = BuildWavePointsPx(p1, p2, amplitude, wavelength);
    if (pts.size() < 2) return;
    DrawPolylineAlphaPx(hdc, pts, penW, color, alpha);
}

static void DrawPolylineAlphaPx(HDC hdc, const std::vector<POINT>& pts, int penW, COLORREF color, BYTE alpha) {
    if (pts.size() < 2 || penW <= 0 || alpha == 0) return;
    LONG minX = pts[0].x, maxX = pts[0].x;
    LONG minY = pts[0].y, maxY = pts[0].y;
    for (const auto& p : pts) {
        minX = std::min<LONG>(minX, p.x);
        maxX = std::max<LONG>(maxX, p.x);
        minY = std::min<LONG>(minY, p.y);
        maxY = std::max<LONG>(maxY, p.y);
    }
    int margin = penW / 2 + 2;
    int w = (maxX - minX + 1) + margin * 2;
    int h = (maxY - minY + 1) + margin * 2;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        return;
    }
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(w) * h, 0);

    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ oldBmp = SelectObject(mem, hbmp);
    HPEN pen = CreatePen(PS_SOLID, penW, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    std::vector<POINT> local;
    local.reserve(pts.size());
    for (const auto& p : pts) {
        POINT lp{};
        lp.x = p.x - minX + margin;
        lp.y = p.y - minY + margin;
        local.push_back(lp);
    }
    Polyline(mem, local.data(), static_cast<int>(local.size()));
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    DWORD* px = static_cast<DWORD*>(bits);
    double a = alpha / 255.0;
    BYTE rT = static_cast<BYTE>(std::round(GetRValue(color) * a));
    BYTE gT = static_cast<BYTE>(std::round(GetGValue(color) * a));
    BYTE bT = static_cast<BYTE>(std::round(GetBValue(color) * a));
    for (int i = 0; i < w * h; ++i) {
        if (px[i] == 0) {
            px[i] = 0;
            continue;
        }
        px[i] = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(rT) << 16) |
                (static_cast<DWORD>(gT) << 8) | bT;
    }

    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, minX - margin, minY - margin, w, h, mem, 0, 0, w, h, bf);

    SelectObject(mem, oldBmp);
    DeleteObject(hbmp);
    DeleteDC(mem);
}

static void FillPolygonAlphaPx(HDC hdc, const std::vector<POINT>& pts, COLORREF color, BYTE alpha) {
    if (pts.size() < 3 || alpha == 0) return;
    LONG minX = pts[0].x;
    LONG maxX = pts[0].x;
    LONG minY = pts[0].y;
    LONG maxY = pts[0].y;
    for (const auto& p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    int w = static_cast<int>(maxX - minX + 1);
    int h = static_cast<int>(maxY - minY + 1);
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    const double a = alpha / 255.0;
    const DWORD premul = (static_cast<DWORD>(alpha) << 24) |
                         (static_cast<DWORD>(std::lround(GetRValue(color) * a)) << 16) |
                         (static_cast<DWORD>(std::lround(GetGValue(color) * a)) << 8) |
                         static_cast<DWORD>(std::lround(GetBValue(color) * a));

    std::vector<POINT> local;
    local.reserve(pts.size());
    for (const auto& p : pts) {
        local.push_back(POINT{ p.x - minX, p.y - minY });
    }
    auto inside = [&](double x, double y) {
        bool hit = false;
        size_t j = local.size() - 1;
        for (size_t i = 0; i < local.size(); ++i) {
            const double xi = static_cast<double>(local[i].x);
            const double yi = static_cast<double>(local[i].y);
            const double xj = static_cast<double>(local[j].x);
            const double yj = static_cast<double>(local[j].y);
            const bool intersect = ((yi > y) != (yj > y)) &&
                                   (x < (xj - xi) * (y - yi) / ((yj - yi) == 0.0 ? 1.0 : (yj - yi)) + xi);
            if (intersect) hit = !hit;
            j = i;
        }
        return hit;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (inside(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5)) {
                buf[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = premul;
            }
        }
    }

    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, minX, minY, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static void FillEllipseAlphaPx(HDC hdc, const RECT& r, COLORREF color, BYTE alpha) {
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || alpha == 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> buf(static_cast<size_t>(w) * h, 0);
    const double a = alpha / 255.0;
    const DWORD premul = (static_cast<DWORD>(alpha) << 24) |
                         (static_cast<DWORD>(std::lround(GetRValue(color) * a)) << 16) |
                         (static_cast<DWORD>(std::lround(GetGValue(color) * a)) << 8) |
                         static_cast<DWORD>(std::lround(GetBValue(color) * a));
    const double rx = static_cast<double>(w) * 0.5;
    const double ry = static_cast<double>(h) * 0.5;
    if (rx <= 0.0 || ry <= 0.0) return;
    for (int y = 0; y < h; ++y) {
        double ny = ((static_cast<double>(y) + 0.5) - ry) / ry;
        for (int x = 0; x < w; ++x) {
            double nx = ((static_cast<double>(x) + 0.5) - rx) / rx;
            if (nx * nx + ny * ny <= 1.0) {
                buf[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = premul;
            }
        }
    }

    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        memcpy(bits, buf.data(), buf.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, hbmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
        SelectObject(mem, old);
        DeleteObject(hbmp);
    }
    DeleteDC(mem);
}

static std::vector<POINT> BuildEllipseOutlinePointsPx(const RECT& rc) {
    std::vector<POINT> pts;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return pts;
    constexpr int kSegs = 64;
    const double cx = (static_cast<double>(rc.left) + static_cast<double>(rc.right)) * 0.5;
    const double cy = (static_cast<double>(rc.top) + static_cast<double>(rc.bottom)) * 0.5;
    const double rx = static_cast<double>(w) * 0.5;
    const double ry = static_cast<double>(h) * 0.5;
    pts.reserve(kSegs + 1);
    for (int i = 0; i <= kSegs; ++i) {
        const double t = (2.0 * 3.14159265358979323846 * i) / kSegs;
        pts.push_back(POINT{
            static_cast<LONG>(std::lround(cx + rx * std::cos(t))),
            static_cast<LONG>(std::lround(cy + ry * std::sin(t)))
        });
    }
    return pts;
}

static double EffectiveRotatedEllipseAngleRad(double angleRad) {
    return std::isfinite(angleRad) ? angleRad : kDefaultRotatedEllipseAngleRad;
}

static std::vector<POINT> BuildRotatedEllipseOutlinePointsPx(const RECT& rc, double angleRad) {
    std::vector<POINT> pts;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return pts;
    constexpr int kSegs = 96;
    const double cx = (static_cast<double>(rc.left) + static_cast<double>(rc.right)) * 0.5;
    const double cy = (static_cast<double>(rc.top) + static_cast<double>(rc.bottom)) * 0.5;
    const double rx = std::max(0.5, static_cast<double>(w) * 0.5);
    const double ry = std::max(0.5, static_cast<double>(h) * 0.5);
    const double angle = EffectiveRotatedEllipseAngleRad(angleRad);
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double extentX = std::sqrt((rx * c) * (rx * c) + (ry * s) * (ry * s));
    const double extentY = std::sqrt((rx * s) * (rx * s) + (ry * c) * (ry * c));
    const double fit = std::min(rx / std::max(0.5, extentX), ry / std::max(0.5, extentY));
    pts.reserve(kSegs + 1);
    for (int i = 0; i <= kSegs; ++i) {
        const double t = (2.0 * 3.14159265358979323846 * i) / kSegs;
        const double x = std::cos(t) * rx * fit;
        const double y = std::sin(t) * ry * fit;
        pts.push_back(POINT{
            static_cast<LONG>(std::lround(cx + x * c - y * s)),
            static_cast<LONG>(std::lround(cy + x * s + y * c))
        });
    }
    return pts;
}

static void DrawShapeOutlineAlphaPx(HDC hdc, const RECT& rc, const std::vector<POINT>& polygon,
                                    int penW, COLORREF color, BYTE alpha) {
    if (penW <= 0 || alpha == 0) return;
    std::vector<POINT> pts;
    if (!polygon.empty()) {
        pts = polygon;
        if (pts.size() >= 2 &&
            (pts.front().x != pts.back().x || pts.front().y != pts.back().y)) {
            pts.push_back(pts.front());
        }
    } else {
        pts = {
            POINT{ rc.left, rc.top },
            POINT{ rc.right, rc.top },
            POINT{ rc.right, rc.bottom },
            POINT{ rc.left, rc.bottom },
            POINT{ rc.left, rc.top },
        };
    }
    DrawPolylineAlphaPx(hdc, pts, penW, color, alpha);
}

static void DrawMathAnnotationPx(HDC hdc, const RECT& rc, const Annotation& ann, double dpi) {
    if (!hdc || rc.left >= rc.right || rc.top >= rc.bottom || dpi <= 0.0) return;
    double fontPt = (ann.fontPt > 0.0) ? ann.fontPt : g_textFontPt;
    int fontPx = std::max(6, static_cast<int>(std::round(fontPt * dpi / 72.0)));
    LOGFONTW lf{};
    lf.lfHeight = -fontPx;
    std::wstring fontName = ann.fontName.empty() ? g_textFontName : ann.fontName;
    if (!fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, fontName.c_str(), LF_FACESIZE - 1);
    }
    HFONT hFont = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = hFont ? SelectObject(hdc, hFont) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ann.color);

    const int pad = std::max(2, static_cast<int>(std::round(4.0 * dpi / kDpi)));
    const int availW = std::max(1, static_cast<int>(rc.right - rc.left) - pad * 2);
    const int availH = std::max(1, static_cast<int>(rc.bottom - rc.top) - pad * 2);
    std::wstring body = NormalizeMathAnnotText(ann);
    auto node = mathrender::Parse(body);
    if (node) {
        auto layout = mathrender::Measure(*node, hdc, fontPx, mathrender::RenderStyle::Display);
        int drawFontPx = fontPx;
        if (layout.width > availW || layout.height > availH) {
            int shrink = std::max(2, fontPx / 4);
            drawFontPx = std::max(6, fontPx - shrink);
            layout = mathrender::Measure(*node, hdc, drawFontPx, mathrender::RenderStyle::Display);
        }
        mathrender::Draw(*node, layout, hdc, rc.left + pad, rc.top + pad, drawFontPx, ann.color,
                         mathrender::RenderStyle::Display);
    }

    if (oldFont) SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
}

static void DrawAnnotationsToBuffer(const std::vector<Annotation>& annots, int pageIndex, double pageHeightPt,
                                    double dpi, uint8_t* buf, int wPx, int hPx, int stride,
                                    AnnotationRenderMode mode, const PageCache* pageCache) {
    if (!buf || stride <= 0 || wPx <= 0 || hPx <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = wPx;
    bmi.bmiHeader.biHeight = -hPx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!hbmp || !dibBits) {
        if (hbmp) DeleteObject(hbmp);
        return;
    }
    memcpy(dibBits, buf, static_cast<size_t>(stride) * hPx);

    HDC hdc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBmp = SelectObject(hdc, hbmp);

    double scale = dpi / 72.0;
    std::vector<RECT> pageTextRects;
    if (pageCache) {
        pageTextRects = BuildTextRectsForPage(*pageCache, dpi);
    }
    const std::vector<RECT>* textRectsPtr =
        (mode == AnnotationRenderMode::ViewerLike && !pageTextRects.empty()) ? &pageTextRects : nullptr;
    auto PtToPxX = [&](double xPt) { return static_cast<int>(std::lround(xPt * scale)); };
    auto PtToPxY = [&](double yPt) { return static_cast<int>(std::lround((pageHeightPt - yPt) * scale)); };

    for (const auto& ann : annots) {
        if (ann.pageIndex != pageIndex) continue;
        switch (ann.type) {
        case Annotation::Type::TextColor: {
            if (pageTextRects.empty()) break;
            auto tintRectPt = [&](double leftPt, double bottomPt, double rightPt, double topPt) {
                const int l = PtToPxX(leftPt);
                const int r = PtToPxX(rightPt);
                const int t = PtToPxY(topPt);
                const int b = PtToPxY(bottomPt);
                RECT rc{ std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b) };
                TintRectTextPixels(hdc, rc, ann.color, buf, stride, wPx, hPx, pageTextRects);
            };
            if (!ann.quads.empty()) {
                for (size_t qi = 0; qi + 7 < ann.quads.size(); qi += 8) {
                    const double minX = std::min(std::min(ann.quads[qi + 0], ann.quads[qi + 2]), std::min(ann.quads[qi + 4], ann.quads[qi + 6]));
                    const double maxX = std::max(std::max(ann.quads[qi + 0], ann.quads[qi + 2]), std::max(ann.quads[qi + 4], ann.quads[qi + 6]));
                    const double minY = std::min(std::min(ann.quads[qi + 1], ann.quads[qi + 3]), std::min(ann.quads[qi + 5], ann.quads[qi + 7]));
                    const double maxY = std::max(std::max(ann.quads[qi + 1], ann.quads[qi + 3]), std::max(ann.quads[qi + 5], ann.quads[qi + 7]));
                    tintRectPt(minX, minY, maxX, maxY);
                }
            } else {
                tintRectPt(std::min(ann.x1, ann.x2), std::min(ann.y1, ann.y2),
                           std::max(ann.x1, ann.x2), std::max(ann.y1, ann.y2));
            }
            break;
        }
        case Annotation::Type::MarkerText: {
            COLORREF col = DarkenColor(ann.color, 0.85);
            BYTE a = AlphaFromNorm(ann.alpha > 0.0 ? ann.alpha : kMarkerAlphaDefault);
            auto fillRectPt = [&](double leftPt, double bottomPt, double rightPt, double topPt) {
                int l = PtToPxX(leftPt);
                int r = PtToPxX(rightPt);
                int t = PtToPxY(topPt);
                int b = PtToPxY(bottomPt);
                RECT rc{ std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b) };
                if (textRectsPtr) {
                    FillRectAlphaMaskedByText(hdc, rc, col, a, buf, stride, wPx, hPx,
                                              *textRectsPtr, kMarkerTextAlphaScaleOnText);
                } else {
                    FillRectAlpha(hdc, rc, col, a);
                }
            };
            if (!ann.quads.empty()) {
                for (size_t qi = 0; qi + 7 < ann.quads.size(); qi += 8) {
                    double minX = std::min(std::min(ann.quads[qi + 0], ann.quads[qi + 2]), std::min(ann.quads[qi + 4], ann.quads[qi + 6]));
                    double maxX = std::max(std::max(ann.quads[qi + 0], ann.quads[qi + 2]), std::max(ann.quads[qi + 4], ann.quads[qi + 6]));
                    double minY = std::min(std::min(ann.quads[qi + 1], ann.quads[qi + 3]), std::min(ann.quads[qi + 5], ann.quads[qi + 7]));
                    double maxY = std::max(std::max(ann.quads[qi + 1], ann.quads[qi + 3]), std::max(ann.quads[qi + 5], ann.quads[qi + 7]));
                    fillRectPt(minX, minY, maxX, maxY);
                }
            } else {
                fillRectPt(std::min(ann.x1, ann.x2), std::min(ann.y1, ann.y2),
                           std::max(ann.x1, ann.x2), std::max(ann.y1, ann.y2));
            }
            break;
        }
        case Annotation::Type::MarkerFree:
        case Annotation::Type::Freehand:
        case Annotation::Type::Line:
        case Annotation::Type::Arrow:
        case Annotation::Type::Wave: {
            double defAlpha = (ann.type == Annotation::Type::MarkerFree) ? kMarkerAlphaDefault : 1.0;
            double alphaNorm = ann.alpha > 0.0 ? ann.alpha : defAlpha;
            BYTE a = AlphaFromNorm(alphaNorm);
            COLORREF col = (ann.type == Annotation::Type::MarkerFree) ? DarkenColor(ann.color, 0.85) : ann.color;
            int penW = static_cast<int>(std::round(std::max(1.0, ann.width * scale)));

            if (ann.type == Annotation::Type::Arrow) {
                POINT p1{ PtToPxX(ann.x1), PtToPxY(ann.y1) };
                POINT p2{ PtToPxX(ann.x2), PtToPxY(ann.y2) };
                DrawArrowAlphaPx(hdc, p1, p2, penW, col, a, ann.arrowHead);
            } else if (ann.type == Annotation::Type::Wave) {
                POINT p1{ PtToPxX(ann.x1), PtToPxY(ann.y1) };
                POINT p2{ PtToPxX(ann.x2), PtToPxY(ann.y2) };
                DrawWaveAlphaPx(hdc, p1, p2, penW, col, a);
            } else {
                std::vector<POINT> pts;
                if (ann.type == Annotation::Type::Line) {
                    POINT p1{ PtToPxX(ann.x1), PtToPxY(ann.y1) };
                    POINT p2{ PtToPxX(ann.x2), PtToPxY(ann.y2) };
                    pts = { p1, p2 };
                } else {
                    if (ann.path.size() < 2) break;
                    pts.reserve(ann.path.size());
                    for (const auto& p : ann.path) {
                        pts.push_back(POINT{ PtToPxX(p.x), PtToPxY(p.y) });
                    }
                }
                if (pts.size() >= 2) {
                    DrawPolylineAlphaPx(hdc, pts, penW, col, a);
                }
            }
            break;
        }
        case Annotation::Type::Shape: {
            double leftPt = std::min(ann.x1, ann.x2);
            double rightPt = std::max(ann.x1, ann.x2);
            double bottomPt = std::min(ann.y1, ann.y2);
            double topPt = std::max(ann.y1, ann.y2);
            int l = PtToPxX(leftPt);
            int r = PtToPxX(rightPt);
            int t = PtToPxY(topPt);
            int b = PtToPxY(bottomPt);
            RECT rc{ std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b) };
            if (rc.left >= rc.right || rc.top >= rc.bottom) break;
            BYTE a = AlphaFromNorm(ann.alpha > 0.0 ? ann.alpha : 0.35);
            const bool outline = ann.shapeDrawMode == ShapeDrawMode::Outline;
            int penW = static_cast<int>(std::round(std::max(1.0, ((ann.width > 0.0) ? ann.width : 2.5) * scale)));
            if (ann.shapeKind == ShapeKind::Rectangle || ann.shapeKind == ShapeKind::Square) {
                if (outline) {
                    DrawShapeOutlineAlphaPx(hdc, rc, {}, penW, ann.color, a);
                } else {
                    FillRectAlpha(hdc, rc, ann.color, a);
                }
            } else if (ann.shapeKind == ShapeKind::Ellipse || ann.shapeKind == ShapeKind::Circle) {
                if (outline) {
                    DrawPolylineAlphaPx(hdc, BuildEllipseOutlinePointsPx(rc), penW, ann.color, a);
                } else {
                    FillEllipseAlphaPx(hdc, rc, ann.color, a);
                }
            } else if (ann.shapeKind == ShapeKind::RotatedEllipse) {
                const auto pts = BuildRotatedEllipseOutlinePointsPx(rc, EffectiveRotatedEllipseAngleRad(ann.shapeRotation));
                if (outline) {
                    DrawPolylineAlphaPx(hdc, pts, penW, ann.color, a);
                } else {
                    FillPolygonAlphaPx(hdc, pts, ann.color, a);
                }
            } else {
                const LONG midX = (rc.left + rc.right) / 2;
                const LONG midY = (rc.top + rc.bottom) / 2;
                std::vector<POINT> pts;
                if (ann.shapeKind == ShapeKind::Diamond) {
                    pts = {
                        POINT{ midX, rc.top },
                        POINT{ rc.right, midY },
                        POINT{ midX, rc.bottom },
                        POINT{ rc.left, midY },
                    };
                } else {
                    pts = {
                        POINT{ midX, rc.top },
                        POINT{ rc.right, rc.bottom },
                        POINT{ rc.left, rc.bottom },
                    };
                }
                if (outline) {
                    DrawShapeOutlineAlphaPx(hdc, rc, pts, penW, ann.color, a);
                } else {
                    FillPolygonAlphaPx(hdc, pts, ann.color, a);
                }
            }
            break;
        }
        case Annotation::Type::TextBox: {
            double leftPt = std::min(ann.x1, ann.x2);
            double rightPt = std::max(ann.x1, ann.x2);
            double bottomPt = std::min(ann.y1, ann.y2);
            double topPt = std::max(ann.y1, ann.y2);
            int l = PtToPxX(leftPt);
            int r = PtToPxX(rightPt);
            int t = PtToPxY(topPt);
            int b = PtToPxY(bottomPt);
            RECT rc{ std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b) };
            SetBkMode(hdc, TRANSPARENT);
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(static_cast<int>(std::round(ann.fontPt > 0 ? ann.fontPt : g_textFontPt)),
                                  static_cast<int>(dpi), 72);
            if (!ann.fontName.empty()) {
                wcsncpy_s(lf.lfFaceName, ann.fontName.c_str(), LF_FACESIZE - 1);
            } else {
                wcsncpy_s(lf.lfFaceName, g_textFontName.c_str(), LF_FACESIZE - 1);
            }
            HFONT hFont = CreateFontIndirectW(&lf);
            HGDIOBJ oldFont = nullptr;
            if (hFont) oldFont = SelectObject(hdc, hFont);
            SetTextColor(hdc, ann.color);
            int pad = 4;
            RECT tr{ rc.left + pad, rc.top + pad, rc.right - pad, rc.bottom - pad };
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            int lineHeight = std::max(1, static_cast<int>(tm.tmHeight));
            if (!ann.textLines.empty()) {
                int y = tr.top;
                int bottomLimit = tr.bottom;
                for (const auto& line : ann.textLines) {
                    if (y >= bottomLimit) break;
                    if (!line.empty()) {
                        TextOutW(hdc, tr.left, y, line.c_str(), static_cast<int>(line.size()));
                    }
                    y += lineHeight;
                }
            } else {
                DrawTextW(hdc, ann.text.c_str(), static_cast<int>(ann.text.size()), &tr,
                          DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
            }
            if (oldFont) SelectObject(hdc, oldFont);
            if (hFont) DeleteObject(hFont);
            break;
        }
        case Annotation::Type::MathBox: {
            double leftPt = std::min(ann.x1, ann.x2);
            double rightPt = std::max(ann.x1, ann.x2);
            double bottomPt = std::min(ann.y1, ann.y2);
            double topPt = std::max(ann.y1, ann.y2);
            int l = PtToPxX(leftPt);
            int r = PtToPxX(rightPt);
            int t = PtToPxY(topPt);
            int b = PtToPxY(bottomPt);
            RECT rc{ std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b) };
            DrawMathAnnotationPx(hdc, rc, ann, dpi);
            break;
        }
        case Annotation::Type::LinkMarker: {
            int cx = PtToPxX(ann.x1);
            int cy = PtToPxY(ann.y1);
            double radiusPt = (ann.width > 0.0) ? ann.width : 6.0;
            int radiusPx = static_cast<int>(std::round(radiusPt * scale));
            radiusPx = std::max(3, radiusPx);
            int dotPx = std::max(1, radiusPx / 3);
            int penW = std::max(1, radiusPx / 4);
            HPEN pen = CreatePen(PS_SOLID, penW, ann.color);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(HOLLOW_BRUSH)));
            Ellipse(hdc, cx - radiusPx, cy - radiusPx, cx + radiusPx, cy + radiusPx);
            HBRUSH dotBrush = CreateSolidBrush(ann.color);
            SelectObject(hdc, dotBrush);
            Ellipse(hdc, cx - dotPx, cy - dotPx, cx + dotPx, cy + dotPx);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(dotBrush);
            DeleteObject(pen);
            break;
        }
        default:
            break;
        }
    }

    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);
    memcpy(buf, dibBits, static_cast<size_t>(stride) * hPx);
    DeleteObject(hbmp);
}

static bool SavePdfDocument(FPDF_DOCUMENT doc, const std::wstring& outPath, std::wstring* err) {
    std::filesystem::path dest(outPath);
    if (dest.empty()) {
        if (err) *err = L"Invalid output path.";
        return false;
    }
    if (!dest.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            if (err) *err = L"Failed to create output folder: " + dest.parent_path().wstring();
            return false;
        }
    }

    std::filesystem::path tmp;
    std::wstring tmpErr;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (!atomic_write::CreateUniqueTempFile(dest, /*preferredTempDir=*/dest.parent_path(), &tmp, &hFile, &tmpErr)) {
        if (err) *err = tmpErr.empty() ? L"Failed to create temp file." : tmpErr;
        return false;
    }

    struct Writer : FPDF_FILEWRITE { HANDLE hFile; };
    auto writeBlock = [](FPDF_FILEWRITE* p, const void* data, unsigned long size) -> int {
        Writer* w = static_cast<Writer*>(p);
        DWORD written = 0;
        if (!WriteFile(w->hFile, data, size, &written, nullptr)) return 0;
        return written == size ? 1 : 0;
    };
    Writer writer{};
    writer.version = 1;
    writer.WriteBlock = writeBlock;
    writer.hFile = hFile;

    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        ok = !!FPDF_SaveAsCopy(doc, &writer, 0);
    }
    if (ok && !FlushFileBuffers(hFile)) {
        ok = false;
        if (err) {
            DWORD e = GetLastError();
            *err = L"Failed to flush temp output file (" + std::to_wstring(e) + L") " +
                   atomic_write::Win32ErrorMessage(e);
        }
    }
    CloseHandle(hFile);
    if (!ok) {
        if (err && err->empty()) *err = L"Failed to save PDF.";
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
        return false;
    }

    auto toExtendedPathIfAbsolute = [](const std::wstring& path) -> std::wstring {
        if (path.rfind(L"\\\\?\\", 0) == 0) return path;
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
            return std::wstring(L"\\\\?\\UNC\\") + path.substr(2);
        }
        if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
            return std::wstring(L"\\\\?\\") + path;
        }
        return path;
    };
    auto readPdfBlock = [](void* param,
                           unsigned long position,
                           unsigned char* outBuffer,
                           unsigned long size) -> int {
        if (!param || !outBuffer || size == 0) return 0;
        HANDLE h = reinterpret_cast<HANDLE>(param);
        if (!h || h == INVALID_HANDLE_VALUE) return 0;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(position);
        ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(position) >> 32);
        DWORD read = 0;
        if (ReadFile(h, outBuffer, size, &read, &ov)) {
            return (read == size) ? 1 : 0;
        }
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (GetOverlappedResult(h, &ov, &read, TRUE)) {
                return (read == size) ? 1 : 0;
            }
        }
        return 0;
    };
    auto validatePdfFile = [&](const std::filesystem::path& pathToValidate,
                               const wchar_t* label) -> bool {
        std::wstring openPath = toExtendedPathIfAbsolute(pathToValidate.wstring());
        HANDLE verifyFile = CreateFileW(openPath.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS |
                                            FILE_FLAG_OVERLAPPED,
                                        nullptr);
        if (verifyFile == INVALID_HANDLE_VALUE) {
            if (err) {
                DWORD e = GetLastError();
                *err = std::wstring(label) + L" の検証用オープンに失敗しました。(" +
                       std::to_wstring(e) + L") " + atomic_write::Win32ErrorMessage(e);
            }
            return false;
        }
        LARGE_INTEGER verifySize{};
        if (!GetFileSizeEx(verifyFile, &verifySize) || verifySize.QuadPart <= 0 ||
            verifySize.QuadPart >
                static_cast<LONGLONG>(std::numeric_limits<unsigned long>::max())) {
            if (err) *err = std::wstring(label) + L" の検証サイズが不正です。";
            CloseHandle(verifyFile);
            return false;
        }
        FPDF_FILEACCESS access{};
        access.m_FileLen = static_cast<unsigned long>(verifySize.QuadPart);
        access.m_GetBlock = readPdfBlock;
        access.m_Param = reinterpret_cast<void*>(verifyFile);
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        FPDF_DOCUMENT verifyDoc = FPDF_LoadCustomDocument(&access, nullptr);
        if (!verifyDoc) {
            if (err) *err = std::wstring(label) + L" のPDF再読み込み検証に失敗しました。";
            CloseHandle(verifyFile);
            return false;
        }
        const int expectedPageCount = FPDF_GetPageCount(doc);
        const int actualPageCount = FPDF_GetPageCount(verifyDoc);
        if (actualPageCount != expectedPageCount) {
            if (err) {
                *err = std::wstring(label) + L" のページ数検証に失敗しました。";
            }
            FPDF_CloseDocument(verifyDoc);
            CloseHandle(verifyFile);
            return false;
        }
        if (actualPageCount <= 0) {
            FPDF_CloseDocument(verifyDoc);
            CloseHandle(verifyFile);
            return true;
        }
        FPDF_PAGE verifyPage = FPDF_LoadPage(verifyDoc, 0);
        if (!verifyPage) {
            if (err) *err = std::wstring(label) + L" の先頭ページ検証に失敗しました。";
            FPDF_CloseDocument(verifyDoc);
            CloseHandle(verifyFile);
            return false;
        }
        FPDF_ClosePage(verifyPage);
        FPDF_CloseDocument(verifyDoc);
        CloseHandle(verifyFile);
        return true;
    };
    if (!validatePdfFile(tmp, L"一時出力")) {
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
        return false;
    }

    std::wstring repErr;
    if (!atomic_write::AtomicReplaceFile(dest, tmp, /*quarantineDir=*/dest.parent_path(), &repErr)) {
        if (err) *err = repErr.empty() ? L"Failed to replace output file." : repErr;
        return false;
    }
    return true;
}

struct ImagePdfSourcePixels {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    int stride = 0;
    double dpiX = kDpi;
    double dpiY = kDpi;
};

static bool LoadImageForPdfConversion(const std::wstring& sourcePath,
                                      ImagePdfSourcePixels* out,
                                      std::wstring* err) {
    if (!out || sourcePath.empty()) return false;
    *out = {};
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    const auto cleanup = [&]() {
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
    };
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (err) *err = L"WIC factory creation failed.";
        return false;
    }
    hr = factory->CreateDecoderFromFilename(sourcePath.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        if (err) *err = L"Image decoder creation failed.";
        cleanup();
        return false;
    }
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        if (err) *err = L"Image frame loading failed.";
        cleanup();
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > static_cast<UINT>(std::numeric_limits<int>::max() / 4) ||
        height > static_cast<UINT>(std::numeric_limits<int>::max())) {
        if (err) *err = L"Image dimensions are not supported.";
        cleanup();
        return false;
    }
    const size_t stride = static_cast<size_t>(width) * 4;
    if (height > std::numeric_limits<size_t>::max() / stride ||
        stride > static_cast<size_t>(std::numeric_limits<UINT>::max()) ||
        stride * height > static_cast<size_t>(std::numeric_limits<UINT>::max())) {
        if (err) *err = L"Image is too large to convert safely.";
        cleanup();
        return false;
    }
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        if (err) *err = L"Image pixel conversion failed.";
        cleanup();
        return false;
    }
    out->bgra.assign(stride * height, 0);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                               static_cast<UINT>(stride * height), out->bgra.data());
    if (FAILED(hr)) {
        if (err) *err = L"Image pixel read failed.";
        cleanup();
        return false;
    }
    double dpiX = kDpi;
    double dpiY = kDpi;
    if (FAILED(frame->GetResolution(&dpiX, &dpiY)) || !std::isfinite(dpiX) || !std::isfinite(dpiY) ||
        dpiX <= 0.0 || dpiY <= 0.0) {
        dpiX = kDpi;
        dpiY = kDpi;
    }
    cleanup();

    // The image viewer composites transparent pixels against white. Do the same for the PDF,
    // so conversion preserves its visible pixels instead of introducing a black background.
    for (size_t i = 0; i + 3 < out->bgra.size(); i += 4) {
        const int alpha = out->bgra[i + 3];
        if (alpha == 255) continue;
        const int inverse = 255 - alpha;
        out->bgra[i] = static_cast<uint8_t>((out->bgra[i] * alpha + 255 * inverse + 127) / 255);
        out->bgra[i + 1] = static_cast<uint8_t>((out->bgra[i + 1] * alpha + 255 * inverse + 127) / 255);
        out->bgra[i + 2] = static_cast<uint8_t>((out->bgra[i + 2] * alpha + 255 * inverse + 127) / 255);
        out->bgra[i + 3] = 255;
    }
    out->width = static_cast<int>(width);
    out->height = static_cast<int>(height);
    out->stride = static_cast<int>(stride);
    out->dpiX = dpiX;
    out->dpiY = dpiY;
    return true;
}

static bool ConvertImageToPdfFile(const std::wstring& sourcePath,
                                  const std::wstring& destinationPath,
                                  std::wstring* err) {
    ImagePdfSourcePixels image;
    if (!LoadImageForPdfConversion(sourcePath, &image, err)) return false;
    const double widthPt = image.width * 72.0 / image.dpiX;
    const double heightPt = image.height * 72.0 / image.dpiY;
    if (!std::isfinite(widthPt) || !std::isfinite(heightPt) || widthPt <= 0.0 || heightPt <= 0.0 ||
        widthPt > 14400.0 || heightPt > 14400.0) {
        if (err) *err = IsEnglishUi() ? L"Image page size is not supported by PDF."
                                      : L"画像のページサイズはPDFで扱えません。";
        return false;
    }
    FPDF_DOCUMENT doc = nullptr;
    bool prepared = false;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        doc = FPDF_CreateNewDocument();
        if (doc) {
            FPDF_PAGE page = FPDFPage_New(doc, 0, widthPt, heightPt);
            FPDF_PAGEOBJECT object = page ? FPDFPageObj_NewImageObj(doc) : nullptr;
            FPDF_BITMAP bitmap = object
                ? FPDFBitmap_CreateEx(image.width, image.height, FPDFBitmap_BGRA,
                                       image.bgra.data(), image.stride)
                : nullptr;
            if (page && object && bitmap &&
                FPDFImageObj_SetBitmap(&page, 1, object, bitmap) &&
                FPDFImageObj_SetMatrix(object, widthPt, 0.0, 0.0, -heightPt, 0.0, heightPt)) {
                FPDFPage_InsertObject(page, object);
                object = nullptr; // FPDFPage_InsertObject transfers ownership to PDFium.
                prepared = !!FPDFPage_GenerateContent(page);
            }
            if (bitmap) FPDFBitmap_Destroy(bitmap);
            if (object) FPDFPageObj_Destroy(object);
            if (page) FPDF_ClosePage(page);
        }
    }
    if (!prepared) {
        if (doc) {
            std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
            FPDF_CloseDocument(doc);
        }
        if (err) *err = IsEnglishUi() ? L"Failed to create the image PDF."
                                      : L"画像PDFを作成できませんでした。";
        return false;
    }
    const bool saved = SavePdfDocument(doc, destinationPath, err);
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        FPDF_CloseDocument(doc);
    }
    return saved;
}

static bool ExportPdfDocumentWithSpecs(FPDF_DOCUMENT srcDoc,
                                       const std::vector<Annotation>& annots,
                                       const std::vector<file_output::PdfPageSpec>& pages,
                                       const std::wstring& outPath,
                                       bool standardTextAnnots,
                                       double exportScale,
                                       bool matchPdfPaneTextLayout,
                                       std::wstring* err) {
    if (!srcDoc) return false;
    if (!IsValidPdfExportScale(exportScale)) {
        if (err) *err = L"Invalid export scale.";
        return false;
    }
    FPDF_DOCUMENT dest = nullptr;
    PdfTextFontCache fontCache;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        dest = FPDF_CreateNewDocument();
        if (!dest) {
            if (err) *err = L"Failed to create PDF.";
            return false;
        }
        int destIndex = 0;
        int pageCount = FPDF_GetPageCount(srcDoc);
        for (const auto& spec : pages) {
            if (spec.pageIndex < 0 || spec.pageIndex >= pageCount) {
                if (err) *err = L"Page index out of range.";
                ClosePdfTextFonts(fontCache);
                FPDF_CloseDocument(dest);
                return false;
            }
            double sourcePageWPt = 0.0;
            double sourcePageHPt = 0.0;
            if (spec.cropPt) {
                if (!FPDF_GetPageSizeByIndex(srcDoc, spec.pageIndex, &sourcePageWPt, &sourcePageHPt) ||
                    !IsValidPdfCropRectPt(*spec.cropPt, sourcePageWPt, sourcePageHPt)) {
                    if (err) *err = L"Slide crop rectangle is outside the source page.";
                    ClosePdfTextFonts(fontCache);
                    FPDF_CloseDocument(dest);
                    return false;
                }
            }
            std::string range = std::to_string(spec.pageIndex + 1);
            if (!FPDF_ImportPages(dest, srcDoc, range.c_str(), destIndex)) {
                if (err) *err = L"Failed to import page.";
                ClosePdfTextFonts(fontCache);
                FPDF_CloseDocument(dest);
                return false;
            }
            FPDF_PAGE page = FPDF_LoadPage(dest, destIndex);
            if (page) {
                bool dirty = false;
                if (spec.cropPt) {
                    ApplyPdfPageCropBoxes(page, *spec.cropPt);
                    dirty = true;
                }
                if (exportScale != 1.0) {
                    if (!ScalePdfPageInPlace(page, exportScale)) {
                        if (err) *err = L"Failed to scale PDF page.";
                        ClosePdfTextFonts(fontCache);
                        FPDF_ClosePage(page);
                        FPDF_CloseDocument(dest);
                        return false;
                    }
                    dirty = true;
                }
                if (spec.withAnnotations) {
                    AddAnnotationsToPage(annots, fontCache, dest, page, spec.pageIndex,
                                         standardTextAnnots, matchPdfPaneTextLayout,
                                         exportScale, /*bitmapDpiScale=*/1.0);
                    dirty = true;
                }
                if (dirty) {
                    FPDFPage_GenerateContent(page);
                }
                FPDF_ClosePage(page);
            }
            ++destIndex;
        }
    }
    bool ok = SavePdfDocument(dest, outPath, err);
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        ClosePdfTextFonts(fontCache);
        FPDF_CloseDocument(dest);
    }
    return ok;
}

} // namespace

namespace file_output {

bool ConvertImageToPdf(HWND owner, const std::wstring& imagePath, std::wstring* outPath) {
    if (outPath) outPath->clear();
    const std::filesystem::path source(imagePath);
    if (!IsImageFile(source)) return false;
    const std::wstring defaultName = DefaultNameFromPath(imagePath, L".pdf", L"image.pdf");
    const std::wstring title = IsEnglishUi() ? L"Convert Image to PDF" : L"画像をPDFに変換";
    COMDLG_FILTERSPEC filters[] = {
        { L"PDF (*.pdf)", L"*.pdf" },
        { L"All Files (*.*)", L"*.*" },
    };
    auto target = PickSavePath(owner, title.c_str(), defaultName, InitialDirForPath(imagePath),
                               filters, static_cast<UINT>(std::size(filters)), L"pdf");
    if (!target) return false;
    std::wstring err;
    if (!ConvertImageToPdfFile(imagePath, *target, &err)) {
        ShowFileOutputMessageDialog(owner, title,
                                    err.empty() ? (IsEnglishUi() ? L"Image-to-PDF conversion failed."
                                                                 : L"画像からPDFへの変換に失敗しました。")
                                                : err,
                                    SoftNoticeKind::Error);
        return false;
    }
    if (outPath) *outPath = *target;
    ShowFileOutputSoftNotice(owner,
                             IsEnglishUi() ? L"Image PDF created without modifying the source image."
                                           : L"元画像を変更せずにPDFを作成しました。",
                             SoftNoticeKind::Info);
    return true;
}

static bool ResolvePdfPageSize(int pageIndex, double* outWPt, double* outHPt) {
    if (outWPt) *outWPt = 0.0;
    if (outHPt) *outHPt = 0.0;
    FPDF_DOCUMENT doc = CurrentLogicalPdfDocument();
    if (!doc || pageIndex < 0) return false;
    if (pageIndex < static_cast<int>(g_pdf.pages.size())) {
        const auto& page = g_pdf.pages[static_cast<size_t>(pageIndex)];
        if (page.widthPt > 0.0 && page.heightPt > 0.0) {
            if (outWPt) *outWPt = page.widthPt;
            if (outHPt) *outHPt = page.heightPt;
            return true;
        }
    }
    double wPt = 0.0;
    double hPt = 0.0;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    if (!FPDF_GetPageSizeByIndex(doc, pageIndex, &wPt, &hPt)) return false;
    if (outWPt) *outWPt = wPt;
    if (outHPt) *outHPt = hPt;
    return wPt > 0.0 && hPt > 0.0;
}

static bool LoadPageTextBoxesForPng(FPDF_DOCUMENT doc, int pageIndex,
                                    double widthPt, double heightPt,
                                    PageCache* out) {
    if (!doc || !out || pageIndex < 0) return false;
    out->index = pageIndex;
    out->widthPt = widthPt;
    out->heightPt = heightPt;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        FPDF_ClosePage(page);
        return false;
    }
    const int count = FPDFText_CountChars(textPage);
    out->charBoxes.reserve(static_cast<size_t>(std::max(0, count)));
    for (int i = 0; i < count; ++i) {
        double left = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        double top = 0.0;
        if (!FPDFText_GetCharBox(textPage, i, &left, &right, &bottom, &top)) continue;
        PageCache::CharBox box{};
        box.left = static_cast<float>(left);
        box.right = static_cast<float>(right);
        box.bottom = static_cast<float>(bottom);
        box.top = static_cast<float>(top);
        out->charBoxes.push_back(box);
    }
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return !out->charBoxes.empty();
}

static std::vector<PdfPageSpec> BuildAllPdfPageSpecs(bool includeAnnotations) {
    std::vector<PdfPageSpec> pages;
    FPDF_DOCUMENT doc = CurrentLogicalPdfDocument();
    if (!doc) return pages;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    const int count = FPDF_GetPageCount(doc);
    pages.reserve(static_cast<size_t>(std::max(0, count)));
    for (int i = 0; i < count; ++i) {
        pages.push_back(PdfPageSpec{i, includeAnnotations});
    }
    return pages;
}

static bool ExportPdfPagesImpl(HWND owner,
                               const std::vector<PdfPageSpec>& pages,
                               const std::wstring& outPath,
                               bool standardTextAnnots,
                               double exportScale,
                               bool matchPdfPaneTextLayout) {
    FPDF_DOCUMENT doc = CurrentLogicalPdfDocument();
    const auto* annots = CurrentLogicalPdfAnnotations();
    if (!doc || CurrentLogicalPdfPath().empty()) return false;
    if (pages.empty()) return false;
    std::vector<Annotation> finalAnnots;
    if (annots) {
        finalAnnots = *annots;
        const bool containsTextColor = std::any_of(
            pages.begin(), pages.end(), [&](const PdfPageSpec& spec) {
                return spec.withAnnotations &&
                    std::any_of(finalAnnots.begin(), finalAnnots.end(), [&](const Annotation& ann) {
                        return ann.pageIndex == spec.pageIndex && ann.type == Annotation::Type::TextColor;
                    });
            });
        if (containsTextColor) {
            const std::wstring msg = IsEnglishUi()
                ? L"PDF export with text-color annotations is not supported yet. No file was written."
                : L"文字色変更注釈を含むPDF書き出しには未対応です。ファイルは書き込みませんでした。";
            ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
            return false;
        }
    }
    if (auto blockedMessage = GetProtectedPdfExportBlockMessage(doc)) {
        return ConfirmProtectedPdfExportAllowed(owner,
                                                ExperimentalExportDialogTitle(GetUiText().menuExportPdf),
                                                *blockedMessage,
                                                GetProtectedPdfExportBlockMessage);
    }
    if (IsSamePath(std::filesystem::path(outPath), std::filesystem::path(CurrentLogicalPdfPath()))) {
        WarnExportOverwriteOriginal(owner, /*isPdf=*/true);
        return false;
    }
    std::wstring err;
    if (!ExportPdfDocumentWithSpecs(doc,
                                    finalAnnots,
                                    pages,
                                    outPath,
                                    standardTextAnnots,
                                    exportScale,
                                    matchPdfPaneTextLayout,
                                    &err)) {
        if (!err.empty()) {
            ShowFileOutputMessageDialog(owner,
                                        ExperimentalExportDialogTitle(GetUiText().menuExportPdf),
                                        err,
                                        SoftNoticeKind::Error);
        }
        return false;
    }
    return true;
}

bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations) {
    return ExportPdfWithAnnotations(owner, includeAnnotations, g_config.exportStandardTextAnnots, 1.0);
}

bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations, bool standardTextAnnots) {
    return ExportPdfWithAnnotations(owner, includeAnnotations, standardTextAnnots, 1.0);
}

bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations, bool standardTextAnnots, double exportScale) {
    return ExportPdfWithAnnotations(owner, includeAnnotations, standardTextAnnots, exportScale,
                                    /*matchPdfPaneTextLayout=*/true);
}

bool ExportPdfWithAnnotations(HWND owner, bool includeAnnotations, bool standardTextAnnots, double exportScale,
                              bool matchPdfPaneTextLayout) {
    if (!CurrentLogicalPdfDocument() || CurrentLogicalPdfPath().empty()) return false;
    std::wstring defName = DefaultNameFromPath(CurrentLogicalPdfPath(), L"_annotated.pdf", L"document_annotated.pdf");
    std::wstring initialDir = InitialDirForPath(CurrentLogicalPdfPath());
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExportPdf);
    COMDLG_FILTERSPEC filters[] = {
        { L"PDF (*.pdf)", L"*.pdf" },
        { L"All Files (*.*)", L"*.*" }
    };
    // PickSavePath(owner, GetUiText().menuExportPdf.c_str(), ...) remains the explicit output-path gate.
    auto target = PickSavePath(owner, dialogTitle.c_str(),
                               defName, initialDir, filters, 2, L"pdf");
    if (!target) return false;
    return ExportPdfPagesImpl(owner, BuildAllPdfPageSpecs(includeAnnotations), *target,
                              standardTextAnnots, exportScale, matchPdfPaneTextLayout);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages) {
    return ExportPdfPages(owner, pages, g_config.exportStandardTextAnnots, 1.0);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, bool standardTextAnnots) {
    return ExportPdfPages(owner, pages, standardTextAnnots, 1.0);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath) {
    return ExportPdfPages(owner, pages, outPath, g_config.exportStandardTextAnnots, 1.0);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath,
                    bool standardTextAnnots) {
    return ExportPdfPages(owner, pages, outPath, standardTextAnnots, 1.0);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, bool standardTextAnnots, double exportScale) {
    return ExportPdfPages(owner, pages, standardTextAnnots, exportScale,
                          /*matchPdfPaneTextLayout=*/true);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, bool standardTextAnnots,
                    double exportScale, bool matchPdfPaneTextLayout) {
    if (!CurrentLogicalPdfDocument() || CurrentLogicalPdfPath().empty()) return false;
    std::wstring defName = DefaultNameFromPath(CurrentLogicalPdfPath(), L"_pages.pdf", L"document_pages.pdf");
    std::wstring initialDir = InitialDirForPath(CurrentLogicalPdfPath());
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExportPdfPages);
    COMDLG_FILTERSPEC filters[] = {
        { L"PDF (*.pdf)", L"*.pdf" },
        { L"All Files (*.*)", L"*.*" }
    };
    auto target = PickSavePath(owner, dialogTitle.c_str(),
                               defName, initialDir, filters, 2, L"pdf");
    if (!target) return false;
    return ExportPdfPages(owner, pages, *target, standardTextAnnots, exportScale, matchPdfPaneTextLayout);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath,
                    bool standardTextAnnots, double exportScale) {
    return ExportPdfPages(owner, pages, outPath, standardTextAnnots, exportScale,
                          /*matchPdfPaneTextLayout=*/true);
}

bool ExportPdfPages(HWND owner, const std::vector<PdfPageSpec>& pages, const std::wstring& outPath,
                    bool standardTextAnnots, double exportScale, bool matchPdfPaneTextLayout) {
    return ExportPdfPagesImpl(owner, pages, outPath, standardTextAnnots, exportScale,
                              matchPdfPaneTextLayout);
}

static bool TryParseFinitePdfNumber(const std::wstring& text, double* out) {
    if (out) *out = 0.0;
    std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.empty()) return false;
    wchar_t* end = nullptr;
    const double value = std::wcstod(trimmed.c_str(), &end);
    if (end == trimmed.c_str()) return false;
    while (end && *end != L'\0' && std::iswspace(*end)) ++end;
    if (end && *end != L'\0') return false;
    if (!std::isfinite(value)) return false;
    if (out) *out = value;
    return true;
}

static bool ExtractPdfCropRectFromPageToken(std::wstring* token,
                                            std::optional<PdfCropRectPt>* outCrop,
                                            std::wstring* err) {
    if (outCrop) outCrop->reset();
    if (!token) return false;
    const size_t open = token->find(L'[');
    if (open == std::wstring::npos) return true;
    const size_t close = token->rfind(L']');
    if (close == std::wstring::npos || close != token->size() - 1 || token->find(L'[', open + 1) != std::wstring::npos) {
        if (err) *err = L"切り出し矩形は page[left:bottom:right:top] の形式で指定してください。";
        return false;
    }
    const std::wstring cropText = token->substr(open + 1, close - open - 1);
    *token = TrimWhitespace(token->substr(0, open));

    std::vector<double> values;
    size_t pos = 0;
    while (pos <= cropText.size()) {
        const size_t next = cropText.find(L':', pos);
        const std::wstring part = TrimWhitespace(cropText.substr(pos, next == std::wstring::npos
                                                                    ? std::wstring::npos
                                                                    : next - pos));
        double value = 0.0;
        if (!TryParseFinitePdfNumber(part, &value)) {
            if (err) *err = L"切り出し矩形は数値の left:bottom:right:top で指定してください。";
            return false;
        }
        values.push_back(value);
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    if (values.size() != 4 || values[2] <= values[0] || values[3] <= values[1]) {
        if (err) *err = L"切り出し矩形は left < right かつ bottom < top にしてください。";
        return false;
    }
    if (outCrop) {
        PdfCropRectPt crop{};
        crop.left = values[0];
        crop.bottom = values[1];
        crop.right = values[2];
        crop.top = values[3];
        *outCrop = crop;
    }
    return true;
}

std::vector<PdfPageSpec> ParsePdfPageSpec(const std::wstring& spec, bool defaultAnnot, std::wstring* err) {
    if (err) err->clear();
    std::vector<PdfPageSpec> pages;
    std::wstring normalized = spec;
    std::replace(normalized.begin(), normalized.end(), L'/', L',');
    size_t pos = 0;
    while (pos < normalized.size()) {
        size_t next = normalized.find(L',', pos);
        std::wstring token = TrimWhitespace(normalized.substr(pos, next == std::wstring::npos
                                                                    ? std::wstring::npos
                                                                    : next - pos));
        pos = (next == std::wstring::npos) ? normalized.size() : (next + 1);
        if (token.empty()) continue;

        bool withAnnotations = defaultAnnot;
        wchar_t suffix = static_cast<wchar_t>(std::towlower(token.back()));
        if (suffix == L'a' || suffix == L'n') {
            withAnnotations = (suffix == L'a');
            token.pop_back();
            token = TrimWhitespace(token);
        }
        std::optional<PdfCropRectPt> cropPt;
        if (!ExtractPdfCropRectFromPageToken(&token, &cropPt, err)) {
            return {};
        }
        if (token.empty()) {
            if (err) *err = L"ページ指定が不正です。";
            return {};
        }

        auto appendPage = [&](int pageNo) {
            if (pageNo <= 0) return false;
            PdfPageSpec pageSpec{};
            pageSpec.pageIndex = pageNo - 1;
            pageSpec.withAnnotations = withAnnotations;
            pageSpec.cropPt = cropPt;
            pages.push_back(pageSpec);
            return true;
        };

        size_t dash = token.find(L'-');
        if (dash != std::wstring::npos) {
            const std::wstring firstText = TrimWhitespace(token.substr(0, dash));
            const std::wstring lastText = TrimWhitespace(token.substr(dash + 1));
            if (firstText.empty() || lastText.empty()) {
                if (err) *err = L"ページ範囲の指定が不正です。";
                return {};
            }
            int first = _wtoi(firstText.c_str());
            int last = _wtoi(lastText.c_str());
            if (first <= 0 || last <= 0 || first > last) {
                if (err) *err = L"ページ範囲の指定が不正です。";
                return {};
            }
            for (int page = first; page <= last; ++page) {
                if (!appendPage(page)) {
                    if (err) *err = L"ページ番号が不正です。";
                    return {};
                }
            }
            continue;
        }

        int pageNo = _wtoi(token.c_str());
        if (!appendPage(pageNo)) {
            if (err) *err = L"ページ番号が不正です。";
            return {};
        }
    }
    return pages;
}

bool ExportPdfPagePng(HWND owner, int pageIndex, PdfPngStyle style, bool includeAnnotations) {
    return ExportPdfPagePng(owner, pageIndex, style, includeAnnotations, 0, 0);
}

bool ExportPdfPagePng(HWND owner, int pageIndex, const std::wstring& outPath, PdfPngStyle style, bool includeAnnotations) {
    return ExportPdfPagePng(owner, pageIndex, outPath, style, includeAnnotations, 0, 0);
}

bool ExportPdfPagePng(HWND owner, int pageIndex, PdfPngStyle style, bool includeAnnotations,
                      int outWidthPx, int outHeightPx) {
    if (!CurrentLogicalPdfDocument() || CurrentLogicalPdfPath().empty()) return false;
    std::wstring pageSuffix = L"_page_" + std::to_wstring(pageIndex + 1) + L".png";
    std::wstring defName = DefaultNameFromPath(CurrentLogicalPdfPath(), pageSuffix, L"page.png");
    std::wstring initialDir = InitialDirForPath(CurrentLogicalPdfPath());
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExportPngPage);
    COMDLG_FILTERSPEC filters[] = {
        { L"PNG (*.png)", L"*.png" },
        { L"All Files (*.*)", L"*.*" }
    };
    auto target = PickSavePath(owner, dialogTitle.c_str(),
                               defName, initialDir, filters, 2, L"png");
    if (!target) return false;
    return ExportPdfPagePng(owner, pageIndex, *target, style, includeAnnotations, outWidthPx, outHeightPx);
}

bool ExportPdfPagePng(HWND owner, int pageIndex, const std::wstring& outPath, PdfPngStyle style,
                      bool includeAnnotations, int outWidthPx, int outHeightPx) {
    FPDF_DOCUMENT doc = CurrentLogicalPdfDocument();
    int pageCount = 0;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        pageCount = doc ? FPDF_GetPageCount(doc) : 0;
    }
    if (!doc || pageIndex < 0 || pageIndex >= pageCount) return false;
    if (auto blockedMessage = GetProtectedPdfPngExportBlockMessage(doc)) {
        return ConfirmProtectedPdfExportAllowed(owner,
                                                ExperimentalExportDialogTitle(GetUiText().menuExportPngPage),
                                                *blockedMessage,
                                                GetProtectedPdfPngExportBlockMessage);
    }
    double widthPt = 0.0;
    double heightPt = 0.0;
    if (!ResolvePdfPageSize(pageIndex, &widthPt, &heightPt)) return false;
    if (outWidthPx <= 0 || outHeightPx <= 0) {
        constexpr double kBaseDpi = 144.0;
        outWidthPx = std::max(1, static_cast<int>(std::lround(widthPt * kBaseDpi / 72.0)));
        outHeightPx = std::max(1, static_cast<int>(std::lround(heightPt * kBaseDpi / 72.0)));
    }
    const double dpi = (widthPt > 0.0) ? (static_cast<double>(outWidthPx) * 72.0 / widthPt) : 144.0;
    const auto* annots = CurrentLogicalPdfAnnotations() ? CurrentLogicalPdfAnnotations() : &g_annots;
    const bool hasTextColorAnnotation = includeAnnotations && std::any_of(
        annots->begin(), annots->end(), [pageIndex](const Annotation& ann) {
            return ann.pageIndex == pageIndex && ann.type == Annotation::Type::TextColor;
        });
    const PageCache* pageCache = (pageIndex >= 0 && pageIndex < static_cast<int>(g_pdf.pages.size()))
        ? &g_pdf.pages[static_cast<size_t>(pageIndex)]
        : nullptr;
    PageCache textPageCache;
    if (hasTextColorAnnotation && (!pageCache || pageCache->charBoxes.empty()) &&
        LoadPageTextBoxesForPng(doc, pageIndex, widthPt, heightPt, &textPageCache)) {
        pageCache = &textPageCache;
    }
    std::vector<uint8_t> pixels;
    int stride = 0;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
        if (!page) return false;
        FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(outWidthPx, outHeightPx, FPDFBitmap_BGRA, nullptr, 0);
        if (!bitmap) {
            FPDF_ClosePage(page);
            return false;
        }
        FPDFBitmap_FillRect(bitmap, 0, 0, outWidthPx, outHeightPx, 0xFFFFFFFFu);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, outWidthPx, outHeightPx, 0, FPDF_LCD_TEXT);

        auto* buf = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
        stride = FPDFBitmap_GetStride(bitmap);
        if (buf && stride > 0 && outHeightPx > 0) {
            pixels.assign(buf, buf + static_cast<size_t>(stride) * static_cast<size_t>(outHeightPx));
        }
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);
    }
    if (includeAnnotations && !pixels.empty()) {
        DrawAnnotationsToBuffer(*annots,
                                pageIndex, heightPt, dpi, pixels.data(), outWidthPx, outHeightPx, stride,
                                style == PdfPngStyle::ViewerLike ? AnnotationRenderMode::ViewerLike
                                                                 : AnnotationRenderMode::PdfLike,
                                pageCache);
    }

    std::wstring err;
    bool ok = !pixels.empty() && SavePngWic(outPath, pixels.data(), outWidthPx, outHeightPx, stride, dpi, &err);
    if (!ok && !err.empty()) {
        ShowFileOutputMessageDialog(owner,
                                    ExperimentalExportDialogTitle(GetUiText().menuExportPngPage),
                                    err,
                                    SoftNoticeKind::Error);
    }
    return ok;
}

bool ExportNotePlainText(HWND owner, const TextExportOptions& options) {
    if (g_currentNotePath.empty()) return false;
    std::wstring defName = DefaultNameFromPath(g_currentNotePath, L".txt", L"note.txt");
    std::wstring initialDir = InitialDirForPath(g_currentNotePath);
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExportNoteText);
    COMDLG_FILTERSPEC filters[] = {
        { L"Text (*.txt)", L"*.txt" },
        { L"All Files (*.*)", L"*.*" }
    };
    auto target = PickSavePath(owner, dialogTitle.c_str(),
                               defName, initialDir, filters, 2, L"txt");
    if (!target) return false;
    if (IsSamePath(std::filesystem::path(*target), std::filesystem::path(g_currentNotePath))) {
        WarnExportOverwriteOriginal(owner, /*isPdf=*/false);
        return false;
    }
    return ExportNotePlainText(g_currentNotePath, *target, options);
}

bool ExportNoteMarkup(HWND owner, const NoteMarkupExportOptions& options) {
    if (g_currentNotePath.empty()) return false;
    const bool html = options.format == NoteMarkupExportOptions::Format::Html;
    std::wstring defName = DefaultNameFromPath(g_currentNotePath, html ? L".html" : L".md",
                                               html ? L"note.html" : L"note.md");
    std::wstring initialDir = InitialDirForPath(g_currentNotePath);
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExportNoteMarkup);
    COMDLG_FILTERSPEC filters[] = {
        { html ? L"HTML (*.html)" : L"Markdown (*.md)", html ? L"*.html" : L"*.md" },
        { L"All Files (*.*)", L"*.*" }
    };
    auto target = PickSavePath(owner, dialogTitle.c_str(),
                               defName, initialDir, filters, 2, html ? L"html" : L"md");
    if (!target) return false;
    if (IsSamePath(std::filesystem::path(*target), std::filesystem::path(g_currentNotePath))) {
        WarnExportOverwriteOriginal(owner, /*isPdf=*/false);
        return false;
    }
    return ExportNoteMarkup(g_currentNotePath, *target, options);
}

bool ExportNoteHtml(HWND owner) {
    NoteMarkupExportOptions options{};
    return ExportNoteHtml(owner, options);
}

bool ExportNoteHtml(HWND owner, const NoteMarkupExportOptions& options) {
    if (g_currentNotePath.empty()) return false;
    std::wstring defName = DefaultNameFromPath(g_currentNotePath, L".html", L"note.html");
    std::wstring initialDir = InitialDirForPath(g_currentNotePath);
    const std::wstring dialogTitle = ExperimentalExportDialogTitle(GetUiText().menuExportNoteHtml);
    COMDLG_FILTERSPEC filters[] = {
        { L"HTML (*.html)", L"*.html" },
        { L"All Files (*.*)", L"*.*" }
    };
    auto target = PickSavePath(owner, dialogTitle.c_str(), defName, initialDir, filters, 2, L"html");
    if (!target) return false;
    if (IsSamePath(std::filesystem::path(*target), std::filesystem::path(g_currentNotePath))) {
        WarnExportOverwriteOriginal(owner, /*isPdf=*/false);
        return false;
    }
    return ExportNoteHtml(g_currentNotePath, *target, options);
}


bool ExportNotePlainText(const std::wstring& notePath, const std::wstring& outPath, const TextExportOptions& options) {
    if (!notePath.empty() && !outPath.empty() &&
        IsSamePath(std::filesystem::path(notePath), std::filesystem::path(outPath))) {
        return false;
    }
    auto source = ResolveNoteExportSource(notePath);
    if (!source) return false;
    if (!source->plain_text) {
        note::TextExportConfig config{};
        switch (options.mathMode) {
        case MathMode::Placeholder:
            config.mathMode = note::ExportTextMathMode::Placeholder;
            break;
        case MathMode::Simplified:
            config.mathMode = note::ExportTextMathMode::Simplified;
            break;
        case MathMode::Raw:
        default:
            config.mathMode = note::ExportTextMathMode::Raw;
            break;
        }
        switch (options.markupMode) {
        case MarkupMode::Raw:
            config.markupMode = note::ExportTextMarkupMode::Raw;
            break;
        case MarkupMode::Placeholder:
            config.markupMode = note::ExportTextMarkupMode::Placeholder;
            break;
        case MarkupMode::Simplified:
        default:
            config.markupMode = note::ExportTextMarkupMode::Simplified;
            break;
        }
        config.mathPlaceholder = options.mathPlaceholder;
        config.markupPlaceholder = options.markupPlaceholder;
        const note::WorkspaceNoteExportResult result =
            note::ExportWorkspacePlainText(*source->index, config);
        return result.ok && WriteFileUtf8(outPath, result.bytes);
    }
    (void)options;
    return WriteFileUtf8(outPath, source->snapshot.bytes);
}

bool ExportNoteHtml(const std::wstring& notePath, const std::wstring& outPath) {
    NoteMarkupExportOptions options{};
    return ExportNoteHtml(notePath, outPath, options);
}

bool ExportNoteHtml(const std::wstring& notePath, const std::wstring& outPath, const NoteMarkupExportOptions& options) {
    if (!notePath.empty() && !outPath.empty() &&
        IsSamePath(std::filesystem::path(notePath), std::filesystem::path(outPath))) {
        return false;
    }
    auto source = ResolveNoteExportSource(notePath);
    if (!source) return false;
    if (!source->plain_text) {
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.mathMode = (options.mathMode == NoteMarkupExportOptions::MathMode::Placeholder)
            ? note::ExportMarkupMathMode::Placeholder
            : note::ExportMarkupMathMode::Format;
        config.mathPlaceholder = options.mathPlaceholder;
        config.includeTitleHeading = options.includeTitleHeading;
        config.shiftHeadingLevels = options.shiftHeadingLevels;
        config.title = NoteTitleFromPath(notePath);
        const note::WorkspaceNoteExportResult result =
            note::ExportWorkspaceHtml(*source->index, config);
        return result.ok && WriteFileUtf8(outPath, result.bytes);
    }
    std::string out = BuildPlainTextHtmlDocument(
        source->raw, NoteTitleFromPath(notePath), options.includeTitleHeading);
    return WriteFileUtf8(outPath, out);
}

bool ExportNoteMarkup(const std::wstring& notePath, const std::wstring& outPath, const NoteMarkupExportOptions& options) {
    if (!notePath.empty() && !outPath.empty() &&
        IsSamePath(std::filesystem::path(notePath), std::filesystem::path(outPath))) {
        return false;
    }
    auto source = ResolveNoteExportSource(notePath);
    if (!source) return false;
    if (!source->plain_text) {
        note::MarkupExportConfig config{};
        config.format = (options.format == NoteMarkupExportOptions::Format::Html)
            ? note::ExportMarkupFormat::Html
            : note::ExportMarkupFormat::Markdown;
        config.mathMode = (options.mathMode == NoteMarkupExportOptions::MathMode::Placeholder)
            ? note::ExportMarkupMathMode::Placeholder
            : note::ExportMarkupMathMode::Format;
        config.mathPlaceholder = options.mathPlaceholder;
        config.includeTitleHeading = options.includeTitleHeading;
        config.shiftHeadingLevels = options.shiftHeadingLevels;
        config.title = NoteTitleFromPath(notePath);
        const note::WorkspaceNoteExportResult result =
            (config.format == note::ExportMarkupFormat::Html)
                ? note::ExportWorkspaceHtml(*source->index, config)
                : note::ExportWorkspaceMarkdown(*source->index, config);
        return result.ok && WriteFileUtf8(outPath, result.bytes);
    }
    if (options.format == NoteMarkupExportOptions::Format::Html) {
        std::string out = BuildPlainTextHtmlDocument(
            source->raw, NoteTitleFromPath(notePath), options.includeTitleHeading);
        return WriteFileUtf8(outPath, out);
    }

    return WriteFileUtf8(outPath, source->snapshot.bytes);
}

} // namespace file_output
