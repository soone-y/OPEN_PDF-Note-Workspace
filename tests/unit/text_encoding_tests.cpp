#include "core/text_encoding.h"

#include <cassert>
#include <string>

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
    assert(!text_encoding::EncodeText(L"emoji \U0001F642", Encoding::Cp932, &encoded, &error));
    assert(!error.empty());
    return 0;
}
