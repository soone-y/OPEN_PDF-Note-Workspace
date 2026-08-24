#include "localization.h"

#include "locale_catalog.generated.h"

namespace localization {

std::wstring Text(std::wstring_view id) {
    return generated_locale_catalog::Text(id);
}

std::wstring Format(std::wstring_view id,
                    const std::vector<std::pair<std::wstring_view, std::wstring>>& values) {
    std::wstring text = Text(id);
    for (const auto& [name, value] : values) {
        const std::wstring token = L"{" + std::wstring(name) + L"}";
        size_t offset = 0;
        while ((offset = text.find(token, offset)) != std::wstring::npos) {
            text.replace(offset, token.size(), value);
            offset += value.size();
        }
    }
    return text;
}

const UiText& UserInterfaceText() {
    static const UiText text = generated_locale_catalog::BuildUiText();
    return text;
}

bool IsEnglishBuild() {
#if PDF_NOTE_LOCALE_EN
    return true;
#else
    return false;
#endif
}

}  // namespace localization
