// Test-only host: use the real stage implementation, including its private
// journal writer and integration transaction, without adding production hooks.
// The runner replaces the ordinary stage object with this TU and starts a new
// process for each phase. All writes stay in an owned, disposable workspace.
#include <windows.h>
BOOL WINAPI StageTestWriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
// Instrument the inline stage write pipeline, not the observer's ledger writer.
// Distinct names prevent another app TU's uninstrumented COMDAT from winning.
#define WriteFile StageTestWriteFile
#define WriteAllBytesWin32 StageTestWriteAllBytesWin32
#define AtomicWriteBytes StageTestAtomicWriteBytes
#define AtomicWriteUtf8 StageTestAtomicWriteUtf8
#define ObservedWriteBytes StageTestObservedWriteBytes
#define ObservedWriteUtf8 StageTestObservedWriteUtf8
#include "../../src/file_output/file_output_stage.cpp"
#undef ObservedWriteUtf8
#undef ObservedWriteBytes
#undef AtomicWriteUtf8
#undef AtomicWriteBytes
#undef WriteAllBytesWin32
#undef WriteFile
#include <iostream>
#include <iomanip>
#include <map>
#include <chrono>
#include <stdexcept>
#include <sddl.h>
#include <aclapi.h>

namespace stage_test {
namespace fs = std::filesystem;
int checks = 0;
void Require(bool ok, const char* label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
    std::cout << "PASS: " << label << '\n';
}
Annotation Text(const std::wstring& text, bool cancellation = false) {
    Annotation a;
    a.type = Annotation::Type::TextBox;
    a.id = L"stage-fixture-text";
    a.pageIndex = 0;
    a.x1 = 443.998337641;
    a.x2 = a.x1 + 51.125;
    a.y1 = cancellation ? 1000.0 : 318.222620455;
    a.y2 = cancellation ? 1e-14 : a.y1 - 28.375;
    a.fontPt = 14.0023794622;
    a.fontName = L"Segoe UI";
    a.text = text;
    a.textLines = {text};
    return a;
}
AnnotCommand Update(const Annotation& before, const Annotation& after, bool legacy = false) {
    AnnotCommand command;
    command.kind = AnnotCommandKind::Update;
    command.beforeIndex = command.afterIndex = 0;
    command.before = before;
    command.after = after;
    command.numericPrecision = legacy ? AnnotNumericPrecision::Legacy6 : AnnotNumericPrecision::RoundTrip17;
    return command;
}
std::string Read(const fs::path& path) {
    std::string bytes;
    Require(ReadFileBytesWin32(path, bytes), "read owned fixture bytes");
    return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::wstring error;
    Require(atomic_write::StageTestAtomicWriteUtf8(path, bytes, path.parent_path(), &error), "write owned fixture bytes");
}
// Reproduce the old serializer's 12-significant-digit JSON numbers without
// touching quoted timestamps, IDs or text. Input is our valid generated JSON.
std::string LegacyCheckpoint(std::string_view input) {
    std::string result;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < input.size();) {
        const char c = input[i];
        if (quoted) {
            result += c; ++i;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') { quoted = true; result += c; ++i; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t end = i + 1;
            while (end < input.size() && std::string_view("0123456789.eE+-").find(input[end]) != std::string_view::npos) ++end;
            std::ostringstream number;
            number.imbue(std::locale::classic());
            number << std::setprecision(12) << std::stod(std::string(input.substr(i, end-i)));
            result += number.str(); i = end;
        } else { result += c; ++i; }
    }
    return result;
}
StageMeta Latest() {
    auto meta = FindLatestStageMeta(file_output::StagedDiffKind::Clrop, g_pdf.path);
    Require(meta.has_value() && meta->revision.has_value(), "find checkpoint and revision in fresh process");
    return *meta;
}
void Seed(bool legacy, bool cancellation) {
    std::wstring error;
    Require(clrop_bridge::SaveAnnotations(clrop_bridge::ClropPathForPdf(g_pdf.path),
        g_pdf.path, {Text(L"original")}, error), "create original CLROP fixture");
    const auto originalBytes = Read(clrop_bridge::ClropPathForPdf(g_pdf.path));
    const auto checkpoint = Text(L"checkpoint", cancellation);
    g_annots = {checkpoint}; g_annotsDirty = true;
    Require(file_output::PreserveUnsavedChangesForSystemEndSession(nullptr), "production emergency stage save succeeds");
    Require(!g_annotsDirty && g_annotsNeedsIntegrate, "stage save clears dirty and retains integration requirement");
    auto meta = Latest();
    if (legacy) {
        Write(meta.stagePath, LegacyCheckpoint(Read(meta.stagePath)));
        auto bytes = Read(meta.metaPath);
        const std::string precision = "annotation_numeric_precision=17\n";
        const auto pos = bytes.find(precision);
        Require(pos != std::string::npos, "new checkpoint declares precision");
        bytes.erase(pos, precision.size()); Write(meta.metaPath, bytes);
    }
    const auto intermediate = Text(L"journal-one", cancellation);
    const auto final = Text(L"journal-final", cancellation);
    Require(WriteAnnotJournalSegment(g_pdf.path, *meta.revision, {Update(checkpoint, intermediate, legacy)}, &error), "production journal writer saves first update");
    Require(WriteAnnotJournalSegment(g_pdf.path, *meta.revision, {Update(intermediate, final, legacy)}, &error), "production journal writer saves second update");
    if (legacy) {
        for (const auto& segment : ListAnnotJournalSegmentsForTarget(g_pdf.path)) {
            auto bytes = Read(segment.path);
            const std::string precision = ",\"numeric_precision\":6";
            const auto pos = bytes.find(precision);
            Require(pos != std::string::npos, "legacy journal fixture precision marker found");
            bytes.erase(pos, precision.size()); Write(segment.path, bytes);
        }
    }
    Require(Read(clrop_bridge::ClropPathForPdf(g_pdf.path)) == originalBytes,
        "checkpoint and journal writes leave original CLROP bytes unchanged");
}
void BackgroundAssistRoundTrip() {
    const std::array<TextBackgroundAssistMode, 4> modes{
        TextBackgroundAssistMode::Inherit, TextBackgroundAssistMode::Off,
        TextBackgroundAssistMode::Auto, TextBackgroundAssistMode::Inverted};
    std::vector<Annotation> expected;
    for (const auto type : {Annotation::Type::TextBox, Annotation::Type::MathBox}) {
        const auto original = Text(L"assist-mode");
        for (size_t index = 0; index < modes.size(); ++index) {
            Annotation changed = original;
            changed.type = type;
            changed.id = (type == Annotation::Type::TextBox ? L"text-" : L"math-") +
                std::to_wstring(index);
            changed.backgroundAssistMode = modes[index];
            if (index != 0) {
                Annotation before = changed;
                before.backgroundAssistMode = TextBackgroundAssistMode::Inherit;
                Require(!AnnotationCompareEqual(before, changed),
                    "stage comparison detects a background-assist-only change");
            }
            expected.push_back(std::move(changed));
        }
    }
    const fs::path savedPath = fs::path(g_workspaceRoot) / L"background-assist.clrop";
    std::wstring error;
    Require(clrop_bridge::SaveAnnotations(savedPath.wstring(), g_pdf.path, expected, error),
        "save all text and math background-assist modes");
    const std::string savedBytes = Read(savedPath);
    for (const char* mode : {"inherit", "off", "auto", "inverted"}) {
        Require(savedBytes.find(std::string("\"background_assist_mode\":\"") + mode + "\"") !=
                std::string::npos, "saved CLROP contains each background-assist mode");
    }
    std::vector<Annotation> loaded;
    bool mismatch = false;
    Require(clrop_bridge::LoadAnnotations(savedPath.wstring(), g_pdf.path, loaded, mismatch,
        nullptr, error) && !mismatch, "reload saved background-assist annotations");
    Require(loaded.size() == expected.size(), "reload keeps all text and math items");
    for (size_t index = 0; index < expected.size(); ++index) {
        Require(loaded[index].type == expected[index].type &&
                loaded[index].backgroundAssistMode == expected[index].backgroundAssistMode,
                "reload preserves the annotation kind and background-assist mode");
    }
    const std::string inheritField = ",\"background_assist_mode\":\"inherit\"";
    const size_t inheritPosition = savedBytes.find(inheritField);
    Require(inheritPosition != std::string::npos, "owned CLROP has a legacy field-removal target");
    std::string legacyBytes = savedBytes;
    legacyBytes.erase(inheritPosition, inheritField.size());
    const fs::path legacyPath = fs::path(g_workspaceRoot) / L"background-assist-legacy.clrop";
    Write(legacyPath, legacyBytes);
    loaded.clear(); error.clear(); mismatch = false;
    Require(clrop_bridge::LoadAnnotations(legacyPath.wstring(), g_pdf.path, loaded, mismatch,
        nullptr, error) && !mismatch && loaded.size() == expected.size() &&
        loaded[0].backgroundAssistMode == TextBackgroundAssistMode::Inherit,
        "legacy missing field retains inherited background assist");
    std::string unsupportedBytes = savedBytes;
    unsupportedBytes.replace(inheritPosition, inheritField.size(),
        ",\"background_assist_mode\":\"future\"");
    const fs::path unsupportedPath = fs::path(g_workspaceRoot) / L"background-assist-unsupported.clrop";
    Write(unsupportedPath, unsupportedBytes);
    loaded = {Text(L"caller-sentinel")}; error.clear(); mismatch = false;
    Require(!clrop_bridge::LoadAnnotations(unsupportedPath.wstring(), g_pdf.path, loaded,
        mismatch, nullptr, error) && loaded.empty() && !error.empty(),
        "unsupported mode rejects the load without publishing partial annotations");
    Require(Read(savedPath) == savedBytes, "negative cases leave the saved CLROP unchanged");
}
void Load(bool legacy, bool cancellation, bool reject) {
    const auto meta = Latest();
    const auto sentinel = Text(L"caller-sentinel");
    std::vector<Annotation> result{sentinel};
    std::wstring error;
    const bool loaded = file_output::LoadResolvedStagedAnnotations(g_pdf.path, meta.stagePath, &result, &error);
    if (reject) {
        Require(!loaded && !error.empty(), "bad input is rejected with an explicit error");
        Require(result.size() == 1 && AnnotationEquals(result[0], sentinel), "failed resolution leaves caller state unchanged");
    } else {
        if (!loaded) {
            std::wcerr << L"Resolution error: " << error << L'\n';
            Require(result.size() == 1 && AnnotationEquals(result[0], sentinel),
                "unexpected recovery failure also leaves caller state unchanged");
        }
        Require(loaded, "valid checkpoint and journals resolve in a new process");
        const auto expected = Text(L"journal-final", cancellation);
        Require(result.size() == 1 && annot_stage_replay::AnnotationMatches(result[0], expected,
            legacy ? AnnotNumericPrecision::Legacy6 : AnnotNumericPrecision::RoundTrip17,
            legacy ? AnnotNumericPrecision::Legacy6 : AnnotNumericPrecision::RoundTrip17), "resolved annotation matches the final edit");
    }
}
class LockedFixture {
    fs::path path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool created_;
public:
    LockedFixture(fs::path path, bool create) : path_(std::move(path)), created_(create) {
        handle_ = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path_).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(handle_ != INVALID_HANDLE_VALUE, "lock only the owned fixture target");
    }
    ~LockedFixture() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (created_) DeleteFileW(ToExtendedWin32PathIfAbsoluteLocal(path_).c_str());
    }
};
class DenyNewFixtureFiles {
    std::wstring path_;
    std::vector<unsigned char> original_;
    bool applied_ = false;
    bool RestoreAcl() noexcept {
        BOOL present = FALSE, defaulted = FALSE;
        PACL acl = nullptr;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorDacl(original_.data(), &present, &acl, &defaulted) ||
            !GetSecurityDescriptorControl(original_.data(), &control, &revision)) return false;
        const SECURITY_INFORMATION protection = (control & SE_DACL_PROTECTED)
            ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        return SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | protection, nullptr, nullptr, present ? acl : nullptr, nullptr) == ERROR_SUCCESS;
    }
public:
    explicit DenyNewFixtureFiles(const fs::path& path) : path_(ToExtendedWin32PathIfAbsoluteLocal(path)) {
        DWORD needed = 0;
        GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
        Require(needed != 0, "capture owned directory ACL size");
        original_.resize(needed);
        Require(GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION,
            original_.data(), needed, &needed), "capture owned directory ACL");
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(D;;0x00000006;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1,
            &descriptor, nullptr), "prepare owned directory create denial");
        applied_ = SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, descriptor) != FALSE;
        LocalFree(descriptor);
        Require(applied_, "deny creation only in owned stage directory");
    }
    void Restore() {
        if (!applied_) return;
        Require(RestoreAcl(), "restore owned directory ACL and inheritance");
        applied_ = false;
    }
    ~DenyNewFixtureFiles() {
        if (applied_) RestoreAcl();
    }
};
void SaveDenied(bool unsupported) {
    const auto meta = Latest();
    g_annots = {Text(L"new unsaved edit")}; g_annotsDirty = true;
    if (unsupported) {
        Require(!file_output::PreserveUnsavedChangesForSystemEndSession(nullptr), "unsupported precision blocks stage save");
    } else {
        DenyNewFixtureFiles denial(meta.stagePath.parent_path());
        const bool saved = file_output::PreserveUnsavedChangesForSystemEndSession(nullptr);
        denial.Restore();
        Require(!saved, "directory create denial causes a checkpoint save failure");
    }
    Require(g_annotsDirty && g_annots[0].text == L"new unsaved edit", "failed save preserves dirty state and unsaved edit");
    Require(fs::exists(meta.stagePath) && fs::exists(meta.metaPath), "failed save retains previous checkpoint and metadata");
}
void JournalDenied() {
    const auto meta = Latest();
    DenyNewFixtureFiles denial(AnnotJournalDir());
    std::wstring error;
    const bool saved = WriteAnnotJournalSegment(g_pdf.path, *meta.revision,
        {Update(Text(L"journal-final"), Text(L"unsaved journal"))}, &error);
    denial.Restore();
    Require(!saved && !error.empty(), "journal directory create denial fails explicitly");
}
void Integrate(bool denied) {
    const auto meta = Latest();
    std::wstring error;
    if (denied) {
        LockedFixture lock(meta.destPath, false);
        Require(!IntegrateStageMetaToDestination(nullptr, meta, true, &error) && !error.empty(),
            "locked original blocks integration explicitly");
        Require(fs::exists(meta.stagePath) && fs::exists(meta.metaPath), "failed integration retains recovery checkpoint");
    } else {
        Require(file_output::IntegrateStagedDiff(nullptr, meta.stagePath), "public integration succeeds with backup enabled");
        std::vector<Annotation> result; bool mismatch = false;
        Require(clrop_bridge::LoadAnnotations(meta.destPath.wstring(), g_pdf.path, result, mismatch,
            nullptr, error) && !mismatch, "integrated CLROP reopens against the same PDF");
        Require(result.size() == 1 && result[0].text == L"journal-final", "integration preserves the final journal edit");
        Require(!fs::exists(meta.stagePath) && !fs::exists(meta.metaPath), "successful integration removes the consumed checkpoint");
        Require(ListAnnotJournalSegmentsForTarget(g_pdf.path).empty(), "successful integration removes the consumed journal segments");
    }
}
void IntegrateRejectedInput() {
    const auto meta = Latest();
    std::wstring error;
    Require(!IntegrateStageMetaToDestination(nullptr, meta, true, &error) && !error.empty(),
        "invalid recovery input blocks integration before original replacement");
}

StageMeta RetentionStage(bool noteStage) {
    if (!noteStage) return Latest();
    const auto path = fs::path(g_workspaceRoot) / L"retention-note.txt";
    const std::string original = "original note\n";
    Write(path, original);
    std::wstring error;
    Require(note::ConfigureRuntimeNoteIdentityStore(g_workspaceRoot, &error), "configure owned note persistence store");
    const auto identity = note::ResolveRuntimeNoteIdentityPath(path.wstring(), &error);
    Require(identity.note_id.valid() && error.empty(), "allocate durable identity for the owned note fixture");
    const auto fingerprint = note::FingerprintSnapshotBytes(original);
    const auto revision = note::ObserveRuntimeNoteDiskSnapshot(identity.note_id, fingerprint, &error);
    Require(revision > 0 && error.empty(), "record the original note persistence revision");
    // Supply a full checkpoint in the production format without requiring an
    // open editor. The real save adapters still resolve and commit this stage.
    StageMeta meta;
    meta.kind = file_output::StagedDiffKind::Note;
    meta.targetPath = path.wstring(); meta.destPath = path;
    meta.noteId = identity.note_id.value;
    meta.basePersistenceRevision = revision; meta.persistenceRevision = revision + 1;
    meta.revision = 1;
    meta.destinationExistedAtStage = true; meta.destinationFingerprintAtStage = fingerprint;
    meta.stagePath = BuildStagePath(meta.kind, meta.targetPath, *meta.revision);
    meta.metaPath = StageMetaPath(meta.stagePath);
    Write(meta.stagePath, "new note\n");
    Require(WriteStageMetaFile(meta, &error), "write the real note stage metadata and persistence revision");
    return meta;
}
using BackupSnapshot = std::map<fs::path, std::string>;
BackupSnapshot SeedBackupHistory(const StageMeta& meta) {
    BackupSnapshot snapshot;
    const auto directory = BackupDir(meta.kind);
    Require(EnsureDir(directory), "create owned backup fixture directory");
    for (size_t i = 0; i < kMaxBackupGenerationsPerFile; ++i) {
        const auto backup = directory / (L"history-" + std::to_wstring(i) + L".bak");
        std::string bytes = "historical note " + std::to_string(i) + "\n";
        if (meta.kind == file_output::StagedDiffKind::Clrop) {
            std::wstring error;
            Require(clrop_bridge::SerializeAnnotations(g_pdf.path,
                {Text(L"historical annotation " + std::to_wstring(i))}, bytes, error), "serialize historical CLROP fixture");
        }
        const auto metadata = StageMetaPath(backup);
        const std::string metadataBytes = "kind=" + WideToUTF8(StageKindName(meta.kind)) +
            "\ndest=" + WideToUTF8(meta.destPath.wstring()) + "\nbackup=" + WideToUTF8(backup.wstring()) + "\n";
        Write(backup, bytes); Write(metadata, metadataBytes);
        fs::last_write_time(metadata, fs::file_time_type::clock::now() -
            std::chrono::hours(static_cast<long long>(kMaxBackupGenerationsPerFile - i)));
        snapshot.emplace(backup, bytes); snapshot.emplace(metadata, metadataBytes);
    }
    return snapshot;
}
void RequireHistoryPreserved(const BackupSnapshot& snapshot) {
    for (const auto& entry : snapshot) {
        Require(fs::is_regular_file(entry.first) && Read(entry.first) == entry.second,
            "failed transaction preserves every historical backup and metadata byte");
    }
}
std::vector<BackupMetaInfo> BackupsFor(const StageMeta& meta) {
    std::vector<BackupMetaInfo> backups;
    for (const auto& entry : fs::directory_iterator(BackupDir(meta.kind))) {
        if (entry.path().extension() != L".txt") continue;
        BackupMetaInfo backup;
        if (LoadBackupMetaFile(entry.path(), &backup) && backup.kind == meta.kind &&
            IsSamePath(backup.destPath, meta.destPath)) backups.push_back(std::move(backup));
    }
    return backups;
}
void RequireTrimmed(const StageMeta& meta, const std::string& original) {
    const auto backups = BackupsFor(meta);
    Require(backups.size() == kMaxBackupGenerationsPerFile, "successful transaction trims to three backup generations");
    bool originalRetained = false;
    for (const auto& backup : backups) {
        const auto bytes = Read(backup.backupPath);
        if (bytes == original) originalRetained = true;
    }
    Require(originalRetained, "trimming retains this transaction's pre-replacement original");
}
void BackupRetention(bool worker, bool denied, bool noteStage) {
    const auto meta = RetentionStage(noteStage);
    const auto original = Read(meta.destPath);
    const auto history = SeedBackupHistory(meta);
    const auto checkpoint = Read(meta.stagePath), metadata = Read(meta.metaPath);
    auto integrate = [&]() {
        std::wstring error;
        const bool ok = worker ? WriteStageMetaToDestinationOnWorker(meta, fs::path(g_workspaceRoot), &error)
                               : IntegrateStageMetaToDestination(nullptr, meta, true, &error);
        if (denied) Require(!ok && !error.empty(), "replacement failure returns an explicit error after backup creation");
        else Require(ok, "real save transaction succeeds before backup trimming");
    };
    if (denied) {
        LockedFixture lock(meta.destPath, false);
        for (int attempt = 0; attempt < 2; ++attempt) {
            integrate(); RequireHistoryPreserved(history);
            Require(Read(meta.destPath) == original, "repeated failed saves leave original bytes unchanged");
            Require(Read(meta.stagePath) == checkpoint && Read(meta.metaPath) == metadata,
                "repeated failed saves retain checkpoint and metadata");
        }
        Require(BackupsFor(meta).size() == kMaxBackupGenerationsPerFile + 2,
            "failed saves retain new recovery copies without applying the generation cap");
    } else {
        integrate(); RequireTrimmed(meta, original);
        Require(Read(meta.destPath) != original, "successful save publishes the new contents");
        if (noteStage) {
            const auto record = note::FindRuntimeNotePersistenceRecord(note::NoteId{*meta.noteId});
            Require(record.has_value() && record->persistence_revision == *meta.persistenceRevision,
                "successful note transaction commits its persistence revision");
        }
    }
}
void BackupRetentionCommitDenied(bool worker) {
    const auto meta = RetentionStage(true);
    const auto original = Read(meta.destPath);
    const auto history = SeedBackupHistory(meta);
    const auto checkpoint = Read(meta.stagePath), metadata = Read(meta.metaPath);
    const auto store = note::RuntimeNoteIdentityStorePath();
    const auto storeBytes = Read(store);
    LockedFixture lock(store, false);
    std::wstring error;
    const bool ok = worker ? WriteStageMetaToDestinationOnWorker(meta, fs::path(g_workspaceRoot), &error)
                           : IntegrateStageMetaToDestination(nullptr, meta, true, &error);
    Require(!ok && !error.empty(), "note revision commit failure returns an explicit error");
    Require(Read(meta.destPath) == "new note\n", "fault occurs after destination write and verification");
    Require(Read(store) == storeBytes, "failed revision commit preserves the durable identity store");
    RequireHistoryPreserved(history);
    Require(Read(meta.stagePath) == checkpoint && Read(meta.metaPath) == metadata,
        "failed revision commit retains checkpoint and metadata");
    const auto backups = BackupsFor(meta);
    Require(backups.size() == kMaxBackupGenerationsPerFile + 1, "revision commit failure does not trim backups");
    bool originalRetained = false;
    for (const auto& backup : backups) if (Read(backup.backupPath) == original) originalRetained = true;
    Require(originalRetained, "revision commit failure retains the pre-replacement original backup");
}
void RestoreBackupRetention(bool denied) {
    const auto meta = Latest();
    const auto original = Read(meta.destPath);
    const auto history = SeedBackupHistory(meta);
    const auto selected = BackupDir(meta.kind) / L"history-0.bak";
    const auto selectedBytes = Read(selected);
    fs::path restored;
    // Keep expected error notices off-screen in this owned test-only process.
    struct QuietErrors {
        bool prior = g_systemEndSessionStageSave;
        QuietErrors() { g_systemEndSessionStageSave = true; }
        ~QuietErrors() { g_systemEndSessionStageSave = prior; }
    } quiet;
    if (denied) {
        LockedFixture lock(meta.destPath, false);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Require(!file_output::RestoreFromBackupMeta(nullptr, StageMetaPath(selected), &restored) && restored.empty(),
                "failed restore does not publish a restored destination");
            RequireHistoryPreserved(history);
            Require(Read(meta.destPath) == original, "failed restore preserves the current original");
        }
        Require(BackupsFor(meta).size() == kMaxBackupGenerationsPerFile + 2, "repeated failed restores do not trim backups");
    } else {
        Require(file_output::RestoreFromBackupMeta(nullptr, StageMetaPath(selected), &restored) &&
            IsSamePath(restored, meta.destPath), "oldest historical backup restores successfully");
        Require(Read(meta.destPath) == selectedBytes, "restore publishes the selected historical bytes");
        RequireTrimmed(meta, original);
    }
}
void CrashWrite() {
    const auto meta = Latest();
    auto after = Text(L"journal-final");
    after.text.assign(2 * 1024 * 1024, L'x'); after.textLines = {after.text};
    std::wstring error;
    // The test-only stage write pipeline pauses after a real WriteFile chunk.
    // The parent kills this owned child only after observing that boundary.
    Require(WriteAnnotJournalSegment(g_pdf.path, *meta.revision,
        {Update(Text(L"journal-final"), after)}, &error), "crash writer should never return before parent termination");
    throw std::runtime_error("crash injection did not stop at the write boundary");
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace stage_test;
    if (argc != 3) return 2;
    try {
        const fs::path root = fs::weakly_canonical(argv[2]);
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        Require(length > 0 && length < 32768, "locate the test executable");
        std::wstring prefix = fs::weakly_canonical(fs::path(executable).parent_path()).wstring() + L"\\";
        const auto candidate = root.wstring();
        Require(candidate.size() > prefix.size() && CompareStringOrdinal(candidate.c_str(),
            static_cast<int>(prefix.size()), prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL,
            "workspace stays below this test executable's owned output directory");
        Require(root.is_absolute() && fs::is_regular_file(root / L"stage-test-owned.txt"), "runner supplied an owned absolute workspace");
        g_workspaceRoot = root.wstring();
        g_config.autoSaveSeconds = 0;
        g_pdf.path = (root / L"sample.pdf").wstring();
        g_pdf.kind = DocKind::Pdf;
        FPDF_InitLibrary();
        struct Library { ~Library() { if (g_pdf.doc) FPDF_CloseDocument(g_pdf.doc); g_pdf.doc = nullptr; FPDF_DestroyLibrary(); } } library;
        g_pdf.doc = FPDF_LoadDocument(WideToUTF8(g_pdf.path).c_str(), nullptr);
        Require(g_pdf.doc != nullptr, "real PDFium opens the copied PDF fixture");
        g_pdf.pageCount = FPDF_GetPageCount(g_pdf.doc);
        const std::wstring mode = argv[1];
        if (mode == L"seed") Seed(false, false);
        else if (mode == L"background-assist") BackgroundAssistRoundTrip();
        else if (mode == L"seed-legacy") Seed(true, false);
        else if (mode == L"seed-cancellation") Seed(false, true);
        else if (mode == L"load") Load(false, false, false);
        else if (mode == L"load-legacy") Load(true, false, false);
        else if (mode == L"load-cancellation") Load(false, true, false);
        else if (mode == L"reject") Load(false, false, true);
        else if (mode == L"save-denied") SaveDenied(false);
        else if (mode == L"save-unsupported") SaveDenied(true);
        else if (mode == L"journal-denied") JournalDenied();
        else if (mode == L"integrate-denied") Integrate(true);
        else if (mode == L"integrate") Integrate(false);
        else if (mode == L"integrate-rejected-input") IntegrateRejectedInput();
        else if (mode == L"retention-sync-denied") BackupRetention(false, true, false);
        else if (mode == L"retention-sync-success") BackupRetention(false, false, false);
        else if (mode == L"retention-worker-denied") BackupRetention(true, true, false);
        else if (mode == L"retention-worker-success") BackupRetention(true, false, false);
        else if (mode == L"retention-note-sync-denied") BackupRetention(false, true, true);
        else if (mode == L"retention-note-sync-success") BackupRetention(false, false, true);
        else if (mode == L"retention-note-worker-denied") BackupRetention(true, true, true);
        else if (mode == L"retention-note-worker-success") BackupRetention(true, false, true);
        else if (mode == L"retention-note-sync-commit-denied") BackupRetentionCommitDenied(false);
        else if (mode == L"retention-note-worker-commit-denied") BackupRetentionCommitDenied(true);
        else if (mode == L"retention-restore-denied") RestoreBackupRetention(true);
        else if (mode == L"retention-restore-success") RestoreBackupRetention(false);
        else if (mode == L"crash-write") CrashWrite();
        else throw std::runtime_error("unknown test mode");
        std::cout << "checks=" << checks << " failures=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " checks=" << checks << '\n';
        return 1;
    }
}
