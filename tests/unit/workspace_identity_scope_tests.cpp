#include "workspace/workspace_resource_identity.h"
#include "note/note_identity_store.h"
#include "core/identity_path_scope.h"
#include "core/sha256.h"
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

std::recursive_mutex g_pdfiumMutex;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void Check(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
std::string Read(const fs::path& path) {
    std::string bytes; Check(ReadFileBytesWin32(path, bytes), "read owned fixture"); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << bytes; Check(bool(file), "write owned fixture");
}
void ChangeResourceVersion(std::string& bytes, unsigned char version) {
    bytes[8] = static_cast<char>(version);
    core_hash::Sha256 hash;
    hash.Update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()-32);
    const auto digest = hash.Finalize();
    std::copy(digest.begin(), digest.end(), bytes.end()-32);
}
struct ReadLock {
    HANDLE value;
    explicit ReadLock(const fs::path& path) : value(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)) { Check(value != INVALID_HANDLE_VALUE, "lock owned catalog"); }
    ~ReadLock() { CloseHandle(value); }
};
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--verify"), "owned fixture root and known phase supplied");
        const auto root = fs::absolute(argv[1]);
        const auto owned = identity_path_scope::LocalPath(root.wstring());
        const auto base = identity_path_scope::LocalPath((fs::current_path() / L"out" / L"tests").wstring());
        Check(owned && base && identity_path_scope::Below(*owned, *base) &&
            identity_path_scope::NoReparseAncestors(*owned), "fixture stays below owned out/tests without redirected ancestors");
        const auto workspace = root / L"workspace", external = root / L"registered";
        const auto store = workspace / L"__pdf_note_workspace__" / L"__settings__";
        const auto resourceStore = store / L"workspace_resource_registry.bin";
        const auto noteStore = store / L"note_identity_registry.bin";
        const auto pdf = external / L"session" / L"\u2460\u8cc7\u6599.pdf";
        const auto renamedPdf = external / L"session" / L"renamed.pdf";
        const auto note = external / L"session" / L"note.clro";
        const auto renamedNote = external / L"session" / L"renamed.clro";
        std::wstring error;
        Check(ConfigureRuntimeWorkspaceResourceCatalog(workspace, {external}, &error), "configure resource catalog");
        Check(note::ConfigureRuntimeNoteIdentityStore(workspace, &error), "configure note catalog");
        if (argc == 3 && std::string_view(argv[2]) == "--verify") {
            note::SetRuntimeNoteIdentityExternalRoots({external});
            std::ifstream expected(root / L"expected.txt"); std::string resourceId; std::uint64_t noteId = 0;
            expected >> resourceId >> noteId;
            const auto found = FindRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, renamedPdf.wstring(), &error);
            Check(found && WorkspaceResourceIdToHex(found->resource_id) == resourceId, "resource ID survives new process");
            Check(note::FindRuntimeNoteIdentityPath(renamedNote.wstring(), &error) == note::NoteId{noteId}, "note ID survives new process");
            Check(Read(renamedPdf) == "pdf bytes" && Read(renamedNote) == "note bytes", "restart preserves member bytes");
            std::cout << "Identity scope restart: " << checks << " checks passed\n"; return 0;
        }
        const auto internal = workspace / L"session" / L"inside.pdf";
        Write(internal, "pdf bytes"); Write(pdf, "pdf bytes"); Write(note, "note bytes");
        const auto inside = ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, internal.wstring(), &error);
        Check(inside.has_value(), "workspace file remains supported");
        const auto oldBytes = Read(resourceStore);
        Check(static_cast<unsigned char>(oldBytes[8]) == 1, "internal-only catalog keeps v1");
        Check(ConfigureRuntimeWorkspaceResourceCatalog(workspace, &error), "revoke external roots");
        Check(!ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, pdf.wstring(), &error), "unregistered external file rejected");
        Check(Read(resourceStore) == oldBytes && Read(pdf) == "pdf bytes", "rejection leaves catalog and source intact");
        const auto openedNote = note::ResolveRuntimeNoteIdentityPath(note.wstring(), &error);
        Check(openedNote.valid(), "open external note before registration");
        note::SetRuntimeNoteIdentityExternalRoots({external});
        const auto registeredNote = note::ResolveRuntimeNoteIdentityPath(note.wstring(), &error);
        Check(registeredNote.valid() && registeredNote.note_id == openedNote.note_id && error.empty(), "registration preserves live note ID");
        Check(ConfigureRuntimeWorkspaceResourceCatalog(workspace, {external}, &error), "register external root");
        const auto originalId = FindRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, internal.wstring(), &error);
        Check(originalId && originalId->resource_id == inside->resource_id, "legacy internal identity survives catalog reload");
        const auto resource = ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, pdf.wstring(), &error);
        Check(resource && resource->resource_id != inside->resource_id, "equal bytes at different paths have separate IDs");
        auto sidecar = pdf; sidecar.replace_extension(L".clrop"); Write(sidecar, "annotation bytes");
        Check(ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::LinkSet, sidecar.wstring(), &error).has_value(), "external annotation sidecar registered");
        Check(static_cast<unsigned char>(Read(resourceStore)[8]) == 2 &&
            static_cast<unsigned char>(Read(noteStore)[8]) == 3, "extended aliases use explicit catalog versions");

        fs::rename(pdf, renamedPdf);
        auto wrong = resource->fingerprint; wrong.size++;
        Check(!RebindRuntimeWorkspaceResourceIdentityPath(resource->resource_id, resource->kind,
            pdf.wstring(), renamedPdf.wstring(), wrong, &error), "wrong fingerprint cannot rebind");
        Check(RebindRuntimeWorkspaceResourceIdentityPath(resource->resource_id, resource->kind,
            pdf.wstring(), renamedPdf.wstring(), resource->fingerprint, &error), "rename preserves resource identity");
        Check(!FindRuntimeWorkspaceResourceIdentityPath(resource->kind, pdf.wstring(), &error) &&
            FindRuntimeWorkspaceResourceIdentityPath(resource->kind, renamedPdf.wstring(), &error)->resource_id == resource->resource_id,
            "only destination alias remains");
        const auto noteBytes = Read(noteStore);
        fs::rename(note, renamedNote);
        { ReadLock lock(noteStore);
          Check(!note::RebindRuntimeNoteIdentityPath(registeredNote.note_id, note.wstring(), renamedNote.wstring(), &error), "locked note catalog blocks rebind"); }
        Check(Read(noteStore) == noteBytes && note::FindRuntimeNoteIdentityPath(note.wstring(), &error) == registeredNote.note_id,
            "failed note rebind leaves alias and catalog intact");
        Check(note::RebindRuntimeNoteIdentityPath(registeredNote.note_id, note.wstring(), renamedNote.wstring(), &error), "note rebind succeeds after retry");
        const auto resourceBytes = Read(resourceStore);
        const auto pending = external / L"session" / L"pending.pdf"; Write(pending, "pending bytes");
        { ReadLock lock(resourceStore);
          Check(!ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, pending.wstring(), &error), "catalog write failure blocks ID allocation"); }
        Check(Read(resourceStore) == resourceBytes && Read(pending) == "pending bytes", "failed allocation preserves original bytes");
        Check(ConfigureRuntimeWorkspaceResourceCatalog(workspace, {}, &error), "reload catalog with revoked permission");
        note::SetRuntimeNoteIdentityExternalRoots({});
        Check(!FindRuntimeWorkspaceResourceIdentityPath(resource->kind, renamedPdf.wstring(), &error) &&
            !note::FindRuntimeNoteIdentityPath(renamedNote.wstring(), &error), "stored aliases never grant external permission");
        Check(Read(resourceStore) == resourceBytes, "revocation does not erase dormant identities");
        Check(ConfigureRuntimeWorkspaceResourceCatalog(workspace, {external / L"session", external}, &error), "overlapping roots are supported");
        note::SetRuntimeNoteIdentityExternalRoots({external});
        Check(FindRuntimeWorkspaceResourceIdentityPath(resource->kind, renamedPdf.wstring(), &error)->resource_id == resource->resource_id,
            "root order and nesting do not change IDs");
        const auto sibling = root / L"registered_other" / L"file.pdf"; Write(sibling, "outside");
        bool junction = false;
        Check(TryIsReparsePointNoFollow(external / L"junction", junction) && junction,
            "junction rejection case has an observed reparse fixture");
        for (const auto& bad : {sibling.wstring(), (external / L"junction" / L"escape.pdf").wstring(),
            (external / L".." / L"registered_other" / L"file.pdf").wstring(),
            std::wstring(L"\\\\not-a-server\\share\\file.pdf"), std::wstring(L"C:relative.pdf")})
            Check(!ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind::Pdf, bad, &error), "unsafe or unregistered path rejected before hashing");
        Check(!identity_path_scope::ValidExternalKey(L"@external:\\\\server\\share\\file") &&
            !identity_path_scope::ValidExternalKey(L"@external:c:\\file:stream") &&
            !identity_path_scope::ValidExternalKey(L"@external:c:\\a\\..\\b"), "malformed external catalog keys rejected");
        auto future = resourceBytes; ChangeResourceVersion(future, 99); Write(resourceStore, future);
        Check(!ConfigureRuntimeWorkspaceResourceCatalog(workspace, {external}, &error) && Read(resourceStore) == future,
            "unknown catalog version is rejected without rewriting");
        Write(resourceStore, resourceBytes);
        Check(ConfigureRuntimeWorkspaceResourceCatalog(workspace, {external}, &error), "restore owned catalog fixture");
        Write(root / L"expected.txt", WorkspaceResourceIdToHex(resource->resource_id) + "\n" +
            std::to_string(registeredNote.note_id.value) + "\n");
        Check(Read(renamedPdf) == "pdf bytes" && Read(sidecar) == "annotation bytes" && Read(renamedNote) == "note bytes", "all member contents preserved");
        std::cout << "Identity scope: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << " checks=" << checks << '\n'; return 1; }
}
