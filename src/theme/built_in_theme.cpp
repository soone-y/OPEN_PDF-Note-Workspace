#include "theme/built_in_theme.h"

namespace theme {
namespace {

ThemeColors MakeTheme(const wchar_t* name,
                      const wchar_t* nameJp,
                      COLORREF windowBg, COLORREF windowText,
                      COLORREF panelBg, COLORREF panelText,
                      COLORREF menuBg, COLORREF menuText,
                      COLORREF menuSelBg, COLORREF menuSelText,
                      COLORREF toolbarBg, COLORREF toolbarText,
                      COLORREF buttonBg, COLORREF buttonText,
                      COLORREF buttonBorder, COLORREF buttonHot,
                      COLORREF buttonPressed,
                      COLORREF splitterBg, COLORREF splitterLine,
                      COLORREF pdfBg, COLORREF pdfPageBg,
                      COLORREF noteBg, COLORREF noteText,
                      COLORREF selectionBg, COLORREF selectionText,
                      COLORREF accent) {
    ThemeColors t;
    t.name = name ? name : L"default";
    t.nameJp = nameJp ? nameJp : L"";
    t.windowBg = windowBg;
    t.windowText = windowText;
    t.panelBg = panelBg;
    t.panelText = panelText;
    t.menuBg = menuBg;
    t.menuText = menuText;
    t.menuSelBg = menuSelBg;
    t.menuSelText = menuSelText;
    t.toolbarBg = toolbarBg;
    t.toolbarText = toolbarText;
    t.buttonBg = buttonBg;
    t.buttonText = buttonText;
    t.buttonBorder = buttonBorder;
    t.buttonHot = buttonHot;
    t.buttonPressed = buttonPressed;
    t.splitterBg = splitterBg;
    t.splitterLine = splitterLine;
    t.pdfBg = pdfBg;
    t.pdfPageBg = pdfPageBg;
    t.noteBg = noteBg;
    t.noteText = noteText;
    t.selectionBg = selectionBg;
    t.selectionText = selectionText;
    t.accent = accent;
    return t;
}

} // namespace

std::vector<ThemeColors> MakeBuiltInThemeCatalog() {
    std::vector<ThemeColors> themes;
    // Built-in themes embedded in the exe. Keep this list stable across the
    // main app and the read-only viewer so theme IDs can be passed by argv.
    themes.push_back(MakeTheme(
        L"000000",
        L"000000",
        RGB(30, 30, 30), RGB(224, 224, 224),
        RGB(37, 37, 38), RGB(224, 224, 224),
        RGB(28, 28, 28), RGB(224, 224, 224),
        RGB(58, 84, 117), RGB(255, 255, 255),
        RGB(37, 37, 38), RGB(224, 224, 224),
        RGB(50, 50, 50), RGB(224, 224, 224),
        RGB(70, 70, 70), RGB(60, 90, 120),
        RGB(45, 70, 100),
        RGB(40, 40, 40), RGB(90, 90, 90),
        RGB(25, 25, 25), RGB(255, 255, 255),
        RGB(30, 30, 30), RGB(224, 224, 224),
        RGB(58, 84, 117), RGB(255, 255, 255),
        RGB(79, 195, 247)
    ));
    themes.push_back(MakeTheme(
        L"00AA7B",
        L"00AA7B",
        RGB(236, 252, 246), RGB(14, 43, 31),
        RGB(243, 254, 250), RGB(14, 43, 31),
        RGB(231, 251, 244), RGB(14, 43, 31),
        RGB(189, 238, 221), RGB(8, 48, 34),
        RGB(236, 252, 246), RGB(14, 43, 31),
        RGB(246, 255, 252), RGB(14, 43, 31),
        RGB(142, 220, 196), RGB(212, 247, 235),
        RGB(189, 238, 221),
        RGB(161, 230, 207), RGB(106, 218, 185),
        RGB(230, 247, 240), RGB(255, 255, 255),
        RGB(241, 255, 250), RGB(14, 43, 31),
        RGB(189, 238, 221), RGB(8, 48, 34),
        RGB(0, 170, 123)
    ));
    themes.push_back(MakeTheme(
        L"1D50DD",
        L"1D50DD",
        RGB(232, 240, 255), RGB(15, 15, 15),
        RGB(245, 249, 255), RGB(15, 15, 15),
        RGB(232, 240, 255), RGB(20, 20, 20),
        RGB(210, 224, 255), RGB(15, 15, 15),
        RGB(232, 240, 255), RGB(20, 20, 20),
        RGB(255, 255, 255), RGB(20, 20, 20),
        RGB(185, 185, 185), RGB(232, 241, 255),
        RGB(215, 231, 255),
        RGB(224, 234, 255), RGB(120, 150, 235),
        RGB(210, 210, 210), RGB(255, 255, 255),
        RGB(250, 252, 255), RGB(15, 15, 15),
        RGB(200, 220, 255), RGB(15, 15, 15),
        RGB(29, 80, 221)
    ));
    themes.push_back(MakeTheme(
        L"FFBD85",
        L"FFBD85",
        RGB(255, 246, 236), RGB(42, 26, 13),
        RGB(255, 251, 246), RGB(42, 26, 13),
        RGB(255, 242, 227), RGB(42, 26, 13),
        RGB(255, 214, 177), RGB(42, 26, 13),
        RGB(255, 240, 223), RGB(42, 26, 13),
        RGB(255, 250, 244), RGB(42, 26, 13),
        RGB(216, 176, 141), RGB(255, 240, 223),
        RGB(255, 224, 196),
        RGB(243, 215, 190), RGB(216, 176, 141),
        RGB(234, 223, 212), RGB(255, 255, 255),
        RGB(255, 247, 238), RGB(42, 26, 13),
        RGB(255, 208, 165), RGB(42, 26, 13),
        RGB(255, 189, 133)
    ));
    themes.push_back(MakeTheme(
        L"FFFFFF",
        L"FFFFFF",
        RGB(255, 255, 255), RGB(0, 0, 0),
        RGB(255, 255, 255), RGB(0, 0, 0),
        RGB(255, 255, 255), RGB(0, 0, 0),
        RGB(160, 210, 255), RGB(0, 0, 0),
        RGB(255, 255, 255), RGB(0, 0, 0),
        RGB(255, 255, 255), RGB(0, 0, 0),
        RGB(200, 200, 200), RGB(235, 245, 255),
        RGB(210, 230, 255),
        RGB(235, 235, 235), RGB(180, 180, 180),
        RGB(230, 230, 230), RGB(255, 255, 255),
        RGB(255, 255, 255), RGB(0, 0, 0),
        RGB(160, 210, 255), RGB(0, 0, 0),
        RGB(0, 120, 215)
    ));
    return themes;
}

const ThemeColors* FindBuiltInThemeById(const std::vector<ThemeColors>& catalog,
                                        const std::wstring& id) {
    if (catalog.empty()) return nullptr;
    for (const auto& theme : catalog) {
        if (theme.name == id) return &theme;
    }
    return nullptr;
}

ThemeColors BuiltInThemeOrDefault(const std::wstring& id) {
    auto catalog = MakeBuiltInThemeCatalog();
    if (const ThemeColors* requested = FindBuiltInThemeById(catalog, id)) {
        return *requested;
    }
    if (const ThemeColors* fallback = FindBuiltInThemeById(catalog, kDefaultBuiltInThemeId)) {
        return *fallback;
    }
    return catalog.empty() ? ThemeColors{} : catalog.front();
}

} // namespace theme

