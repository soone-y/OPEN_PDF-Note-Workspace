#include "diagnostics/write_checks.h"
#include "diagnostics/normal_operations.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sddl.h>
using namespace write_checks;
namespace {
HANDLE persistEntered = nullptr, persistRelease = nullptr;
std::atomic_bool persistTimedOut{false};
void HoldLedgerWorker() {
    SetEvent(persistEntered);
    if (WaitForSingleObject(persistRelease, 5000) != WAIT_OBJECT_0) persistTimedOut = true;
}
struct ScopedWriteDenial {
    std::filesystem::path path;
    std::vector<BYTE> original;
    bool applied = false;
    void Deny(const std::filesystem::path& target) {
        path = target;
        DWORD bytes = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes);
        assert(bytes); original.resize(bytes);
        assert(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, original.data(), bytes, &bytes));
        PSECURITY_DESCRIPTOR denied = nullptr;
        assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x00000006;;;WD)(A;;FA;;;WD)",
            SDDL_REVISION_1, &denied, nullptr));
        applied = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, denied) != FALSE;
        LocalFree(denied); assert(applied);
    }
    void Restore() {
        if (applied) { assert(SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, original.data())); applied = false; }
    }
    ~ScopedWriteDenial() { Restore(); }
};
}
int wmain() {
    ManualLimit limit;
    assert(limit.Start(0)); assert(!limit.Start(1)); limit.Finish(2, false);
    for (unsigned i = 0; i < 4; ++i) { assert(limit.Start(10 + i)); limit.Finish(10 + i, true); assert(!limit.Remaining(10 + i)); }
    assert(limit.Start(20)); limit.Finish(20, false); // changed result does not consume the fifth count
    assert(limit.Start(30)); limit.Finish(30, true);
    assert(limit.Remaining(30) == 300000); assert(!limit.Start(31));
    assert(limit.RecoveryAvailable(31)); assert(limit.Start(31, true));
    limit.Finish(50, true); // recovery does not extend cooldown
    assert(limit.Remaining(50) == 299980); assert(limit.RemainingRetries(50) == 1);
    assert(limit.Start(51, true)); limit.Finish(52, false);
    assert(!limit.RecoveryAvailable(52)); assert(!limit.Start(53, true)); assert(!limit.Start(300029));
    assert(limit.Start(300030)); limit.Finish(300030, true); assert(!limit.Remaining(300030));
    ManualLimit restarted; assert(restarted.Start(51)); restarted.Finish(52, false);
    // An in-flight exception retry can complete after expiration without starting another cooldown.
    ManualLimit slow;
    for (unsigned i = 0; i < 5; ++i) { assert(slow.Start(i)); slow.Finish(i, true); }
    assert(slow.Start(5, true)); slow.Finish(400000, true); assert(slow.Start(400001)); slow.Finish(400002, true); assert(!slow.Remaining(400002));

    std::error_code ec;
    const auto root = std::filesystem::current_path(ec) / (L"write_checks_fixture_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    assert(!ec); assert(std::filesystem::create_directory(root, ec) && !ec);
    const auto write = ProbeDirectory(root, true);
    assert(write.outcome == Outcome::Passed && write.error == 0 && write.remaining.empty());
    assert(std::filesystem::is_empty(root, ec) && !ec);
    assert(ProbeDirectory(root, false).outcome == Outcome::Passed);
    const auto missing = ProbeDirectory(root / L"missing", true);
    assert(missing.outcome == Outcome::Unavailable && missing.step == Step::Open);
    assert(!std::filesystem::exists(root / L"missing", ec) && !ec);
    std::atomic_bool destinationCancel{false};
    const auto session = root / L"session";
    assert(std::filesystem::create_directory(session, ec) && !ec);
    for (const auto kind : {Kind::PdfWrite, Kind::NoteWrite}) {
        const auto target = session / (kind == Kind::PdfWrite ? L"pdf" : L"note");
        const auto parent = ProbeSessionDestination(kind, target, session, destinationCancel);
        assert(parent.outcome == Outcome::Passed && parent.operationPath == session.wstring() && parent.remaining.empty());
        Record row{kind, target.wstring(), parent};
        assert(IsParentDestinationProbe(row));
        row.result.source = Source::Normal; assert(!IsParentDestinationProbe(row));
        assert(!std::filesystem::exists(target, ec) && !ec && std::filesystem::is_empty(session, ec) && !ec);
        assert(std::filesystem::create_directory(target, ec) && !ec);
        const auto direct = ProbeSessionDestination(kind, target, session, destinationCancel);
        assert(direct.outcome == Outcome::Passed && direct.operationPath.empty());
        // Deny child creation explicitly. Metadata-only directory handles do
        // not prevent creating children on Windows.
        ScopedWriteDenial denial; denial.Deny(target);
        const auto denied = ProbeSessionDestination(kind, target, session, destinationCancel);
        assert(denied.outcome != Outcome::Passed && denied.operationPath.empty());
        denial.Restore();
        assert(RemoveDirectoryW(target.c_str()));
        { std::ofstream file(target); file << "not a directory"; }
        const auto file = ProbeSessionDestination(kind, target, session, destinationCancel);
        assert(file.outcome == Outcome::Unavailable && file.operationPath.empty());
        assert(DeleteFileW(target.c_str()));
    }
    const auto absentSession = root / L"absent_session";
    const auto absentParent = ProbeSessionDestination(Kind::PdfWrite, absentSession / L"pdf", absentSession, destinationCancel);
    assert(absentParent.outcome == Outcome::Unavailable && absentParent.operationPath.empty());
    const auto wrongLeaf = ProbeSessionDestination(Kind::PdfWrite, session / L"other", session, destinationCancel);
    assert(wrongLeaf.outcome == Outcome::Unavailable && wrongLeaf.operationPath.empty());
    const auto wrongKind = ProbeSessionDestination(Kind::TempWrite, session / L"pdf", session, destinationCancel);
    assert(wrongKind.outcome == Outcome::Unavailable && wrongKind.operationPath.empty());
    const auto flat = ProbeSessionDestination(Kind::PdfWrite, session, session, destinationCancel);
    assert(flat.outcome == Outcome::Passed && flat.operationPath.empty());
    destinationCancel.store(true);
    const auto canceledDestination = ProbeSessionDestination(Kind::PdfWrite, session / L"pdf", session, destinationCancel);
    assert(canceledDestination.outcome == Outcome::Canceled && canceledDestination.operationPath.empty());
    destinationCancel.store(false);
    // Parent evidence survives the existing v2 ledger without being promoted
    // to a direct destination success after reload.
    const auto parentStore = root / L"parent.log";
    std::vector<Record> parentRows{{Kind::PdfWrite, (session / L"pdf").wstring(),
        ProbeSessionDestination(Kind::PdfWrite, session / L"pdf", session, destinationCancel)}};
    std::string parentBaseline; DWORD parentError = 0;
    assert(Save(parentStore, parentRows, parentBaseline, parentError));
    parentRows.clear(); assert(Load(parentStore, parentRows, parentBaseline, parentError));
    assert(parentRows.size() == 1 && IsParentDestinationProbe(parentRows.front()));
    assert(DeleteFileW(parentStore.c_str()));
    assert(RemoveDirectoryW(session.c_str()));
    assert(!IsSafeLocalPath(L"C:relative")); assert(!IsSafeLocalPath(root / L".." / L"other"));
    const auto original = root / L"original.txt";
    { std::ofstream out(original); out << "original"; }
    assert(ProbeFile(original).outcome == Outcome::Passed);
    assert(ProbeDirectory(original, true).outcome == Outcome::Unavailable);
    assert(ProbeFile(root / L"missing.txt").outcome == Outcome::Unavailable);
    std::atomic_bool canceled{false};
    assert(ProbeNoteFile(original, canceled).outcome == Outcome::Passed);
    { std::ifstream in(original); std::string text; std::getline(in, text); assert(text == "original"); }
    assert(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY));
    const auto readonly = ProbeNoteFile(original, canceled);
    assert(readonly.outcome == Outcome::Failed && readonly.error == ERROR_ACCESS_DENIED);
    assert(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_NORMAL));
    canceled.store(true); assert(ProbeNoteFile(original, canceled).outcome == Outcome::Canceled);
    canceled.store(false); assert(ProbeNoteFile(root / L"missing.txt", canceled).outcome == Outcome::Unavailable);
    const auto largeNote = root / L"large.txt";
    const std::string largePayload(2 * 1024 * 1024, 'n');
    { std::ofstream out(largeNote, std::ios::binary); out << largePayload; }
    assert(ProbeFile(largeNote).outcome == Outcome::Unavailable); // startup/settings retain their 1 MiB limit
    assert(ProbeNoteFile(largeNote, canceled).outcome == Outcome::Passed);
    { std::ifstream in(largeNote, std::ios::binary); std::string actual((std::istreambuf_iterator<char>(in)), {}); assert(actual == largePayload); }
    assert(DeleteFileW(largeNote.c_str()));
    const auto store = root / L"results.log";
    DWORD error = 0; std::string baseline; std::vector<Record> records;
    assert(Load(store, records, baseline, error) && records.empty());
    Record first{Kind::WorkspaceWrite, root.wstring(), write};
    Upsert(records, first);
    assert(Save(store, records, baseline, error));
    std::vector<Record> restored; std::string persisted;
    assert(Load(store, restored, persisted, error) && restored.size() == 1);
    assert(restored[0].result.time == write.time && restored[0].result.outcome == Outcome::Passed);
    const auto compactSize = persisted.size();
    for (int i = 0; i < 20; ++i) {
        first.result.time = UtcNow(); Upsert(records, first); assert(Save(store, records, baseline, error));
    }
    assert(Load(store, restored, persisted, error) && restored.size() == 1 && persisted.size() == compactSize);
    records.push_back(first);
    assert(!Save(store, records, baseline, error) && error == ERROR_INVALID_DATA);
    records.pop_back();
    records[0].result.outcome = static_cast<Outcome>(99);
    assert(!Save(store, records, baseline, error) && error == ERROR_INVALID_DATA);
    records[0] = first;
    assert(Load(store, restored, persisted, error) && restored[0].result.outcome == Outcome::Passed);
    auto stale = persisted;
    first.result.outcome = Outcome::Failed; first.result.error = ERROR_ACCESS_DENIED;
    Upsert(records, first); assert(Save(store, records, baseline, error));
    assert(!Save(store, restored, stale, error) && error == ERROR_REVISION_MISMATCH);
    assert(Load(store, restored, persisted, error) && restored[0].result.outcome == Outcome::Failed);
    Record unicode{Kind::PdfRead, (root / L"文書 with spaces.pdf").wstring(), write};
    Upsert(records, unicode); assert(Save(store, records, baseline, error));
    assert(Load(store, restored, persisted, error) && Find(restored, Kind::PdfRead, unicode.target));
    assert(!Find(restored, Kind::PdfRead, (root / L"different.pdf").wstring()));
    const auto broken = root / L"broken.log";
    { std::ofstream out(broken); out << "broken record\n"; }
    std::string empty;
    assert(!Load(broken, restored, empty, error));
    assert(!Save(broken, records, empty, error));
    { std::ifstream in(broken); std::string text; std::getline(in, text); assert(text == "broken record"); }
    HANDLE locked = CreateFileW(store.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE); assert(!Save(store, records, baseline, error)); CloseHandle(locked);
    assert(Load(store, restored, persisted, error));
    // Retention is oldest-first, bounded, and only commits after storage succeeds.
    std::vector<Record> history;
    for (unsigned i = 0; i < 80; ++i) {
        Record row{Kind::PdfRead, (root / (L"history_" + std::to_wstring(i))).wstring(), write};
        row.result.time = write.time + i; history.push_back(row);
    }
    locked = CreateFileW(store.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE && !Save(store, history, baseline, error) && history.size() == 80); CloseHandle(locked);
    assert(Save(store, history, baseline, error) && history.size() == 64);
    assert(!Find(history, Kind::PdfRead, (root / L"history_0").wstring()));
    assert(Find(history, Kind::PdfRead, (root / L"history_79").wstring()));
    history.clear();
    for (unsigned i = 0; i < 20; ++i) {
        Record row{Kind::PdfRead, (root / (std::wstring(2000, L'文') + std::to_wstring(i))).wstring(), write};
        row.result.time = write.time + i; history.push_back(row);
    }
    assert(Save(store, history, baseline, error) && baseline.size() <= 64 * 1024 && history.size() < 20);
    assert(history.back().result.time == write.time + 19);
    history = {{Kind::PdfRead, (root / std::wstring(26000, L'文')).wstring(), write}};
    assert(Save(store, history, baseline, error) && history.size() == 1 && baseline.size() > 64 * 1024);
    assert(Load(store, restored, persisted, error) && restored.size() == 1); // newest oversized entry remains readable
    const auto legacyStore = root / L"legacy.log";
    { std::ofstream out(legacyStore, std::ios::binary); out << "PDF_NOTE_WRITE_CHECKS_1\n";
      for (unsigned i = 0; i < 256; ++i) out << "11 \"legacy_" << i << "\" 0 8 0 " << write.time + i << " \"\"\n"; }
    assert(Load(legacyStore, history, baseline, error) && history.size() == 256);
    Record newest{Kind::PdfRead, L"newest", write}; newest.result.time += 256; Upsert(history, newest);
    assert(Save(legacyStore, history, baseline, error) && history.size() == 64 && Find(history, Kind::PdfRead, L"newest"));
    assert(DeleteFileW(legacyStore.c_str()));
    // Real normal writes: provenance survives restart; a failed replacement
    // updates evidence without changing the original's bytes or attributes.
    const auto resource = root / L"__pdf_note_workspace__";
    const auto settings = resource / L"__settings__";
    const auto normalStore = resource / L"__log__" / L"write_checks.log";
    const auto config = settings / L"theme.json";
    std::wstring detail;
    EnableNormalObservations(false);
    assert(ObservedWriteUtf8(root, config, "first", settings, settings, &detail));
    assert(!std::filesystem::exists(normalStore, ec) && !ec); // No ledger IO before startup lock acquisition.
    bool unsaved = false; DWORD normalError = 0; std::vector<Record> live;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError) && unsaved);
    EnableNormalObservations();
    const auto workspaceFile = root / L"workspace.json";
    assert(ObservedWriteUtf8(root, workspaceFile, "config", root, root, &detail));
    assert(FlushNormalObservations(root));
    assert(Load(normalStore, restored, persisted, error));
    const auto* normal = Find(restored, Kind::SettingsWrite, settings.wstring());
    assert(normal && normal->result.source == Source::Normal && normal->result.operationPath == config.wstring());
    assert(normal->result.outcome == Outcome::Passed && normal->result.remaining.empty());
    assert(persisted.rfind("PDF_NOTE_WRITE_CHECKS_2\n", 0) == 0);
    assert(SetFileAttributesW(config.c_str(), FILE_ATTRIBUTE_READONLY));
    assert(!ObservedWriteUtf8(root, config, "damaged", settings, settings, &detail));
    assert(FlushNormalObservations(root));
    assert(Load(normalStore, restored, persisted, error));
    assert(Find(restored, Kind::SettingsWrite, settings.wstring())->result.outcome == Outcome::Failed);
    { std::ifstream in(config); std::string bytes; in >> bytes; assert(bytes == "first"); }
    assert(GetFileAttributesW(config.c_str()) & FILE_ATTRIBUTE_READONLY);
    assert(SetFileAttributesW(config.c_str(), FILE_ATTRIBUTE_NORMAL));
    assert(ObservedWriteUtf8(root, config, "second", settings, settings, &detail));
    assert(FlushNormalObservations(root));
    assert(Load(normalStore, restored, persisted, error));
    assert(Find(restored, Kind::SettingsWrite, settings.wstring())->result.outcome == Outcome::Passed);
    assert(Find(restored, Kind::WorkspaceWrite, root.wstring() + L"\\")); // Same Windows target.
    // The real ledger worker is blocked, yet operation completion and memory
    // display continue. Two captured workspaces retain separate evidence.
    persistEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    persistRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(persistEntered && persistRelease);
    SetBeforeNormalPersistForTesting(HoldLedgerWorker);
    assert(ObservedWriteUtf8(root, config, "async-first", settings, settings, &detail));
    assert(WaitForSingleObject(persistEntered, 5000) == WAIT_OBJECT_0);
    assert(ObservedWriteUtf8(root, config, "second", settings, settings, &detail));
    const auto otherRoot = root / L"other_workspace";
    const auto otherConfig = otherRoot / L"workspace.json";
    const auto otherStore = otherRoot / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log";
    assert(ObservedWriteUtf8(otherRoot, otherConfig, "other", otherRoot, {}, &detail));
    live.clear(); unsaved = false; normalError = 0;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError) && unsaved && !persistTimedOut);
    assert(Find(live, Kind::SettingsWrite, settings.wstring()));
    SetBeforeNormalPersistForTesting(nullptr);
    assert(SetEvent(persistRelease));
    assert(FlushNormalObservations(root) && FlushNormalObservations(otherRoot) && !persistTimedOut);
    assert(Load(otherStore, restored, persisted, error));
    assert(Find(restored, Kind::WorkspaceWrite, otherRoot.wstring()));
    CloseHandle(persistEntered); CloseHandle(persistRelease);
    assert(DeleteFileW(otherConfig.c_str())); assert(DeleteFileW(otherStore.c_str()));
    assert(RemoveDirectoryW(otherStore.parent_path().c_str()));
    assert(RemoveDirectoryW(otherStore.parent_path().parent_path().c_str())); assert(RemoveDirectoryW(otherRoot.c_str()));
    assert(Load(normalStore, restored, persisted, error));
    // Manual demo internals and canceled/no-op operations do not manufacture
    // normal evidence, or change the manual limit. Preserve Win32 error state.
    const auto before = persisted;
    { IgnoreNormalOperations ignore; assert(ObservedWriteUtf8(root, config, "demo", settings, settings, &detail)); }
    SetLastError(ERROR_LOCK_VIOLATION);
    { NormalOperation canceled(root, config); canceled.Cancel(); }
    assert(GetLastError() == ERROR_LOCK_VIOLATION);
    assert(Load(normalStore, restored, persisted, error) && persisted == before);
    // Actual reads update memory on every operation; identical reads within
    // ten seconds do not rewrite the ledger. Explicit flush persists the latest.
    const auto observeRead = [&] {
        NormalOperation observation(root, original, Kind::SetupRead, original);
        const auto result = ProbeFile(original);
        if (result.outcome == Outcome::Passed) observation.Success(); else observation.Failure(result.error);
    };
    observeRead();
    assert(FlushNormalObservations(root));
    assert(Load(normalStore, restored, persisted, error));
    const auto firstRead = persisted;
    Sleep(2);
    for (int i = 0; i < 20; ++i) observeRead();
    assert(Load(normalStore, restored, persisted, error) && persisted == firstRead);
    live.clear(); unsaved = false; normalError = 0;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError) && unsaved && !normalError);
    assert(Find(live, Kind::SetupRead, original.wstring())->result.time > Find(restored, Kind::SetupRead, original.wstring())->result.time);
    assert(FlushNormalObservations(root));
    assert(Load(normalStore, restored, persisted, error) && persisted != firstRead);
    assert(Find(restored, Kind::SetupRead, original.wstring())->result.source == Source::Normal);
    locked = CreateFileW(original.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE); observeRead(); assert(FlushNormalObservations(root)); CloseHandle(locked);
    assert(Load(normalStore, restored, persisted, error));
    const auto* readFailure = Find(restored, Kind::SetupRead, original.wstring());
    assert(readFailure && readFailure->result.outcome == Outcome::Failed && readFailure->result.error == ERROR_SHARING_VIOLATION && readFailure->result.step == Step::Open);
    observeRead();
    assert(FlushNormalObservations(root));
    assert(Load(normalStore, restored, persisted, error) && Find(restored, Kind::SetupRead, original.wstring())->result.outcome == Outcome::Passed);
    // A stale manual save merges into current evidence, never clobbering it.
    std::vector<Record> staleRows{{Kind::SettingsWrite, settings.wstring(), write}};
    staleRows.front().result.time = 1;
    assert(SaveLatest(normalStore, staleRows, baseline, error));
    assert(Find(staleRows, Kind::SettingsWrite, settings.wstring())->result.source == Source::Normal);
    // A locked/corrupt diagnostic ledger cannot fail the user's real save.
    locked = CreateFileW(normalStore.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(ObservedWriteUtf8(root, config, "third", settings, settings, &detail));
    assert(!FlushNormalObservations(root));
    live.clear(); unsaved = false;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError) && unsaved && normalError);
    CloseHandle(locked);
    { std::ofstream out(normalStore, std::ios::binary | std::ios::trunc); out << "broken ledger\n"; }
    assert(ObservedWriteUtf8(root, config, "fourth", settings, settings, &detail));
    assert(!FlushNormalObservations(root));
    { std::ifstream in(normalStore); std::string bytes; std::getline(in, bytes); assert(bytes == "broken ledger"); }
    live.clear(); unsaved = false;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError) && unsaved && normalError == ERROR_INVALID_DATA);
    assert(Find(live, Kind::SettingsWrite, settings.wstring())->result.outcome == Outcome::Passed);
    // Private managed paths identify their owner without reading UI globals.
    const auto temporary = resource / L"__tmp__" / L"stage.txt";
    assert(ObservedWriteUtf8(std::filesystem::path{}, temporary, "stage", temporary.parent_path(), {}, &detail));
    live.clear(); unsaved = false;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError));
    assert(Find(live, Kind::TempWrite, temporary.parent_path().wstring()));
    assert(!std::filesystem::exists(root / L"wrong_owner", ec) && !ec);
    // A chosen output folder named __pdf_note_workspace__ cannot redirect the result
    // ledger away from the explicitly captured workspace.
    const auto exportFolder = root / L"chosen_export" / L"__pdf_note_workspace__" / L"output";
    const auto exportFile = exportFolder / L"note.txt";
    assert(ObservedWriteUtf8(root, exportFile, "export", exportFolder, {}, &detail));
    assert(!std::filesystem::exists(exportFolder.parent_path() / L"__log__", ec) && !ec);
    live.clear(); unsaved = false;
    assert(MergeNormalObservations(normalStore, live, unsaved, normalError));
    assert(Find(live, Kind::NoteWrite, exportFolder.wstring()));
    ShutdownNormalObservations(); // Join before deleting owned fixture paths.
    assert(DeleteFileW(exportFile.c_str())); assert(RemoveDirectoryW(exportFolder.c_str()));
    assert(RemoveDirectoryW(exportFolder.parent_path().c_str()));
    assert(RemoveDirectoryW(exportFolder.parent_path().parent_path().c_str()));
    assert(DeleteFileW(temporary.c_str())); assert(RemoveDirectoryW(temporary.parent_path().c_str()));
    assert(DeleteFileW(config.c_str()));
    for (const auto& entry : std::filesystem::directory_iterator(settings)) {
        assert(entry.path().filename().wstring().rfind(L"theme.json.__atomic__.", 0) == 0);
        assert(DeleteFileW(entry.path().c_str())); // Only owned draft retained by the failed replacement.
    }
    assert(RemoveDirectoryW(settings.c_str()));
    assert(DeleteFileW(workspaceFile.c_str())); assert(DeleteFileW(normalStore.c_str()));
    assert(RemoveDirectoryW(normalStore.parent_path().c_str())); assert(RemoveDirectoryW(resource.c_str()));
    // Own fixture only, non-recursive cleanup. Originals are unchanged.
    { std::ifstream in(original); std::string text; in >> text; assert(text == "original"); }
    assert(DeleteFileW(original.c_str())); assert(DeleteFileW(store.c_str())); assert(DeleteFileW(broken.c_str())); assert(RemoveDirectoryW(root.c_str()));
    std::cout << "write checks persistence, probes and throttle passed\n";
    return 0;
}
