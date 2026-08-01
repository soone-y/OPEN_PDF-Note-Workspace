#pragma once

#include <windows.h>

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Text encoding is intentionally explicit at file boundaries.  App-managed
// files are UTF-8 only; user text files may retain the encoding they had when
// opened so an ordinary edit never performs a silent conversion.
namespace text_encoding {

enum class Encoding {
    Utf8,
    Utf8Bom,
    Utf16Le,
    Utf16Be,
    Cp932,
};

struct DecodedText {
    std::wstring text;
    Encoding encoding = Encoding::Utf8;
};

inline const wchar_t* Name(Encoding encoding) {
    switch (encoding) {
    case Encoding::Utf8: return L"UTF-8";
    case Encoding::Utf8Bom: return L"UTF-8 with BOM";
    case Encoding::Utf16Le: return L"UTF-16 LE";
    case Encoding::Utf16Be: return L"UTF-16 BE";
    case Encoding::Cp932: return L"CP932 (Shift_JIS)";
    }
    return L"unknown";
}

inline bool IsWellFormedUtf16(std::wstring_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (i + 1 >= text.size() || text[++i] < 0xDC00 || text[i] > 0xDFFF) return false;
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

inline std::optional<std::wstring> DecodeUtf8Strict(std::string_view bytes) {
    if (bytes.empty()) return std::wstring{};
    if (bytes.size() > static_cast<size_t>(INT_MAX)) return std::nullopt;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             bytes.data(), static_cast<int>(bytes.size()),
                                             nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring text(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            bytes.data(), static_cast<int>(bytes.size()),
                            text.data(), required) != required) {
        return std::nullopt;
    }
    return text;
}

inline bool DecodeUtf16(std::string_view bytes, bool littleEndian, std::wstring* out) {
    if (!out || (bytes.size() % 2) != 0) return false;
    out->clear();
    out->reserve(bytes.size() / 2);
    for (size_t i = 0; i < bytes.size(); i += 2) {
        const uint16_t lo = static_cast<unsigned char>(bytes[i]);
        const uint16_t hi = static_cast<unsigned char>(bytes[i + 1]);
        const uint16_t unit = littleEndian ? static_cast<uint16_t>(lo | (hi << 8))
                                           : static_cast<uint16_t>((lo << 8) | hi);
        out->push_back(static_cast<wchar_t>(unit));
    }
    return IsWellFormedUtf16(*out);
}

inline bool DecodeExternalText(std::string_view bytes, DecodedText* out, std::wstring* error = nullptr) {
    if (out) *out = {};
    if (error) error->clear();
    if (!out) return false;

    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        if (!DecodeUtf16(bytes.substr(2), true, &out->text)) {
            if (error) *error = L"UTF-16 LE の本文が不正です。";
            return false;
        }
        out->encoding = Encoding::Utf16Le;
        return true;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        if (!DecodeUtf16(bytes.substr(2), false, &out->text)) {
            if (error) *error = L"UTF-16 BE の本文が不正です。";
            return false;
        }
        out->encoding = Encoding::Utf16Be;
        return true;
    }
    const bool hasUtf8Bom = bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF;
    const std::string_view utf8Bytes = hasUtf8Bom ? bytes.substr(3) : bytes;
    if (const auto utf8 = DecodeUtf8Strict(utf8Bytes); utf8.has_value()) {
        out->text = *utf8;
        out->encoding = hasUtf8Bom ? Encoding::Utf8Bom : Encoding::Utf8;
        return true;
    }

    if (bytes.size() > static_cast<size_t>(INT_MAX)) {
        if (error) *error = L"テキストが大きすぎます。";
        return false;
    }
    const int required = MultiByteToWideChar(932, 0, bytes.data(),
                                             static_cast<int>(bytes.size()), nullptr, 0);
    if (required <= 0) {
        if (error) *error = L"UTF-8 または CP932 として解釈できません。";
        return false;
    }
    out->text.assign(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()),
                            out->text.data(), required) != required) {
        if (error) *error = L"CP932 の変換に失敗しました。";
        return false;
    }
    out->encoding = Encoding::Cp932;
    return true;
}

inline bool EncodeText(std::wstring_view text, Encoding encoding,
                       std::string* out, std::wstring* error = nullptr) {
    if (out) out->clear();
    if (error) error->clear();
    if (!out || !IsWellFormedUtf16(text) || text.size() > static_cast<size_t>(INT_MAX)) {
        if (error) *error = L"本文に不正な UTF-16 文字があります。";
        return false;
    }
    if (encoding == Encoding::Utf16Le || encoding == Encoding::Utf16Be) {
        out->reserve(2 + text.size() * 2);
        if (encoding == Encoding::Utf16Le) { out->push_back(static_cast<char>(0xFF)); out->push_back(static_cast<char>(0xFE)); }
        else { out->push_back(static_cast<char>(0xFE)); out->push_back(static_cast<char>(0xFF)); }
        for (wchar_t ch : text) {
            const uint16_t unit = static_cast<uint16_t>(ch);
            if (encoding == Encoding::Utf16Le) {
                out->push_back(static_cast<char>(unit & 0xFF)); out->push_back(static_cast<char>(unit >> 8));
            } else {
                out->push_back(static_cast<char>(unit >> 8)); out->push_back(static_cast<char>(unit & 0xFF));
            }
        }
        return true;
    }
    const UINT codePage = encoding == Encoding::Cp932 ? 932 : CP_UTF8;
    const DWORD flags = encoding == Encoding::Cp932 ? WC_NO_BEST_FIT_CHARS : WC_ERR_INVALID_CHARS;
    BOOL usedDefault = FALSE;
    const char defaultChar = '?';
    const int required = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0,
                                             encoding == Encoding::Cp932 ? &defaultChar : nullptr,
                                             encoding == Encoding::Cp932 ? &usedDefault : nullptr);
    if (required <= 0 || usedDefault) {
        if (error) *error = encoding == Encoding::Cp932
            ? L"CP932 で表現できない文字が含まれています。UTF-8 への明示的な変換が必要です。"
            : L"UTF-8 への変換に失敗しました。";
        return false;
    }
    out->assign(encoding == Encoding::Utf8Bom ? "\xEF\xBB\xBF" : "");
    const size_t prefix = out->size();
    out->resize(prefix + static_cast<size_t>(required));
    if (WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
                            out->data() + prefix, required,
                            encoding == Encoding::Cp932 ? &defaultChar : nullptr,
                            encoding == Encoding::Cp932 ? &usedDefault : nullptr) != required || usedDefault) {
        out->clear();
        if (error) *error = L"文字コード変換に失敗しました。";
        return false;
    }
    return true;
}

} // namespace text_encoding
