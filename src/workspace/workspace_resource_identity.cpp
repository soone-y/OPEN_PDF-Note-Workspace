#include "workspace/workspace_resource_identity.h"

#include "clrop/hash.h"
#include "core/atomic_write.h"
#include "diagnostics/normal_operations.h"
#include "core/path_safety.h"
#include "core/sha256.h"

#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace {

constexpr std::array<char, 8> kCatalogMagic = {'W', 'R', 'I', 'D', 'R', 'E', 'G', '1'};
constexpr std::uint32_t kCatalogVersion = 1;
constexpr size_t kCatalogDigestBytes = 32;
constexpr size_t kMaxCatalogBytes = 16 * 1024 * 1024;
constexpr std::uint64_t kMaxCatalogRecords = 1'000'000;
constexpr std::uint32_t kMaxRelativePathBytes = 1024 * 1024;

struct CatalogRecord {
    WorkspaceResourceId resourceId{};
    WorkspaceResourceKind kind = WorkspaceResourceKind::Pdf;
    WorkspaceResourceFingerprint fingerprint;
};

class WorkspaceResourceCatalog final {
public:
    [[nodiscard]] bool Parse(std::string_view bytes, std::wstring* outError);
    [[nodiscard]] std::string Serialize() const;
    [[nodiscard]] std::optional<CatalogRecord> FindPath(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> FindId(WorkspaceResourceId resourceId) const;
    [[nodiscard]] bool BindPath(CatalogRecord record, std::string path);
    [[nodiscard]] bool RebindPath(WorkspaceResourceId resourceId,
                                  WorkspaceResourceKind kind,
                                  std::string path,
                                  WorkspaceResourceFingerprint fingerprint);
    [[nodiscard]] bool UpdateFingerprint(WorkspaceResourceId resourceId,
                                         const WorkspaceResourceFingerprint& fingerprint);

private:
    std::map<std::string, CatalogRecord> recordsByPath_;
    std::map<WorkspaceResourceId, std::string> pathById_;
};

struct RuntimeCatalogState {
    std::mutex mutex;
    bool configured = false;
    bool dirty = false;
    std::filesystem::path workspaceRoot;
    std::filesystem::path storePath;
    WorkspaceResourceCatalog catalog;
};

RuntimeCatalogState& RuntimeCatalog() {
    static RuntimeCatalogState state;
    return state;
}

void AppendU32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void AppendU64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

[[nodiscard]] bool ReadU32(std::string_view bytes, size_t* offset, std::uint32_t* out) {
    if (!offset || !out || *offset > bytes.size() || bytes.size() - *offset < 4) return false;
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(bytes[*offset + static_cast<size_t>(shift / 8)]))
                 << shift;
    }
    *offset += 4;
    *out = value;
    return true;
}

[[nodiscard]] bool ReadU64(std::string_view bytes, size_t* offset, std::uint64_t* out) {
    if (!offset || !out || *offset > bytes.size() || bytes.size() - *offset < 8) return false;
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(bytes[*offset + static_cast<size_t>(shift / 8)]))
                 << shift;
    }
    *offset += 8;
    *out = value;
    return true;
}

[[nodiscard]] bool IsValidKind(WorkspaceResourceKind kind) {
    return kind == WorkspaceResourceKind::Pdf || kind == WorkspaceResourceKind::LinkSet;
}

[[nodiscard]] bool IsSha256(std::string_view value) {
    if (value.size() != 64) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

[[nodiscard]] core_hash::Sha256Digest Digest(std::string_view bytes) {
    core_hash::Sha256 hash;
    hash.Update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return hash.Finalize();
}

[[nodiscard]] bool DigestMatches(std::string_view bytes,
                                 const std::array<std::uint8_t, kCatalogDigestBytes>& expected) {
    return Digest(bytes) == expected;
}

[[nodiscard]] std::string WideToUtf8Strict(std::wstring_view text) {
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

[[nodiscard]] std::optional<std::wstring> Utf8ToWideStrict(std::string_view text) {
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

[[nodiscard]] std::wstring NormalizeRelativePathKey(std::filesystem::path path) {
    path = path.lexically_normal();
    std::wstring key = path.wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return key;
}

[[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    for (const auto& part : path) {
        if (part == L"..") return false;
        for (const wchar_t ch : part.wstring()) {
            if (ch < 0x20 || ch == L':' || ch == L'*' || ch == L'?' || ch == L'\"' ||
                ch == L'<' || ch == L'>' || ch == L'|') {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::string> RelativeKeyForPath(const RuntimeCatalogState& state,
                                                             const std::wstring& absolutePath) {
    if (!state.configured || state.workspaceRoot.empty() || absolutePath.empty()) return std::nullopt;
    const std::filesystem::path absolute(
        NormalizePathKey(absolutePath));
    const std::filesystem::path root(
        NormalizePathKey(state.workspaceRoot.wstring()));
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
    const std::string key = WideToUtf8Strict(NormalizeRelativePathKey(relative));
    if (key.empty() || key.size() > kMaxRelativePathBytes) return std::nullopt;
    return key;
}

[[nodiscard]] bool CaptureFingerprint(const std::filesystem::path& path,
                                      WorkspaceResourceFingerprint* out,
                                      std::wstring* outError) {
    if (out) *out = {};
    if (outError) outError->clear();
    if (path.empty()) return false;
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) {
        if (outError) *outError = L"workspace resource rejected a reparse point";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        if (outError) *outError = L"workspace resource requires a regular file";
        return false;
    }
    const std::uintmax_t beforeSize = std::filesystem::file_size(path, ec);
    if (ec || beforeSize > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        if (outError) *outError = L"workspace resource could not read the file size";
        return false;
    }
    WorkspaceResourceFingerprint fingerprint;
    fingerprint.size = static_cast<std::uint64_t>(beforeSize);
    if (!clrop::ComputeFileSha256(path, &fingerprint.sha256) || !fingerprint.valid()) {
        if (outError) *outError = L"workspace resource could not calculate SHA-256";
        return false;
    }
    const std::uintmax_t afterSize = std::filesystem::file_size(path, ec);
    if (ec || afterSize != beforeSize) {
        if (outError) *outError = L"workspace resource changed during fingerprint capture";
        return false;
    }
    if (out) *out = std::move(fingerprint);
    return true;
}

[[nodiscard]] WorkspaceResourceId CreateResourceId() {
    GUID uuid{};
    if (FAILED(CoCreateGuid(&uuid))) return {};
    WorkspaceResourceId result;
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&uuid);
    std::copy(raw, raw + result.value.size(), result.value.begin());
    return result;
}

[[nodiscard]] bool PersistLocked(RuntimeCatalogState& state, std::wstring* outError) {
    if (outError) outError->clear();
    if (!state.configured || state.storePath.empty()) {
        if (outError) *outError = L"workspace resource catalog is not configured";
        return false;
    }
    const std::string bytes = state.catalog.Serialize();
    const std::filesystem::path resource = state.workspaceRoot / L"__pdf_note_workspace__";
    std::wstring error;
    if (!write_checks::ObservedWriteBytes(state.workspaceRoot, state.storePath, bytes.data(), bytes.size(),
                                        resource / L"__tmp__", resource / L"__escape__", &error)) {
        state.dirty = true;
        if (outError) *outError = error;
        return false;
    }
    state.dirty = false;
    return true;
}

} // namespace

bool WorkspaceResourceId::valid() const {
    return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0; });
}

bool WorkspaceResourceFingerprint::valid() const {
    return IsSha256(sha256);
}

std::string WorkspaceResourceIdToHex(WorkspaceResourceId resourceId) {
    if (!resourceId.valid()) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(resourceId.value.size() * 2);
    for (const std::uint8_t byte : resourceId.value) {
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0fu]);
    }
    return encoded;
}

std::optional<WorkspaceResourceId> WorkspaceResourceIdFromHex(std::string_view encoded) {
    if (encoded.size() != 32) return std::nullopt;
    WorkspaceResourceId resourceId;
    auto fromHex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (size_t index = 0; index < resourceId.value.size(); ++index) {
        const int high = fromHex(encoded[index * 2]);
        const int low = fromHex(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        resourceId.value[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return resourceId.valid() ? std::optional<WorkspaceResourceId>(resourceId) : std::nullopt;
}

bool WorkspaceResourceCatalog::Parse(std::string_view bytes, std::wstring* outError) {
    if (outError) outError->clear();
    constexpr size_t kHeaderBytes = kCatalogMagic.size() + 4 + 4 + 8;
    if (bytes.size() < kHeaderBytes + kCatalogDigestBytes || bytes.size() > kMaxCatalogBytes ||
        !std::equal(kCatalogMagic.begin(), kCatalogMagic.end(), bytes.begin())) {
        if (outError) *outError = L"workspace resource catalog header is invalid";
        return false;
    }
    const size_t digestOffset = bytes.size() - kCatalogDigestBytes;
    std::array<std::uint8_t, kCatalogDigestBytes> storedDigest{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data() + digestOffset),
                storedDigest.size(), storedDigest.begin());
    if (!DigestMatches(bytes.substr(0, digestOffset), storedDigest)) {
        if (outError) *outError = L"workspace resource catalog checksum is invalid";
        return false;
    }

    size_t offset = kCatalogMagic.size();
    std::uint32_t version = 0;
    std::uint32_t reserved = 0;
    std::uint64_t count = 0;
    if (!ReadU32(bytes, &offset, &version) || !ReadU32(bytes, &offset, &reserved) ||
        !ReadU64(bytes, &offset, &count) || version != kCatalogVersion || reserved != 0 ||
        count > kMaxCatalogRecords) {
        if (outError) *outError = L"workspace resource catalog header fields are invalid";
        return false;
    }

    WorkspaceResourceCatalog parsed;
    for (std::uint64_t index = 0; index < count; ++index) {
        if (offset > digestOffset || digestOffset - offset < 16 + 8 + 8 + 64 + 4) {
            if (outError) *outError = L"workspace resource catalog record is truncated";
            return false;
        }
        CatalogRecord record;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data() + offset),
                    record.resourceId.value.size(), record.resourceId.value.begin());
        offset += record.resourceId.value.size();
        const auto rawKind = static_cast<std::uint8_t>(bytes[offset++]);
        bool nonzeroReserved = false;
        for (size_t reservedIndex = 0; reservedIndex < 7; ++reservedIndex) {
            nonzeroReserved = nonzeroReserved || bytes[offset++] != 0;
        }
        record.kind = static_cast<WorkspaceResourceKind>(rawKind);
        if (!record.resourceId.valid() || !IsValidKind(record.kind) || nonzeroReserved ||
            !ReadU64(bytes, &offset, &record.fingerprint.size) ||
            digestOffset - offset < 64) {
            if (outError) *outError = L"workspace resource catalog record header is invalid";
            return false;
        }
        record.fingerprint.sha256.assign(bytes.substr(offset, 64));
        offset += 64;
        std::uint32_t pathSize = 0;
        if (!record.fingerprint.valid() || !ReadU32(bytes, &offset, &pathSize) || pathSize == 0 ||
            pathSize > kMaxRelativePathBytes || offset > digestOffset ||
            digestOffset - offset < pathSize) {
            if (outError) *outError = L"workspace resource catalog record data is invalid";
            return false;
        }
        std::string path(bytes.substr(offset, pathSize));
        offset += pathSize;
        const auto pathWide = Utf8ToWideStrict(path);
        if (!pathWide.has_value() || !IsSafeRelativePath(std::filesystem::path(*pathWide)) ||
            WideToUtf8Strict(NormalizeRelativePathKey(std::filesystem::path(*pathWide))) != path ||
            !parsed.BindPath(std::move(record), std::move(path))) {
            if (outError) *outError = L"workspace resource catalog contains duplicate or unsafe records";
            return false;
        }
    }
    if (offset != digestOffset) {
        if (outError) *outError = L"workspace resource catalog has trailing data";
        return false;
    }
    *this = std::move(parsed);
    return true;
}

std::string WorkspaceResourceCatalog::Serialize() const {
    std::string out;
    out.reserve(kCatalogMagic.size() + 16 + recordsByPath_.size() * 128 + kCatalogDigestBytes);
    out.append(kCatalogMagic.data(), kCatalogMagic.size());
    AppendU32(out, kCatalogVersion);
    AppendU32(out, 0);
    AppendU64(out, static_cast<std::uint64_t>(recordsByPath_.size()));
    for (const auto& [path, record] : recordsByPath_) {
        out.append(reinterpret_cast<const char*>(record.resourceId.value.data()), record.resourceId.value.size());
        out.push_back(static_cast<char>(record.kind));
        out.append(7, '\0');
        AppendU64(out, record.fingerprint.size);
        out.append(record.fingerprint.sha256);
        AppendU32(out, static_cast<std::uint32_t>(path.size()));
        out.append(path);
    }
    const auto digest = Digest(out);
    out.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    return out;
}

std::optional<CatalogRecord> WorkspaceResourceCatalog::FindPath(std::string_view path) const {
    const auto it = recordsByPath_.find(std::string(path));
    return it == recordsByPath_.end() ? std::nullopt : std::optional<CatalogRecord>(it->second);
}

std::optional<std::string> WorkspaceResourceCatalog::FindId(WorkspaceResourceId resourceId) const {
    const auto it = pathById_.find(resourceId);
    return it == pathById_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

bool WorkspaceResourceCatalog::BindPath(CatalogRecord record, std::string path) {
    if (!record.resourceId.valid() || !IsValidKind(record.kind) || !record.fingerprint.valid() ||
        path.empty() || path.size() > kMaxRelativePathBytes) {
        return false;
    }
    const auto pathWide = Utf8ToWideStrict(path);
    if (!pathWide.has_value() || !IsSafeRelativePath(std::filesystem::path(*pathWide))) return false;
    const auto pathIt = recordsByPath_.find(path);
    if (pathIt != recordsByPath_.end() && pathIt->second.resourceId != record.resourceId) return false;
    if (pathIt == recordsByPath_.end() && recordsByPath_.size() >= kMaxCatalogRecords) return false;
    const auto idIt = pathById_.find(record.resourceId);
    if (idIt != pathById_.end() && idIt->second != path) return false;
    recordsByPath_[path] = std::move(record);
    pathById_[recordsByPath_[path].resourceId] = std::move(path);
    return true;
}

bool WorkspaceResourceCatalog::RebindPath(WorkspaceResourceId resourceId,
                                          WorkspaceResourceKind kind,
                                          std::string path,
                                          WorkspaceResourceFingerprint fingerprint) {
    if (!resourceId.valid() || !IsValidKind(kind) || !fingerprint.valid() || path.empty()) return false;
    const auto pathWide = Utf8ToWideStrict(path);
    if (!pathWide.has_value() || !IsSafeRelativePath(std::filesystem::path(*pathWide))) return false;
    const auto idIt = pathById_.find(resourceId);
    if (idIt == pathById_.end()) return false;
    const auto sourceIt = recordsByPath_.find(idIt->second);
    if (sourceIt == recordsByPath_.end() || sourceIt->second.kind != kind) return false;
    const auto destinationIt = recordsByPath_.find(path);
    if (destinationIt != recordsByPath_.end() && destinationIt->second.resourceId != resourceId) return false;
    if (destinationIt != recordsByPath_.end()) return false;
    CatalogRecord replacement{resourceId, kind, std::move(fingerprint)};
    recordsByPath_.erase(sourceIt);
    idIt->second = path;
    recordsByPath_[std::move(path)] = std::move(replacement);
    return true;
}

bool WorkspaceResourceCatalog::UpdateFingerprint(WorkspaceResourceId resourceId,
                                                 const WorkspaceResourceFingerprint& fingerprint) {
    if (!resourceId.valid() || !fingerprint.valid()) return false;
    const auto idIt = pathById_.find(resourceId);
    if (idIt == pathById_.end()) return false;
    const auto recordIt = recordsByPath_.find(idIt->second);
    if (recordIt == recordsByPath_.end()) return false;
    recordIt->second.fingerprint = fingerprint;
    return true;
}

bool ConfigureRuntimeWorkspaceResourceCatalog(const std::filesystem::path& workspaceRoot,
                                              std::wstring* outError) {
    if (outError) outError->clear();
    const std::filesystem::path normalizedRoot(
        NormalizePathKey(workspaceRoot.wstring()));
    if (workspaceRoot.empty() || normalizedRoot.empty() || !normalizedRoot.is_absolute()) {
        if (outError) *outError = L"workspace resource catalog requires an absolute workspace root";
        return false;
    }
    RuntimeCatalogState& state = RuntimeCatalog();
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.configured = false;
    state.dirty = false;
    state.workspaceRoot = normalizedRoot;
    state.storePath = state.workspaceRoot / L"__pdf_note_workspace__" / L"__settings__" /
                      L"workspace_resource_registry.bin";
    WorkspaceResourceCatalog catalog;
    // The catalog establishes resource identity, so following a reparse point
    // here would let a workspace silently borrow an unrelated catalog.  A
    // missing catalog is fine (it will be created by the first safe bind), but
    // an existing reparse point is a hard failure.
    bool catalogIsReparse = false;
    if (TryIsReparsePointNoFollow(state.storePath, catalogIsReparse) && catalogIsReparse) {
        if (outError) *outError = L"workspace resource catalog is a reparse point";
        return false;
    }
    if (RegularFileExistsWin32(state.storePath)) {
        std::string bytes;
        if (!ReadFileBytesWin32(state.storePath, bytes) || !catalog.Parse(bytes, outError)) return false;
    }
    state.catalog = std::move(catalog);
    state.configured = true;
    return true;
}

std::optional<WorkspaceResourceIdentity> ResolveRuntimeWorkspaceResourceIdentityPath(
    WorkspaceResourceKind kind,
    const std::wstring& absolutePath,
    std::wstring* outError) {
    if (outError) outError->clear();
    if (!IsValidKind(kind)) {
        if (outError) *outError = L"workspace resource kind is invalid";
        return std::nullopt;
    }
    WorkspaceResourceFingerprint observed;
    if (!CaptureFingerprint(std::filesystem::path(absolutePath), &observed, outError)) return std::nullopt;
    RuntimeCatalogState& state = RuntimeCatalog();
    const std::lock_guard<std::mutex> lock(state.mutex);
    const auto relative = RelativeKeyForPath(state, absolutePath);
    if (!relative.has_value()) {
        if (outError) *outError = L"workspace resource path is outside the configured workspace";
        return std::nullopt;
    }
    if (const auto existing = state.catalog.FindPath(*relative)) {
        if (existing->kind != kind) {
            if (outError) *outError = L"workspace resource alias has a different resource kind";
            return std::nullopt;
        }
        if (existing->fingerprint != observed) {
            WorkspaceResourceCatalog previous = state.catalog;
            if (!state.catalog.UpdateFingerprint(existing->resourceId, observed) ||
                !PersistLocked(state, outError)) {
                state.catalog = std::move(previous);
                return std::nullopt;
            }
        } else if (state.dirty && !PersistLocked(state, outError)) {
            return std::nullopt;
        }
        return WorkspaceResourceIdentity{existing->resourceId, kind, std::move(observed)};
    }
    WorkspaceResourceId resourceId;
    do {
        resourceId = CreateResourceId();
        if (!resourceId.valid()) {
            if (outError) *outError = L"workspace resource ID allocation failed";
            return std::nullopt;
        }
    } while (state.catalog.FindId(resourceId).has_value());
    WorkspaceResourceCatalog previous = state.catalog;
    if (!state.catalog.BindPath({resourceId, kind, observed}, *relative) ||
        !PersistLocked(state, outError)) {
        state.catalog = std::move(previous);
        return std::nullopt;
    }
    return WorkspaceResourceIdentity{resourceId, kind, std::move(observed)};
}

std::optional<WorkspaceResourceIdentity> FindRuntimeWorkspaceResourceIdentityPath(
    WorkspaceResourceKind kind,
    const std::wstring& absolutePath,
    std::wstring* outError) {
    if (outError) outError->clear();
    if (!IsValidKind(kind)) return std::nullopt;
    RuntimeCatalogState& state = RuntimeCatalog();
    const std::lock_guard<std::mutex> lock(state.mutex);
    const auto relative = RelativeKeyForPath(state, absolutePath);
    if (!relative.has_value()) {
        if (outError) *outError = L"workspace resource lookup path is outside the configured workspace";
        return std::nullopt;
    }
    const auto record = state.catalog.FindPath(*relative);
    if (!record.has_value() || record->kind != kind) return std::nullopt;
    return WorkspaceResourceIdentity{record->resourceId, record->kind, record->fingerprint};
}

bool RebindRuntimeWorkspaceResourceIdentityPath(
    WorkspaceResourceId resourceId,
    WorkspaceResourceKind kind,
    const std::wstring& oldAbsolutePath,
    const std::wstring& newAbsolutePath,
    const WorkspaceResourceFingerprint& expectedDestinationFingerprint,
    std::wstring* outError) {
    if (outError) outError->clear();
    if (!resourceId.valid() || !IsValidKind(kind) || !expectedDestinationFingerprint.valid()) return false;
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(oldAbsolutePath), ec) || ec) {
        if (outError) *outError = L"workspace resource source still exists during alias rebind";
        return false;
    }
    WorkspaceResourceFingerprint observed;
    if (!CaptureFingerprint(std::filesystem::path(newAbsolutePath), &observed, outError) ||
        observed != expectedDestinationFingerprint) {
        if (outError && outError->empty()) *outError = L"workspace resource destination fingerprint does not match";
        return false;
    }
    RuntimeCatalogState& state = RuntimeCatalog();
    const std::lock_guard<std::mutex> lock(state.mutex);
    const auto oldRelative = RelativeKeyForPath(state, oldAbsolutePath);
    const auto newRelative = RelativeKeyForPath(state, newAbsolutePath);
    if (!oldRelative.has_value() || !newRelative.has_value()) {
        if (outError) *outError = L"workspace resource rebind path is outside the configured workspace";
        return false;
    }
    const auto source = state.catalog.FindPath(*oldRelative);
    if (!source.has_value() || source->resourceId != resourceId || source->kind != kind ||
        state.catalog.FindPath(*newRelative).has_value()) {
        if (outError) *outError = L"workspace resource alias rebind precondition failed";
        return false;
    }
    WorkspaceResourceCatalog previous = state.catalog;
    if (!state.catalog.RebindPath(resourceId, kind, *newRelative, observed) ||
        !PersistLocked(state, outError)) {
        state.catalog = std::move(previous);
        return false;
    }
    return true;
}

std::filesystem::path RuntimeWorkspaceResourceCatalogPath() {
    RuntimeCatalogState& state = RuntimeCatalog();
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.storePath;
}
