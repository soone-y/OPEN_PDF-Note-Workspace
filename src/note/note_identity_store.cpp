#include "note/note_identity_store.h"

#include "core/atomic_write.h"
#include "core/path_safety.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace note {
namespace {

constexpr std::array<char, 8> kCatalogMagic = {'P', 'N', 'I', 'D', 'R', 'E', 'G', '1'};
constexpr uint32_t kCatalogVersion = 2;
constexpr size_t kMaxCatalogBytes = 16 * 1024 * 1024;
constexpr uint64_t kMaxCatalogRecords = 1'000'000;
constexpr uint32_t kMaxRelativePathBytes = 1024 * 1024;

uint64_t RawFingerprint(std::string_view bytes) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash = kOffsetBasis;
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= kPrime;
    }
    return hash != 0 ? hash : 1;
}

uint64_t Mix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

void AppendU32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void AppendU64(std::string& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

bool ReadU32(std::string_view bytes, size_t* offset, uint32_t* out) {
    if (!offset || !out || *offset > bytes.size() || bytes.size() - *offset < 4) return false;
    uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(
                     static_cast<unsigned char>(bytes[*offset + static_cast<size_t>(shift / 8)]))
                 << shift;
    }
    *offset += 4;
    *out = value;
    return true;
}

bool ReadU64(std::string_view bytes, size_t* offset, uint64_t* out) {
    if (!offset || !out || *offset > bytes.size() || bytes.size() - *offset < 8) return false;
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(
                     static_cast<unsigned char>(bytes[*offset + static_cast<size_t>(shift / 8)]))
                 << shift;
    }
    *offset += 8;
    *out = value;
    return true;
}

std::string WideToUtf8Strict(std::wstring_view text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                             text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string out(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            text.data(), static_cast<int>(text.size()),
                            out.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return out;
}

std::optional<std::wstring> Utf8ToWideStrict(std::string_view text) {
    if (text.empty()) return std::wstring{};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             text.data(), static_cast<int>(text.size()),
                                             nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring out(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            text.data(), static_cast<int>(text.size()),
                            out.data(), required) != required) {
        return std::nullopt;
    }
    return out;
}

std::wstring NormalizeRelativePathKey(std::filesystem::path path) {
    path = path.lexically_normal();
    std::wstring key = path.wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return key;
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    for (const auto& part : path) {
        if (part == L"..") return false;
        for (const wchar_t ch : part.wstring()) {
            if (ch < 0x20 || ch == L':' || ch == L'*' || ch == L'?' ||
                ch == L'\"' || ch == L'<' || ch == L'>' || ch == L'|') {
                return false;
            }
        }
    }
    return true;
}

struct RuntimeStoreState {
    std::mutex mutex;
    bool configured = false;
    bool dirty = false;
    std::filesystem::path workspace_root;
    std::filesystem::path store_path;
    PersistentNoteIdentityCatalog catalog;
};

RuntimeStoreState& RuntimeStore() {
    static RuntimeStoreState state;
    return state;
}

uint64_t NewWorkspaceNonce(const std::filesystem::path& root) {
    const auto now = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const uint64_t process = static_cast<uint64_t>(GetCurrentProcessId());
    const uint64_t ticks = static_cast<uint64_t>(GetTickCount64());
    const std::string rootUtf8 = WideToUtf8Strict(NormalizePathKey(root.wstring()));
    uint64_t nonce = Mix64(now ^ (process << 32) ^ ticks ^ RawFingerprint(rootUtf8));
    return nonce != 0 ? nonce : 1;
}

std::optional<std::string> RelativeKeyForPath(const RuntimeStoreState& state,
                                              const std::wstring& absolutePath) {
    if (!state.configured || state.workspace_root.empty() || absolutePath.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path absolute =
        std::filesystem::path(NormalizePathKey(absolutePath)).lexically_normal();
    const std::filesystem::path root =
        std::filesystem::path(NormalizePathKey(state.workspace_root.wstring())).lexically_normal();
    const std::wstring absoluteKey = NormalizePathKey(absolute.wstring());
    std::wstring rootKey = NormalizePathKey(root.wstring());
    if (rootKey.empty()) return std::nullopt;
    if (rootKey.back() != L'\\') rootKey.push_back(L'\\');
    if (absoluteKey.size() <= rootKey.size() ||
        absoluteKey.compare(0, rootKey.size(), rootKey) != 0) {
        return std::nullopt;
    }
    const std::filesystem::path relative = absolute.lexically_relative(root);
    if (!IsSafeRelativePath(relative)) return std::nullopt;
    const std::wstring key = NormalizeRelativePathKey(relative);
    const std::string utf8 = WideToUtf8Strict(key);
    if (utf8.empty() || utf8.size() > kMaxRelativePathBytes) return std::nullopt;
    return utf8;
}

std::wstring AbsoluteKeyForRecord(const RuntimeStoreState& state,
                                  std::string_view relativeKey) {
    const auto relativeWide = Utf8ToWideStrict(relativeKey);
    if (!relativeWide.has_value()) return {};
    const std::filesystem::path relative(*relativeWide);
    if (!IsSafeRelativePath(relative)) return {};
    return NormalizePathKey((state.workspace_root / relative).wstring());
}

bool PersistRuntimeStoreLocked(RuntimeStoreState& state, std::wstring* outError) {
    if (outError) outError->clear();
    if (!state.configured || state.store_path.empty()) {
        if (outError) *outError = L"note identity store is not configured";
        return false;
    }
    const std::string bytes = state.catalog.Serialize();
    const std::filesystem::path resource = state.workspace_root / L"__resource__";
    const std::filesystem::path preferredTemp = resource / L"__tmp__";
    const std::filesystem::path quarantine = resource / L"__escape__";
    std::wstring err;
    if (!atomic_write::AtomicWriteBytes(state.store_path,
                                        bytes.data(), bytes.size(),
                                        preferredTemp, quarantine, &err)) {
        state.dirty = true;
        if (outError) *outError = err;
        return false;
    }
    state.dirty = false;
    return true;
}

} // namespace

void PersistentNoteIdentityCatalog::Reset(uint64_t workspaceNonce) {
    workspace_nonce_ = workspaceNonce != 0 ? workspaceNonce : 1;
    next_sequence_ = 1;
    records_by_path_.clear();
    path_by_note_id_.clear();
}

bool PersistentNoteIdentityCatalog::Parse(std::string_view bytes, std::wstring* outError) {
    if (outError) outError->clear();
    if (bytes.size() < 48 || bytes.size() > kMaxCatalogBytes) {
        if (outError) *outError = L"note identity catalog size is invalid";
        return false;
    }
    if (!std::equal(kCatalogMagic.begin(), kCatalogMagic.end(), bytes.begin())) {
        if (outError) *outError = L"note identity catalog magic is invalid";
        return false;
    }
    const size_t checksumOffset = bytes.size() - 8;
    size_t checksumReadOffset = checksumOffset;
    uint64_t storedChecksum = 0;
    if (!ReadU64(bytes, &checksumReadOffset, &storedChecksum) ||
        storedChecksum != RawFingerprint(bytes.substr(0, checksumOffset))) {
        if (outError) *outError = L"note identity catalog checksum is invalid";
        return false;
    }

    size_t offset = kCatalogMagic.size();
    uint32_t version = 0;
    uint32_t reserved = 0;
    uint64_t nonce = 0;
    uint64_t nextSequence = 0;
    uint64_t count = 0;
    if (!ReadU32(bytes, &offset, &version) ||
        !ReadU32(bytes, &offset, &reserved) ||
        !ReadU64(bytes, &offset, &nonce) ||
        !ReadU64(bytes, &offset, &nextSequence) ||
        !ReadU64(bytes, &offset, &count) ||
        (version != 1 && version != kCatalogVersion) || reserved != 0 || nonce == 0 ||
        nextSequence == 0 || count > kMaxCatalogRecords) {
        if (outError) *outError = L"note identity catalog header is invalid";
        return false;
    }

    PersistentNoteIdentityCatalog parsed;
    parsed.Reset(nonce);
    parsed.next_sequence_ = nextSequence;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t noteIdValue = 0;
        uint64_t persistenceRevision = 0;
        uint64_t diskFingerprint = 0;
        uint32_t pathSize = 0;
        if (!ReadU64(bytes, &offset, &noteIdValue) ||
            (version >= 2 &&
             (!ReadU64(bytes, &offset, &persistenceRevision) ||
              !ReadU64(bytes, &offset, &diskFingerprint))) ||
            !ReadU32(bytes, &offset, &pathSize) ||
            noteIdValue == 0 || pathSize == 0 || pathSize > kMaxRelativePathBytes ||
            offset > checksumOffset || checksumOffset - offset < pathSize) {
            if (outError) *outError = L"note identity catalog record is invalid";
            return false;
        }
        std::string path(bytes.substr(offset, pathSize));
        offset += pathSize;
        const auto pathWide = Utf8ToWideStrict(path);
        const std::string pathKey = path;
        if (!pathWide.has_value() ||
            !IsSafeRelativePath(std::filesystem::path(*pathWide)) ||
            !parsed.BindPath(NoteId{noteIdValue}, std::move(path))) {
            if (outError) *outError = L"note identity catalog contains duplicate or invalid records";
            return false;
        }
        if ((persistenceRevision == 0) != (diskFingerprint == 0)) {
            if (outError) *outError = L"note identity catalog persistence record is invalid";
            return false;
        }
        PersistentNoteIdentityRecord& record = parsed.records_by_path_[pathKey];
        record.persistence_revision = persistenceRevision;
        record.disk_fingerprint = diskFingerprint;
    }
    if (offset != checksumOffset) {
        if (outError) *outError = L"note identity catalog has trailing data";
        return false;
    }
    *this = std::move(parsed);
    return true;
}

std::string PersistentNoteIdentityCatalog::Serialize() const {
    std::string out;
    out.reserve(48 + records_by_path_.size() * 80);
    out.append(kCatalogMagic.data(), kCatalogMagic.size());
    AppendU32(out, kCatalogVersion);
    AppendU32(out, 0);
    AppendU64(out, workspace_nonce_ != 0 ? workspace_nonce_ : 1);
    AppendU64(out, next_sequence_ != 0 ? next_sequence_ : 1);
    AppendU64(out, static_cast<uint64_t>(records_by_path_.size()));
    for (const auto& [path, record] : records_by_path_) {
        AppendU64(out, record.note_id.value);
        AppendU64(out, record.persistence_revision);
        AppendU64(out, record.disk_fingerprint);
        AppendU32(out, static_cast<uint32_t>(path.size()));
        out.append(path);
    }
    AppendU64(out, RawFingerprint(out));
    return out;
}

std::optional<NoteId> PersistentNoteIdentityCatalog::FindPath(
    std::string_view relativePathKeyUtf8) const {
    const auto it = records_by_path_.find(std::string(relativePathKeyUtf8));
    return it != records_by_path_.end()
        ? std::optional<NoteId>(it->second.note_id)
        : std::nullopt;
}

std::optional<std::string> PersistentNoteIdentityCatalog::FindNote(NoteId noteId) const {
    if (!noteId.valid()) return std::nullopt;
    const auto it = path_by_note_id_.find(noteId.value);
    return it != path_by_note_id_.end() ? std::optional<std::string>(it->second) : std::nullopt;
}

std::optional<PersistentNoteIdentityRecord> PersistentNoteIdentityCatalog::FindRecord(
    NoteId noteId) const {
    const auto path = FindNote(noteId);
    if (!path.has_value()) return std::nullopt;
    const auto it = records_by_path_.find(*path);
    if (it == records_by_path_.end()) return std::nullopt;
    return it->second;
}

NoteId PersistentNoteIdentityCatalog::AllocateNoteId() {
    if (workspace_nonce_ == 0) workspace_nonce_ = 1;
    for (;;) {
        const uint64_t sequence = next_sequence_++;
        if (next_sequence_ == 0) next_sequence_ = 1;
        const NoteId candidate{Mix64(workspace_nonce_ ^ sequence)};
        if (candidate.valid() && path_by_note_id_.find(candidate.value) == path_by_note_id_.end()) {
            return candidate;
        }
    }
}

bool PersistentNoteIdentityCatalog::BindPath(NoteId noteId,
                                             std::string relativePathKeyUtf8) {
    if (!noteId.valid() || relativePathKeyUtf8.empty() ||
        relativePathKeyUtf8.size() > kMaxRelativePathBytes) {
        return false;
    }
    const auto pathWide = Utf8ToWideStrict(relativePathKeyUtf8);
    if (!pathWide.has_value() ||
        !IsSafeRelativePath(std::filesystem::path(*pathWide))) {
        return false;
    }
    const auto pathIt = records_by_path_.find(relativePathKeyUtf8);
    if (pathIt != records_by_path_.end() && pathIt->second.note_id != noteId) return false;
    if (pathIt == records_by_path_.end() && records_by_path_.size() >= kMaxCatalogRecords) {
        return false;
    }
    const auto noteIt = path_by_note_id_.find(noteId.value);
    if (noteIt != path_by_note_id_.end() && noteIt->second != relativePathKeyUtf8) return false;
    PersistentNoteIdentityRecord& record = records_by_path_[relativePathKeyUtf8];
    record.note_id = noteId;
    path_by_note_id_[noteId.value] = std::move(relativePathKeyUtf8);
    return true;
}

bool PersistentNoteIdentityCatalog::RebindPath(NoteId noteId,
                                               std::string relativePathKeyUtf8) {
    if (!noteId.valid() || relativePathKeyUtf8.empty()) return false;
    const auto pathWide = Utf8ToWideStrict(relativePathKeyUtf8);
    if (!pathWide.has_value() ||
        !IsSafeRelativePath(std::filesystem::path(*pathWide))) {
        return false;
    }
    const auto targetIt = records_by_path_.find(relativePathKeyUtf8);
    if (targetIt != records_by_path_.end() && targetIt->second.note_id != noteId) return false;
    const auto noteIt = path_by_note_id_.find(noteId.value);
    if (noteIt == path_by_note_id_.end()) return false;
    const PersistentNoteIdentityRecord record = records_by_path_.at(noteIt->second);
    records_by_path_.erase(noteIt->second);
    noteIt->second = relativePathKeyUtf8;
    records_by_path_[std::move(relativePathKeyUtf8)] = record;
    return true;
}

std::optional<uint64_t> PersistentNoteIdentityCatalog::ObserveDisk(
    NoteId noteId,
    uint64_t contentFingerprint) {
    if (!noteId.valid() || contentFingerprint == 0) return std::nullopt;
    const auto path = FindNote(noteId);
    if (!path.has_value()) return std::nullopt;
    PersistentNoteIdentityRecord& record = records_by_path_.at(*path);
    if (record.disk_fingerprint == contentFingerprint &&
        record.persistence_revision != 0) {
        return record.persistence_revision;
    }
    if (record.persistence_revision == std::numeric_limits<uint64_t>::max()) {
        return std::nullopt;
    }
    ++record.persistence_revision;
    if (record.persistence_revision == 0) ++record.persistence_revision;
    record.disk_fingerprint = contentFingerprint;
    return record.persistence_revision;
}

bool PersistentNoteIdentityCatalog::CommitPersistence(
    NoteId noteId,
    uint64_t expectedBaseRevision,
    uint64_t committedRevision,
    uint64_t contentFingerprint) {
    if (!noteId.valid() || committedRevision == 0 || contentFingerprint == 0) return false;
    const auto path = FindNote(noteId);
    if (!path.has_value()) return false;
    PersistentNoteIdentityRecord& record = records_by_path_.at(*path);
    if (record.persistence_revision == committedRevision &&
        record.disk_fingerprint == contentFingerprint) {
        return true;
    }
    if (record.persistence_revision != expectedBaseRevision ||
        committedRevision != expectedBaseRevision + 1 ||
        committedRevision == 0) {
        return false;
    }
    record.persistence_revision = committedRevision;
    record.disk_fingerprint = contentFingerprint;
    return true;
}

bool ConfigureRuntimeNoteIdentityStore(const std::filesystem::path& workspaceRoot,
                                       std::wstring* outError) {
    if (outError) outError->clear();
    if (workspaceRoot.empty()) {
        if (outError) *outError = L"workspace root is empty";
        return false;
    }
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.configured = false;
    state.dirty = false;
    state.workspace_root = std::filesystem::path(
        NormalizePathKey(workspaceRoot.wstring()));
    state.store_path = state.workspace_root / L"__resource__" / L"__settings__" /
                       L"note_identity_registry.bin";

    PersistentNoteIdentityCatalog catalog;
    if (RegularFileExistsWin32(state.store_path)) {
        std::string bytes;
        if (!ReadFileBytesWin32(state.store_path, bytes) ||
            !catalog.Parse(bytes, outError)) {
            return false;
        }
    } else {
        catalog.Reset(NewWorkspaceNonce(state.workspace_root));
    }

    NoteIdentityRegistry& registry = RuntimeNoteIdentityRegistry();
    for (const auto& [relativeKey, record] : catalog.records()) {
        const NoteId noteId = record.note_id;
        const std::wstring absoluteKey = AbsoluteKeyForRecord(state, relativeKey);
        if (absoluteKey.empty()) {
            if (outError) *outError = L"note identity catalog path is invalid";
            return false;
        }
        const auto byId = registry.FindNote(noteId);
        const auto byPath = registry.FindPath(absoluteKey);
        if ((byId.has_value() && byId->path_key != absoluteKey) ||
            (byPath.has_value() && byPath->note_id != noteId)) {
            if (outError) *outError = L"note identity catalog conflicts with runtime identity";
            return false;
        }
    }
    for (const auto& [relativeKey, record] : catalog.records()) {
        const NoteId noteId = record.note_id;
        const std::wstring absoluteKey = AbsoluteKeyForRecord(state, relativeKey);
        if (!registry.ImportPath(noteId, absoluteKey)) {
            if (outError) *outError = L"failed to import note identity catalog";
            return false;
        }
    }
    state.catalog = std::move(catalog);
    state.configured = true;
    return true;
}

NoteIdentity ResolveRuntimeNoteIdentityPath(const std::wstring& absolutePath,
                                            std::wstring* outError) {
    if (outError) outError->clear();
    const std::wstring absoluteKey = NormalizePathKey(absolutePath);
    if (absoluteKey.empty()) return {};
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    NoteIdentityRegistry& registry = RuntimeNoteIdentityRegistry();
    const auto relativeKey = RelativeKeyForPath(state, absoluteKey);
    if (!relativeKey.has_value()) return registry.ResolvePath(absoluteKey);

    if (const auto storedId = state.catalog.FindPath(*relativeKey)) {
        if (registry.ImportPath(*storedId, absoluteKey)) {
            if (state.dirty) PersistRuntimeStoreLocked(state, outError);
            return *registry.FindNote(*storedId);
        }
        if (outError) *outError = L"persistent note identity conflicts with runtime identity";
        return registry.ResolvePath(absoluteKey);
    }

    NoteId noteId;
    do {
        noteId = state.catalog.AllocateNoteId();
    } while (registry.FindNote(noteId).has_value());
    if (!state.catalog.BindPath(noteId, *relativeKey) ||
        !registry.ImportPath(noteId, absoluteKey)) {
        if (outError) *outError = L"failed to allocate persistent note identity";
        return registry.ResolvePath(absoluteKey);
    }
    PersistRuntimeStoreLocked(state, outError);
    return *registry.FindNote(noteId);
}

bool RebindRuntimeNoteIdentityPath(NoteId noteId,
                                   const std::wstring& oldAbsolutePath,
                                   const std::wstring& newAbsolutePath,
                                   std::wstring* outError) {
    if (outError) outError->clear();
    if (!noteId.valid()) return false;
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    const auto oldRelative = RelativeKeyForPath(state, oldAbsolutePath);
    const auto newRelative = RelativeKeyForPath(state, newAbsolutePath);
    if (!state.configured) {
        return RuntimeNoteIdentityRegistry().BindPath(
            noteId, NormalizePathKey(newAbsolutePath));
    }
    if (!oldRelative.has_value() || !newRelative.has_value()) {
        if (outError) *outError = L"note identity rebind path is outside the workspace";
        return false;
    }
    const auto storedId = state.catalog.FindPath(*oldRelative);
    if (!storedId.has_value() || *storedId != noteId) {
        if (outError) *outError = L"note identity rebind source does not match";
        return false;
    }

    PersistentNoteIdentityCatalog previous = state.catalog;
    if (!state.catalog.RebindPath(noteId, *newRelative) ||
        !PersistRuntimeStoreLocked(state, outError)) {
        state.catalog = std::move(previous);
        return false;
    }
    const std::wstring newKey = NormalizePathKey(newAbsolutePath);
    if (!RuntimeNoteIdentityRegistry().BindPath(noteId, newKey)) {
        state.catalog = std::move(previous);
        PersistRuntimeStoreLocked(state, nullptr);
        if (outError) *outError = L"runtime note identity rebind failed";
        return false;
    }
    return true;
}

uint64_t ObserveRuntimeNoteDiskSnapshot(NoteId noteId,
                                        uint64_t contentFingerprint,
                                        std::wstring* outError) {
    if (outError) outError->clear();
    if (!noteId.valid() || contentFingerprint == 0) {
        if (outError) *outError = L"note persistence observation is invalid";
        return 0;
    }
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.configured) return 0;

    PersistentNoteIdentityCatalog previous = state.catalog;
    const auto revision = state.catalog.ObserveDisk(noteId, contentFingerprint);
    if (!revision.has_value()) {
        if (outError) *outError = L"note persistence record was not found";
        return 0;
    }
    const auto previousRecord = previous.FindRecord(noteId);
    if (previousRecord.has_value() &&
        previousRecord->persistence_revision == *revision &&
        previousRecord->disk_fingerprint == contentFingerprint) {
        if (state.dirty && !PersistRuntimeStoreLocked(state, outError)) return 0;
        return *revision;
    }
    if (!PersistRuntimeStoreLocked(state, outError)) {
        state.catalog = std::move(previous);
        return 0;
    }
    return *revision;
}

std::optional<PersistentNoteIdentityRecord> FindRuntimeNotePersistenceRecord(
    NoteId noteId,
    std::wstring* outError) {
    if (outError) outError->clear();
    if (!noteId.valid()) return std::nullopt;
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.configured) return std::nullopt;
    if (state.dirty && !PersistRuntimeStoreLocked(state, outError)) return std::nullopt;
    return state.catalog.FindRecord(noteId);
}

bool CommitRuntimeNotePersistence(NoteId noteId,
                                  uint64_t expectedBaseRevision,
                                  uint64_t committedRevision,
                                  uint64_t contentFingerprint,
                                  std::wstring* outError) {
    if (outError) outError->clear();
    if (!noteId.valid() || committedRevision == 0 || contentFingerprint == 0) {
        if (outError) *outError = L"note persistence commit is invalid";
        return false;
    }
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.configured) return true;

    PersistentNoteIdentityCatalog previous = state.catalog;
    if (!state.catalog.CommitPersistence(noteId, expectedBaseRevision,
                                         committedRevision, contentFingerprint)) {
        if (outError) *outError = L"note persistence revision conflict";
        return false;
    }
    if (!PersistRuntimeStoreLocked(state, outError)) {
        state.catalog = std::move(previous);
        return false;
    }
    return true;
}

std::filesystem::path RuntimeNoteIdentityStorePath() {
    RuntimeStoreState& state = RuntimeStore();
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.store_path;
}

} // namespace note
