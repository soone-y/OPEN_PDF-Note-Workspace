#include "office/docx_space_protection.h"

#include "core/atomic_write.h"

#include <zlib.h>

#include <algorithm>
#include <cwchar>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <new>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace office {
namespace {

constexpr uint32_t kZipLocalFileHeaderSig = 0x04034b50u;
constexpr uint32_t kZipCentralDirectoryHeaderSig = 0x02014b50u;
constexpr uint32_t kZipEndOfCentralDirectorySig = 0x06054b50u;
constexpr uint16_t kZipMethodStored = 0;
constexpr uint16_t kZipMethodDeflated = 8;
constexpr uint16_t kZipFlagEncrypted = 0x0001u;
constexpr uint16_t kZipFlagDataDescriptor = 0x0008u;
constexpr uint64_t kMaxDocxBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaxWordXmlEntryBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxTotalWordXmlBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaxRelationshipPartBytes = 4ull * 1024ull * 1024ull;
constexpr uint64_t kMaxTotalRelationshipBytes = 16ull * 1024ull * 1024ull;

struct ZipEntry {
    uint16_t versionMadeBy = 0;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t diskStart = 0;
    uint16_t internalAttrs = 0;
    uint32_t externalAttrs = 0;
    uint32_t localHeaderOffset = 0;
    std::string name;
    std::vector<uint8_t> localExtra;
    std::vector<uint8_t> centralExtra;
    std::vector<uint8_t> comment;
    std::vector<uint8_t> compressedData;
};

static uint16_t ReadLe16(const std::vector<uint8_t>& data, size_t off) {
    return static_cast<uint16_t>(data[off]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[off + 1]) << 8);
}

static uint32_t ReadLe32(const std::vector<uint8_t>& data, size_t off) {
    return static_cast<uint32_t>(data[off]) |
           (static_cast<uint32_t>(data[off + 1]) << 8) |
           (static_cast<uint32_t>(data[off + 2]) << 16) |
           (static_cast<uint32_t>(data[off + 3]) << 24);
}

static void AppendLe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

static void AppendLe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

static std::wstring OfficeErr(const wchar_t* en, const wchar_t* ja) {
    (void)en;
    return ja;
}

static bool ReadFileBytes(const std::filesystem::path& path,
                          std::vector<uint8_t>* out,
                          std::wstring* outErr) {
    if (out) out->clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxDocxBytes) {
        if (outErr) *outErr = OfficeErr(L"Failed to read DOCX staging source.",
                                        L"DOCX変換用コピーの読み込みに失敗しました。");
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        if (outErr) *outErr = OfficeErr(L"Failed to open DOCX staging source.",
                                        L"DOCX変換用コピーを開けません。");
        return false;
    }
    out->resize(static_cast<size_t>(size));
    if (!out->empty()) {
        ifs.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
    }
    if (!ifs && static_cast<size_t>(ifs.gcount()) != out->size()) {
        if (outErr) *outErr = OfficeErr(L"Failed while reading DOCX staging source.",
                                        L"DOCX変換用コピーの読み込み中に失敗しました。");
        return false;
    }
    return true;
}

static bool WriteFileBytesAtomically(const std::filesystem::path& dest,
                                     const std::vector<uint8_t>& data,
                                     std::wstring* outErr) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        if (outErr) *outErr = OfficeErr(L"Failed to create DOCX staging folder.",
                                        L"DOCX変換用フォルダを作成できません。");
        return false;
    }

    std::filesystem::path tmp;
    HANDLE tmpHandle = INVALID_HANDLE_VALUE;
    if (!atomic_write::CreateUniqueTempFile(dest, dest.parent_path(), &tmp, &tmpHandle, outErr)) {
        return false;
    }
    auto cleanup = [&]() {
        if (tmpHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(tmpHandle);
            tmpHandle = INVALID_HANDLE_VALUE;
        }
        if (!tmp.empty()) {
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
        }
    };

    size_t written = 0;
    while (written < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - written, 1024u * 1024u));
        DWORD got = 0;
        if (!WriteFile(tmpHandle, data.data() + written, chunk, &got, nullptr) || got != chunk) {
            cleanup();
            if (outErr) *outErr = OfficeErr(L"Failed to write DOCX staging copy.",
                                            L"DOCX変換用コピーを書き込めません。");
            return false;
        }
        written += got;
    }
    if (!FlushFileBuffers(tmpHandle)) {
        cleanup();
        if (outErr) *outErr = OfficeErr(L"Failed to flush DOCX staging copy.",
                                        L"DOCX変換用コピーを確定できません。");
        return false;
    }
    CloseHandle(tmpHandle);
    tmpHandle = INVALID_HANDLE_VALUE;

    if (!atomic_write::AtomicReplaceFile(dest, tmp, dest.parent_path(), outErr)) {
        cleanup();
        return false;
    }
    tmp.clear();
    return true;
}

static std::optional<size_t> FindEndOfCentralDirectory(const std::vector<uint8_t>& data) {
    if (data.size() < 22) return std::nullopt;
    const size_t maxBack = std::min<size_t>(data.size() - 22, 0xFFFFu);
    for (size_t back = 0; back <= maxBack; ++back) {
        const size_t off = data.size() - 22 - back;
        if (ReadLe32(data, off) != kZipEndOfCentralDirectorySig) continue;
        const uint16_t commentLen = ReadLe16(data, off + 20);
        if (off + 22u + commentLen == data.size()) return off;
    }
    return std::nullopt;
}

static bool ParseZipEntries(const std::vector<uint8_t>& data,
                            std::vector<ZipEntry>* entries,
                            std::vector<uint8_t>* eocdComment,
                            std::wstring* outErr) {
    if (entries) entries->clear();
    if (eocdComment) eocdComment->clear();
    const auto eocdOff = FindEndOfCentralDirectory(data);
    if (!eocdOff) {
        if (outErr) *outErr = OfficeErr(L"DOCX ZIP directory was not found.",
                                        L"DOCX ZIPディレクトリが見つかりません。");
        return false;
    }
    if (ReadLe16(data, *eocdOff + 4) != 0 || ReadLe16(data, *eocdOff + 6) != 0) {
        if (outErr) *outErr = OfficeErr(L"Multi-disk DOCX ZIP is not supported.",
                                        L"分割DOCX ZIPには対応していません。");
        return false;
    }
    const uint16_t diskEntries = ReadLe16(data, *eocdOff + 8);
    const uint16_t totalEntries = ReadLe16(data, *eocdOff + 10);
    const uint32_t centralSize = ReadLe32(data, *eocdOff + 12);
    const uint32_t centralOffset = ReadLe32(data, *eocdOff + 16);
    const uint16_t commentLen = ReadLe16(data, *eocdOff + 20);
    if (diskEntries != totalEntries ||
        totalEntries == 0xFFFFu ||
        centralSize == 0xFFFFFFFFu ||
        centralOffset == 0xFFFFFFFFu ||
        static_cast<uint64_t>(centralOffset) + centralSize > data.size() ||
        *eocdOff + 22u + commentLen > data.size()) {
        if (outErr) *outErr = OfficeErr(L"Unsupported DOCX ZIP layout.",
                                        L"未対応のDOCX ZIP構造です。");
        return false;
    }

    size_t off = centralOffset;
    for (uint16_t i = 0; i < totalEntries; ++i) {
        if (off + 46u > data.size() || ReadLe32(data, off) != kZipCentralDirectoryHeaderSig) {
            if (outErr) *outErr = OfficeErr(L"Invalid DOCX ZIP central directory.",
                                            L"DOCX ZIP中央ディレクトリが不正です。");
            return false;
        }

        ZipEntry entry;
        entry.versionMadeBy = ReadLe16(data, off + 4);
        entry.versionNeeded = ReadLe16(data, off + 6);
        entry.flags = ReadLe16(data, off + 8);
        entry.method = ReadLe16(data, off + 10);
        entry.modTime = ReadLe16(data, off + 12);
        entry.modDate = ReadLe16(data, off + 14);
        entry.crc32 = ReadLe32(data, off + 16);
        entry.compressedSize = ReadLe32(data, off + 20);
        entry.uncompressedSize = ReadLe32(data, off + 24);
        const uint16_t nameLen = ReadLe16(data, off + 28);
        const uint16_t extraLen = ReadLe16(data, off + 30);
        const uint16_t fileCommentLen = ReadLe16(data, off + 32);
        entry.diskStart = ReadLe16(data, off + 34);
        entry.internalAttrs = ReadLe16(data, off + 36);
        entry.externalAttrs = ReadLe32(data, off + 38);
        entry.localHeaderOffset = ReadLe32(data, off + 42);
        const size_t variableOff = off + 46u;
        const size_t next = variableOff + nameLen + extraLen + fileCommentLen;
        if (next > data.size() ||
            entry.localHeaderOffset == 0xFFFFFFFFu ||
            entry.compressedSize == 0xFFFFFFFFu ||
            entry.uncompressedSize == 0xFFFFFFFFu ||
            entry.diskStart != 0 ||
            (entry.flags & kZipFlagEncrypted) != 0) {
            if (outErr) *outErr = OfficeErr(L"Unsupported DOCX ZIP entry.",
                                            L"未対応のDOCX ZIP項目です。");
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(data.data() + variableOff), nameLen);
        entry.centralExtra.assign(data.begin() + static_cast<std::ptrdiff_t>(variableOff + nameLen),
                                  data.begin() + static_cast<std::ptrdiff_t>(variableOff + nameLen + extraLen));
        entry.comment.assign(data.begin() + static_cast<std::ptrdiff_t>(variableOff + nameLen + extraLen),
                             data.begin() + static_cast<std::ptrdiff_t>(next));

        const size_t localOff = entry.localHeaderOffset;
        if (localOff + 30u > data.size() || ReadLe32(data, localOff) != kZipLocalFileHeaderSig) {
            if (outErr) *outErr = OfficeErr(L"Invalid DOCX ZIP local header.",
                                            L"DOCX ZIPローカルヘッダーが不正です。");
            return false;
        }
        const uint16_t localNameLen = ReadLe16(data, localOff + 26);
        const uint16_t localExtraLen = ReadLe16(data, localOff + 28);
        const size_t dataOff = localOff + 30u + localNameLen + localExtraLen;
        const size_t dataEnd = dataOff + entry.compressedSize;
        if (dataEnd > data.size()) {
            if (outErr) *outErr = OfficeErr(L"Invalid DOCX ZIP entry size.",
                                            L"DOCX ZIP項目サイズが不正です。");
            return false;
        }
        entry.localExtra.assign(data.begin() + static_cast<std::ptrdiff_t>(localOff + 30u + localNameLen),
                                data.begin() + static_cast<std::ptrdiff_t>(dataOff));
        entry.compressedData.assign(data.begin() + static_cast<std::ptrdiff_t>(dataOff),
                                    data.begin() + static_cast<std::ptrdiff_t>(dataEnd));
        entries->push_back(std::move(entry));
        off = next;
    }

    eocdComment->assign(data.begin() + static_cast<std::ptrdiff_t>(*eocdOff + 22u),
                        data.begin() + static_cast<std::ptrdiff_t>(*eocdOff + 22u + commentLen));
    return true;
}

static bool InflateRawDeflate(const std::vector<uint8_t>& compressed,
                              uint32_t expectedSize,
                              std::vector<uint8_t>* out,
                              std::wstring* outErr) {
    out->assign(expectedSize, 0);
    z_stream stream{};
    int zret = inflateInit2(&stream, -MAX_WBITS);
    if (zret != Z_OK) {
        if (outErr) *outErr = OfficeErr(L"Failed to initialize DOCX inflater.",
                                        L"DOCX展開処理を初期化できません。");
        return false;
    }
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(out->data());
    stream.avail_out = static_cast<uInt>(out->size());
    zret = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (zret != Z_STREAM_END || stream.total_out != expectedSize) {
        if (outErr) *outErr = OfficeErr(L"Failed to inflate DOCX document XML.",
                                        L"DOCX本文XMLを展開できません。");
        return false;
    }
    return true;
}

static bool DecodeEntryData(const ZipEntry& entry,
                            std::vector<uint8_t>* out,
                            std::wstring* outErr) {
    if (entry.method == kZipMethodStored) {
        *out = entry.compressedData;
        if (out->size() != entry.uncompressedSize) {
            if (outErr) *outErr = OfficeErr(L"Invalid stored DOCX XML size.",
                                            L"DOCX本文XMLの格納サイズが不正です。");
            return false;
        }
        return true;
    }
    if (entry.method == kZipMethodDeflated) {
        return InflateRawDeflate(entry.compressedData, entry.uncompressedSize, out, outErr);
    }
    if (outErr) *outErr = OfficeErr(L"Unsupported DOCX document XML compression.",
                                    L"DOCX本文XMLの圧縮方式に対応していません。");
    return false;
}

static bool DecodeUtf8One(std::string_view text, size_t* pos, uint32_t* cp, std::string* bytes) {
    const unsigned char b0 = static_cast<unsigned char>(text[*pos]);
    size_t len = 1;
    uint32_t value = b0;
    if ((b0 & 0x80u) == 0) {
        len = 1;
        value = b0;
    } else if ((b0 & 0xE0u) == 0xC0u) {
        len = 2;
        value = b0 & 0x1Fu;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        len = 3;
        value = b0 & 0x0Fu;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        len = 4;
        value = b0 & 0x07u;
    } else {
        bytes->assign(1, static_cast<char>(b0));
        *cp = b0;
        ++(*pos);
        return false;
    }
    if (*pos + len > text.size()) {
        bytes->assign(1, static_cast<char>(b0));
        *cp = b0;
        ++(*pos);
        return false;
    }
    for (size_t i = 1; i < len; ++i) {
        const unsigned char bx = static_cast<unsigned char>(text[*pos + i]);
        if ((bx & 0xC0u) != 0x80u) {
            bytes->assign(1, static_cast<char>(b0));
            *cp = b0;
            ++(*pos);
            return false;
        }
        value = (value << 6) | (bx & 0x3Fu);
    }
    bytes->assign(text.substr(*pos, len));
    *cp = value;
    *pos += len;
    return true;
}

static bool IsJapaneseSpaceTokenChar(uint32_t cp) {
    return (0x3040u <= cp && cp <= 0x30FFu) ||
           (0x3400u <= cp && cp <= 0x9FFFu) ||
           (0xFF10u <= cp && cp <= 0xFF19u) ||
           (0x0041u <= cp && cp <= 0x005Au) ||
           (0x0061u <= cp && cp <= 0x007Au) ||
           (0x0030u <= cp && cp <= 0x0039u) ||
           (0x2010u <= cp && cp <= 0x2015u);
}

static bool IsTokenWhitespace(uint32_t cp) {
    return cp == 0x09u || cp == 0x0Au || cp == 0x0Bu || cp == 0x0Cu ||
           cp == 0x0Du || cp == 0x20u || cp == 0x85u || cp == 0xA0u ||
           cp == 0x1680u || (0x2000u <= cp && cp <= 0x200Au) ||
           cp == 0x2028u || cp == 0x2029u || cp == 0x202Fu ||
           cp == 0x205Fu || cp == 0x3000u;
}

static bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

static bool EndsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

static std::string AsciiLower(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        if ('A' <= ch && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

static bool IsAsciiXmlSpace(char ch);

static bool HasExternalRelationshipTarget(std::string_view xml) {
    const std::string lower = AsciiLower(xml);
    size_t search = 0;
    while ((search = lower.find("targetmode", search)) != std::string::npos) {
        size_t cursor = search + std::string_view("targetmode").size();
        while (cursor < lower.size() && IsAsciiXmlSpace(lower[cursor])) ++cursor;
        if (cursor >= lower.size() || lower[cursor] != '=') {
            search = cursor;
            continue;
        }
        ++cursor;
        while (cursor < lower.size() && IsAsciiXmlSpace(lower[cursor])) ++cursor;
        if (cursor >= lower.size() || (lower[cursor] != '"' && lower[cursor] != '\'')) {
            search = cursor;
            continue;
        }
        const char quote = lower[cursor++];
        const size_t end = lower.find(quote, cursor);
        if (end == std::string::npos) return true;
        if (lower.substr(cursor, end - cursor) == "external") return true;
        search = end + 1;
    }
    return false;
}

static bool ValidateOfficePackageBytesForOfflineConversion(
    const std::vector<uint8_t>& sourceBytes,
    std::wstring* outErr) {
    std::vector<ZipEntry> entries;
    std::vector<uint8_t> eocdComment;
    if (!ParseZipEntries(sourceBytes, &entries, &eocdComment, outErr)) return false;

    uint64_t totalRelationshipBytes = 0;
    for (const auto& entry : entries) {
        const std::string lowerName = AsciiLower(entry.name);
        if (lowerName.find("vbaproject.bin") != std::string::npos ||
            lowerName.find("/activex/") != std::string::npos ||
            lowerName.rfind("activex/", 0) == 0) {
            if (outErr) *outErr = OfficeErr(
                L"The Office package contains active macro or ActiveX content.",
                L"OfficeファイルにマクロまたはActiveXの実行可能コンテンツが含まれています。");
            return false;
        }
        if (!EndsWith(lowerName, ".rels")) continue;
        if (entry.uncompressedSize > kMaxRelationshipPartBytes ||
            totalRelationshipBytes > kMaxTotalRelationshipBytes - entry.uncompressedSize) {
            if (outErr) *outErr = OfficeErr(
                L"Office relationship metadata exceeds the safe size limit.",
                L"Officeファイルの参照関係メタデータが安全なサイズ上限を超えています。");
            return false;
        }
        totalRelationshipBytes += entry.uncompressedSize;
        std::vector<uint8_t> relationshipBytes;
        if (!DecodeEntryData(entry, &relationshipBytes, outErr)) return false;
        const std::string_view relationshipXml(
            reinterpret_cast<const char*>(relationshipBytes.data()), relationshipBytes.size());
        if (HasExternalRelationshipTarget(relationshipXml)) {
            if (outErr) *outErr = OfficeErr(
                L"The Office package contains an external relationship and was not opened.",
                L"Officeファイルに外部参照が含まれるため、ファイルを開きませんでした。");
            return false;
        }
    }
    return true;
}

static std::wstring Utf8ToWideLossy(std::string_view value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr,
                                           0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            value.data(),
                                            static_cast<int>(value.size()),
                                            out.data(),
                                            needed);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

static int CALLBACK FontFamilyFoundProc(const LOGFONTW*,
                                        const TEXTMETRICW*,
                                        DWORD,
                                        LPARAM lParam) {
    bool* found = reinterpret_cast<bool*>(lParam);
    *found = true;
    return 0;
}

static bool IsSystemFontFamilyAvailable(std::string_view fontName) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, bool> cache;

    const std::string key = AsciiLower(fontName);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    bool found = false;
    std::wstring wideName = Utf8ToWideLossy(fontName);
    if (!wideName.empty() && wideName.size() < LF_FACESIZE) {
        HDC hdc = GetDC(nullptr);
        if (hdc) {
            LOGFONTW lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            wcsncpy_s(lf.lfFaceName, wideName.c_str(), _TRUNCATE);
            EnumFontFamiliesExW(hdc, &lf, FontFamilyFoundProc, reinterpret_cast<LPARAM>(&found), 0);
            ReleaseDC(nullptr, hdc);
        }
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.emplace(key, found);
    return found;
}

static std::optional<std::string_view> LibreOfficeMetricFontSubstitute(std::string_view fontName) {
    struct FontMap {
        std::string_view source;
        std::string_view substitute;
    };
    static constexpr FontMap kMetricSubstitutes[] = {
        {"arial", "Liberation Sans"},
        {"arial narrow", "Liberation Sans Narrow"},
        {"aptos", "Carlito"},
        {"aptos display", "Carlito"},
        {"aptos narrow", "Liberation Sans Narrow"},
        {"aptos mono", "Liberation Mono"},
        {"aptos serif", "Liberation Serif"},
        {"calibri", "Carlito"},
        {"calibri light", "Carlito"},
        {"cambria", "Caladea"},
        {"cambria math", "Caladea"},
        {"courier new", "Liberation Mono"},
        {"times new roman", "Liberation Serif"},
    };
    if (IsSystemFontFamilyAvailable(fontName)) return std::nullopt;
    const std::string lowered = AsciiLower(fontName);
    for (const FontMap& candidate : kMetricSubstitutes) {
        if (lowered == candidate.source) return candidate.substitute;
    }
    return std::nullopt;
}

static bool IsAsciiXmlNameChar(char ch) {
    return ('A' <= ch && ch <= 'Z') ||
           ('a' <= ch && ch <= 'z') ||
           ('0' <= ch && ch <= '9') ||
           ch == '_' ||
           ch == '-' ||
           ch == ':' ||
           ch == '.';
}

static bool IsAsciiXmlSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static bool IsDocxFontAttribute(std::string_view attrName) {
    return attrName == "w:ascii" ||
           attrName == "w:hAnsi" ||
           attrName == "w:eastAsia" ||
           attrName == "w:cs" ||
           attrName == "w:name" ||
           attrName == "typeface";
}

struct Utf8TokenChar {
    uint32_t cp = 0;
    std::string bytes;
};

} // namespace

std::string ProtectJapaneseTokensAfterIdeographicSpaceUtf8(const std::string& text) {
    constexpr const char* kIdeographicSpace = "\xE3\x80\x80";
    constexpr const char* kWordJoiner = "\xE2\x81\xA0";

    std::string out;
    out.reserve(text.size() + 16);
    size_t pos = 0;
    while (pos < text.size()) {
        uint32_t cp = 0;
        std::string bytes;
        DecodeUtf8One(text, &pos, &cp, &bytes);
        out += bytes;
        if (cp != 0x3000u) continue;

        std::vector<Utf8TokenChar> token;
        while (pos < text.size()) {
            size_t probe = pos;
            uint32_t tokenCp = 0;
            std::string tokenBytes;
            DecodeUtf8One(text, &probe, &tokenCp, &tokenBytes);
            if (tokenCp == 0x3000u || IsTokenWhitespace(tokenCp)) break;
            token.push_back(Utf8TokenChar{tokenCp, std::move(tokenBytes)});
            pos = probe;
        }
        for (size_t i = 0; i < token.size(); ++i) {
            out += token[i].bytes;
            if (i + 1 < token.size() &&
                IsJapaneseSpaceTokenChar(token[i].cp) &&
                IsJapaneseSpaceTokenChar(token[i + 1].cp)) {
                out += kWordJoiner;
            }
        }
    }
    (void)kIdeographicSpace;
    return out;
}

static std::string ProtectDocxDocumentXmlTextRuns(const std::string& xml) {
    static const std::regex textRunRe("(<w:t[^>]*>)([\\s\\S]*?)(</w:t>)");
    std::string out;
    out.reserve(xml.size() + 32);
    auto begin = std::sregex_iterator(xml.begin(), xml.end(), textRunRe);
    auto end = std::sregex_iterator();
    size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const std::smatch& m = *it;
        const size_t matchPos = static_cast<size_t>(m.position());
        out.append(xml, last, matchPos - last);
        out += m.str(1);
        out += ProtectJapaneseTokensAfterIdeographicSpaceUtf8(m.str(2));
        out += m.str(3);
        last = matchPos + static_cast<size_t>(m.length());
    }
    out.append(xml, last, std::string::npos);
    return out;
}

static std::string SubstituteDocxFontAttributes(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    size_t last = 0;
    size_t search = 0;
    while (search < xml.size()) {
        const size_t equals = xml.find('=', search);
        if (equals == std::string::npos) break;

        size_t attrEnd = equals;
        while (attrEnd > 0 && IsAsciiXmlSpace(xml[attrEnd - 1])) --attrEnd;
        size_t attrStart = attrEnd;
        while (attrStart > 0 && IsAsciiXmlNameChar(xml[attrStart - 1])) --attrStart;
        if (attrStart == attrEnd) {
            search = equals + 1;
            continue;
        }

        const std::string_view attrName(xml.data() + attrStart, attrEnd - attrStart);
        if (!IsDocxFontAttribute(attrName)) {
            search = equals + 1;
            continue;
        }

        size_t quote = equals + 1;
        while (quote < xml.size() && IsAsciiXmlSpace(xml[quote])) ++quote;
        if (quote >= xml.size() || xml[quote] != '"') {
            search = equals + 1;
            continue;
        }
        const size_t valueStart = quote + 1;
        const size_t valueEnd = xml.find('"', valueStart);
        if (valueEnd == std::string::npos) break;

        const std::string_view fontName(xml.data() + valueStart, valueEnd - valueStart);
        if (const auto substitute = LibreOfficeMetricFontSubstitute(fontName)) {
            out.append(xml, last, valueStart - last);
            out.append(substitute->begin(), substitute->end());
            last = valueEnd;
        }
        search = valueEnd + 1;
    }
    out.append(xml, last, std::string::npos);
    return out;
}

static bool IsDocxWordXmlEntry(std::string_view name) {
    return StartsWith(name, "word/") && EndsWith(name, ".xml");
}

static std::string TransformDocxWordXmlForConversion(std::string_view name, const std::string& xml) {
    std::string transformed = SubstituteDocxFontAttributes(xml);
    if (name == "word/document.xml") {
        transformed = ProtectDocxDocumentXmlTextRuns(transformed);
    }
    return transformed;
}

static void ReplaceEntryWithStoredData(ZipEntry* entry, const std::string& data) {
    entry->method = kZipMethodStored;
    entry->flags = static_cast<uint16_t>(entry->flags & ~kZipFlagDataDescriptor);
    entry->compressedData.assign(data.begin(), data.end());
    entry->compressedSize = static_cast<uint32_t>(entry->compressedData.size());
    entry->uncompressedSize = static_cast<uint32_t>(entry->compressedData.size());
    const Bytef* crcData = entry->compressedData.empty()
        ? Z_NULL
        : reinterpret_cast<const Bytef*>(entry->compressedData.data());
    entry->crc32 = static_cast<uint32_t>(crc32(0, crcData, static_cast<uInt>(entry->compressedData.size())));
}

static std::vector<uint8_t> BuildLocalHeader(const ZipEntry& entry, uint32_t localHeaderOffset) {
    (void)localHeaderOffset;
    std::vector<uint8_t> out;
    out.reserve(30u + entry.name.size() + entry.localExtra.size());
    AppendLe32(out, kZipLocalFileHeaderSig);
    AppendLe16(out, entry.versionNeeded);
    AppendLe16(out, static_cast<uint16_t>(entry.flags & ~kZipFlagDataDescriptor));
    AppendLe16(out, entry.method);
    AppendLe16(out, entry.modTime);
    AppendLe16(out, entry.modDate);
    AppendLe32(out, entry.crc32);
    AppendLe32(out, entry.compressedSize);
    AppendLe32(out, entry.uncompressedSize);
    AppendLe16(out, static_cast<uint16_t>(entry.name.size()));
    AppendLe16(out, static_cast<uint16_t>(entry.localExtra.size()));
    out.insert(out.end(), entry.name.begin(), entry.name.end());
    out.insert(out.end(), entry.localExtra.begin(), entry.localExtra.end());
    return out;
}

static std::vector<uint8_t> BuildCentralHeader(const ZipEntry& entry, uint32_t localHeaderOffset) {
    std::vector<uint8_t> out;
    out.reserve(46u + entry.name.size() + entry.centralExtra.size() + entry.comment.size());
    AppendLe32(out, kZipCentralDirectoryHeaderSig);
    AppendLe16(out, entry.versionMadeBy);
    AppendLe16(out, entry.versionNeeded);
    AppendLe16(out, static_cast<uint16_t>(entry.flags & ~kZipFlagDataDescriptor));
    AppendLe16(out, entry.method);
    AppendLe16(out, entry.modTime);
    AppendLe16(out, entry.modDate);
    AppendLe32(out, entry.crc32);
    AppendLe32(out, entry.compressedSize);
    AppendLe32(out, entry.uncompressedSize);
    AppendLe16(out, static_cast<uint16_t>(entry.name.size()));
    AppendLe16(out, static_cast<uint16_t>(entry.centralExtra.size()));
    AppendLe16(out, static_cast<uint16_t>(entry.comment.size()));
    AppendLe16(out, 0);
    AppendLe16(out, entry.internalAttrs);
    AppendLe32(out, entry.externalAttrs);
    AppendLe32(out, localHeaderOffset);
    out.insert(out.end(), entry.name.begin(), entry.name.end());
    out.insert(out.end(), entry.centralExtra.begin(), entry.centralExtra.end());
    out.insert(out.end(), entry.comment.begin(), entry.comment.end());
    return out;
}

static std::vector<uint8_t> BuildEndOfCentralDirectory(uint16_t entryCount,
                                                       uint32_t centralSize,
                                                       uint32_t centralOffset,
                                                       const std::vector<uint8_t>& comment) {
    std::vector<uint8_t> out;
    out.reserve(22u + comment.size());
    AppendLe32(out, kZipEndOfCentralDirectorySig);
    AppendLe16(out, 0);
    AppendLe16(out, 0);
    AppendLe16(out, entryCount);
    AppendLe16(out, entryCount);
    AppendLe32(out, centralSize);
    AppendLe32(out, centralOffset);
    AppendLe16(out, static_cast<uint16_t>(comment.size()));
    out.insert(out.end(), comment.begin(), comment.end());
    return out;
}

static bool RewriteDocxWithConversionXmlTransforms(const std::vector<ZipEntry>& sourceEntries,
                                                   const std::vector<uint8_t>& eocdComment,
                                                   std::vector<uint8_t>* out,
                                                   std::wstring* outErr) {
    std::vector<ZipEntry> entries = sourceEntries;
    bool foundDocumentXml = false;
    uint64_t totalWordXmlBytes = 0;
    for (auto& entry : entries) {
        if (entry.name == "word/document.xml") foundDocumentXml = true;
        if (!IsDocxWordXmlEntry(entry.name)) continue;
        if (entry.uncompressedSize > kMaxWordXmlEntryBytes ||
            totalWordXmlBytes > kMaxTotalWordXmlBytes - entry.uncompressedSize) {
            if (outErr) *outErr = OfficeErr(
                L"DOCX Word XML exceeds the safe expansion limit.",
                L"DOCX内のWord XMLが安全な展開サイズ上限を超えています。");
            return false;
        }
        totalWordXmlBytes += entry.uncompressedSize;
        std::vector<uint8_t> xmlBytes;
        if (!DecodeEntryData(entry, &xmlBytes, outErr)) return false;
        std::string xml(reinterpret_cast<const char*>(xmlBytes.data()), xmlBytes.size());
        std::string transformedXml = TransformDocxWordXmlForConversion(entry.name, xml);
        if (transformedXml != xml) {
            ReplaceEntryWithStoredData(&entry, transformedXml);
        }
    }
    if (!foundDocumentXml) {
        if (outErr) *outErr = OfficeErr(L"DOCX document XML was not found.",
                                        L"DOCX本文XMLが見つかりません。");
        return false;
    }

    std::vector<uint32_t> offsets;
    offsets.reserve(entries.size());
    out->clear();
    for (const auto& entry : entries) {
        if (out->size() > 0xFFFFFFFFull) {
            if (outErr) *outErr = OfficeErr(L"DOCX ZIP64 output is not supported.",
                                            L"DOCX ZIP64出力には対応していません。");
            return false;
        }
        offsets.push_back(static_cast<uint32_t>(out->size()));
        std::vector<uint8_t> local = BuildLocalHeader(entry, offsets.back());
        out->insert(out->end(), local.begin(), local.end());
        out->insert(out->end(), entry.compressedData.begin(), entry.compressedData.end());
    }

    if (out->size() > 0xFFFFFFFFull) {
        if (outErr) *outErr = OfficeErr(L"DOCX ZIP64 output is not supported.",
                                        L"DOCX ZIP64出力には対応していません。");
        return false;
    }
    const uint32_t centralOffset = static_cast<uint32_t>(out->size());
    uint64_t centralSize = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::vector<uint8_t> central = BuildCentralHeader(entries[i], offsets[i]);
        centralSize += central.size();
        if (centralSize > 0xFFFFFFFFull) {
            if (outErr) *outErr = OfficeErr(L"DOCX ZIP64 output is not supported.",
                                            L"DOCX ZIP64出力には対応していません。");
            return false;
        }
        out->insert(out->end(), central.begin(), central.end());
    }
    std::vector<uint8_t> eocd = BuildEndOfCentralDirectory(
        static_cast<uint16_t>(entries.size()),
        static_cast<uint32_t>(centralSize),
        centralOffset,
        eocdComment);
    out->insert(out->end(), eocd.begin(), eocd.end());
    return true;
}

bool ValidateOfficePackageForOfflineConversion(const std::filesystem::path& source,
                                               std::wstring* outErr) {
    if (outErr) outErr->clear();
    try {
        std::vector<uint8_t> sourceBytes;
        if (!ReadFileBytes(source, &sourceBytes, outErr)) return false;
        return ValidateOfficePackageBytesForOfflineConversion(sourceBytes, outErr);
    } catch (const std::bad_alloc&) {
        if (outErr) *outErr = OfficeErr(
            L"Not enough memory to validate the Office package.",
            L"Officeファイルを検証するためのメモリが不足しています。");
        return false;
    } catch (const std::length_error&) {
        if (outErr) *outErr = OfficeErr(
            L"Office package metadata exceeds the supported size.",
            L"Officeファイルのメタデータが対応可能なサイズを超えています。");
        return false;
    }
}

bool TransformDocxForSpaceProtection(const std::filesystem::path& source,
                                     const std::filesystem::path& dest,
                                     std::wstring* outErr) {
    if (outErr) outErr->clear();
    try {
        std::vector<uint8_t> sourceBytes;
        if (!ReadFileBytes(source, &sourceBytes, outErr)) return false;
        if (!ValidateOfficePackageBytesForOfflineConversion(sourceBytes, outErr)) return false;

        std::vector<ZipEntry> entries;
        std::vector<uint8_t> eocdComment;
        if (!ParseZipEntries(sourceBytes, &entries, &eocdComment, outErr)) return false;

        std::vector<uint8_t> outputBytes;
        if (!RewriteDocxWithConversionXmlTransforms(entries, eocdComment, &outputBytes, outErr)) return false;

        return WriteFileBytesAtomically(dest, outputBytes, outErr);
    } catch (const std::bad_alloc&) {
        if (outErr) *outErr = OfficeErr(
            L"Not enough memory to prepare the DOCX conversion copy.",
            L"DOCX変換用コピーを準備するためのメモリが不足しています。");
        return false;
    } catch (const std::length_error&) {
        if (outErr) *outErr = OfficeErr(
            L"DOCX conversion data exceeds the supported size.",
            L"DOCX変換データが対応可能なサイズを超えています。");
        return false;
    }
}

} // namespace office
