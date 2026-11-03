#include "core/text_encoding.h"

#include <cassert>
#include <string>
#include <utility>

int main() {
    using text_encoding::DecodedText;
    using text_encoding::Encoding;

    const std::string cp932Dummy{"\x83\x5F\x83\x7E\x81\x5B\x81\x42", 8};
    DecodedText decoded;
    assert(text_encoding::DecodeExternalText(cp932Dummy, &decoded));
    assert(decoded.encoding == Encoding::Cp932);
    assert(decoded.text == L"ダミー。");
    std::string encoded;
    assert(text_encoding::EncodeText(decoded.text, decoded.encoding, &encoded));
    assert(encoded == cp932Dummy);

    const std::string utf8Bom{"\xEF\xBB\xBF" "あ", 6};
    assert(text_encoding::DecodeExternalText(utf8Bom, &decoded));
    assert(decoded.encoding == Encoding::Utf8Bom);
    assert(decoded.text == L"あ");
    assert(text_encoding::EncodeText(decoded.text, decoded.encoding, &encoded));
    assert(encoded == utf8Bom);

    const std::string utf16Le{"\xFF\xFE\x41\x00\x42\x30", 6};
    assert(text_encoding::DecodeExternalText(utf16Le, &decoded));
    assert(decoded.encoding == Encoding::Utf16Le);
    assert(decoded.text == L"Aあ");
    assert(text_encoding::EncodeText(decoded.text, decoded.encoding, &encoded));
    assert(encoded == utf16Le);

    assert(!text_encoding::DecodeUtf8Strict(cp932Dummy).has_value());
    std::wstring error;
    text_encoding::ErrorCode errorCode = text_encoding::ErrorCode::None;
    assert(!text_encoding::EncodeText(L"emoji \U0001F642", Encoding::Cp932, &encoded, &error, &errorCode));
    assert(!error.empty());
    assert(errorCode == text_encoding::ErrorCode::Cp932Unrepresentable);
    assert(std::wstring_view(text_encoding::ErrorTextId(errorCode)) == L"text_encoding.cp932_unrepresentable");

    // An empty note remains valid after creation, clearing its text, or a prior
    // conversion failure. Preserve each selected encoding's BOM exactly.
    const std::pair<Encoding, std::string> emptyCases[] = {
        {Encoding::Utf8, ""}, {Encoding::Utf8Bom, "\xEF\xBB\xBF"},
        {Encoding::Cp932, ""}, {Encoding::Utf16Le, "\xFF\xFE"},
        {Encoding::Utf16Be, "\xFE\xFF"},
    };
    for (const auto& [encoding, expected] : emptyCases) {
        encoded = "previous content"; error = L"previous failure";
        assert(text_encoding::EncodeText(std::wstring_view{}, encoding, &encoded, &error));
        assert(encoded == expected && error.empty());
        assert(text_encoding::DecodeExternalText(encoded, &decoded));
        assert(decoded.text.empty());
        assert(text_encoding::EncodeText(L"次の本文", encoding, &encoded, &error));
        assert(text_encoding::EncodeText(L"", encoding, &encoded, &error));
        assert(encoded == expected && error.empty());
    }
    const std::wstring invalidUtf16(1, static_cast<wchar_t>(0xD800));
    assert(!text_encoding::EncodeText(invalidUtf16, Encoding::Utf8, &encoded, &error));
    assert(encoded.empty() && !error.empty());
    assert(text_encoding::EncodeText(L"", Encoding::Utf8, &encoded, &error));
    assert(encoded.empty() && error.empty());
    return 0;
}
