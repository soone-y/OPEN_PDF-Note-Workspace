#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Stable identity for workspace resources whose path can be changed by a
// transaction.  This intentionally has no implicit conversion to a path or
// string: an alias is always looked up or rebound through the catalog.
struct WorkspaceResourceId {
    std::array<std::uint8_t, 16> value{};

    [[nodiscard]] bool valid() const;
    friend bool operator==(const WorkspaceResourceId& lhs, const WorkspaceResourceId& rhs) {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(const WorkspaceResourceId& lhs, const WorkspaceResourceId& rhs) {
        return !(lhs == rhs);
    }
    friend bool operator<(const WorkspaceResourceId& lhs, const WorkspaceResourceId& rhs) {
        return lhs.value < rhs.value;
    }
};

enum class WorkspaceResourceKind : std::uint8_t {
    Pdf = 1,
    LinkSet = 2,
};

struct WorkspaceResourceFingerprint {
    std::uint64_t size = 0;
    std::string sha256;

    [[nodiscard]] bool valid() const;
    friend bool operator==(const WorkspaceResourceFingerprint& lhs,
                           const WorkspaceResourceFingerprint& rhs) {
        return lhs.size == rhs.size && lhs.sha256 == rhs.sha256;
    }
    friend bool operator!=(const WorkspaceResourceFingerprint& lhs,
                           const WorkspaceResourceFingerprint& rhs) {
        return !(lhs == rhs);
    }
};

struct WorkspaceResourceIdentity {
    WorkspaceResourceId resource_id{};
    WorkspaceResourceKind kind = WorkspaceResourceKind::Pdf;
    WorkspaceResourceFingerprint fingerprint;
};

[[nodiscard]] std::string WorkspaceResourceIdToHex(WorkspaceResourceId resourceId);
[[nodiscard]] std::optional<WorkspaceResourceId> WorkspaceResourceIdFromHex(
    std::string_view encoded);

// Opens the workspace-local persistent catalog.  Parsing is strict and never
// attempts to infer a replacement catalog from filesystem paths.
[[nodiscard]] bool ConfigureRuntimeWorkspaceResourceCatalog(
    const std::filesystem::path& workspaceRoot,
    std::wstring* outError = nullptr);

// Registers an unbound local regular file only after capturing its full
// fingerprint. Existing aliases keep their ID; an observed fingerprint change
// is persisted before the caller is told it may use the resource.
[[nodiscard]] std::optional<WorkspaceResourceIdentity>
ResolveRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind kind,
                                            const std::wstring& absolutePath,
                                            std::wstring* outError = nullptr);

// Read-only alias lookup for recovery. It never allocates an ID, updates an
// observation, or writes the catalog.
[[nodiscard]] std::optional<WorkspaceResourceIdentity>
FindRuntimeWorkspaceResourceIdentityPath(WorkspaceResourceKind kind,
                                         const std::wstring& absolutePath,
                                         std::wstring* outError = nullptr);

// Applies the catalog half of a journaled alias move. The destination must
// still be a local regular file with the recorded postcondition fingerprint;
// no existing alias is overwritten.
[[nodiscard]] bool RebindRuntimeWorkspaceResourceIdentityPath(
    WorkspaceResourceId resourceId,
    WorkspaceResourceKind kind,
    const std::wstring& oldAbsolutePath,
    const std::wstring& newAbsolutePath,
    const WorkspaceResourceFingerprint& expectedDestinationFingerprint,
    std::wstring* outError = nullptr);

[[nodiscard]] std::filesystem::path RuntimeWorkspaceResourceCatalogPath();
