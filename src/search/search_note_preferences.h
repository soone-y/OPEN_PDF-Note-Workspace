#pragma once

#include "core/workspace_config.h"
#include "search/search_wire.h"

namespace search {
// Parent and worker use the same field order. A rejected/short record leaves
// the destination untouched, so partial preferences cannot escape the reader.
inline void WriteNoteSyntaxPreferences(Writer& writer, const WorkspaceConfig& config) {
    writer.Put<uint8_t>(config.clroAsciiOnly);
    writer.Put<uint8_t>(config.noteQuoteLazyContinuation);
    writer.Put<uint8_t>(config.clroListImplicitContinuation);
}

inline void ReadNoteSyntaxPreferences(Reader& reader, WorkspaceConfig& config) {
    const auto asciiOnly = reader.Get<uint8_t>();
    const auto quoteContinuation = reader.Get<uint8_t>();
    const auto listContinuation = reader.Get<uint8_t>();
    if (asciiOnly > 1 || quoteContinuation > 1 || listContinuation > 1) {
        throw std::runtime_error("invalid note syntax preferences");
    }
    config.clroAsciiOnly = asciiOnly != 0;
    config.noteQuoteLazyContinuation = quoteContinuation != 0;
    config.clroListImplicitContinuation = listContinuation != 0;
}
} // namespace search
