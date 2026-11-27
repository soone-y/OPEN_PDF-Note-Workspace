#include "math/math_render_internal.h"
#include "core/sha256.h"
#include "render/builder.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace mathrender {
namespace {
constexpr int kPadding = 2;
constexpr size_t kFontBytes = 995497;
constexpr char kFontDigest[] = "1bf9cbe66a4357d63600e3f8e8415321d9d2f87f1476cd4b7e36bed6534e3042";

std::filesystem::path FontPath() {
    std::vector<wchar_t> name(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (!length) throw std::runtime_error("Cannot locate math resources");
        if (length < name.size()) {
            return std::filesystem::path(std::wstring(name.data(), length)).parent_path() /
                L"resources/math/xits/XITSMath-Regular.clm2";
        }
        if (name.size() >= 32768) throw std::runtime_error("Executable path exceeds Windows limit");
        name.resize(name.size() * 2);
    }
}

bool FontDigestMatches(const std::vector<microtex::u8>& bytes) {
    core_hash::Sha256 hash;
    hash.Update(bytes.data(), bytes.size());
    const auto digest = hash.Finalize();
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
        if (hex[digest[i] >> 4] != kFontDigest[i * 2] ||
            hex[digest[i] & 15] != kFontDigest[i * 2 + 1]) return false;
    }
    return true;
}

bool ValidGroups(std::wstring_view source) {
    size_t depth = 0;
    bool comment = false;
    for (size_t i = 0; i < source.size(); ++i) {
        const wchar_t c = source[i];
        if (c == L'\r' || c == L'\n') comment = false;
        if (comment) continue;
        if (c == L'%') { comment = true; continue; }
        if (c == L'\\') { if (i + 1 < source.size()) ++i; continue; }
        if (c == L'{') {
            if (++depth > 256) return false;
        } else if (c == L'}') {
            if (!depth) return false;
            --depth;
        }
    }
    return depth == 0;
}

std::string Utf8(std::wstring_view source) {
    if (source.empty() || source.size() > 1024 * 1024 || source.find(L'\0') != std::wstring_view::npos) {
        throw std::runtime_error("Invalid math source size");
    }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("Invalid math Unicode");
    std::string result(length, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
        static_cast<int>(source.size()), result.data(), length, nullptr, nullptr) != length) {
        throw std::runtime_error("Cannot encode math source");
    }
    return result;
}

std::shared_ptr<detail::Runtime> GetRuntime() {
    static const auto runtime = std::make_shared<detail::Runtime>();
    return runtime;
}
} // namespace

namespace detail {
Runtime::Runtime() {
    const auto path = FontPath();
    if (std::filesystem::file_size(path) != kFontBytes) throw std::runtime_error("Invalid math font resource");
    fontData.resize(kFontBytes);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(fontData.data()), static_cast<std::streamsize>(fontData.size())) ||
        !FontDigestMatches(fontData)) throw std::runtime_error("Math font resource integrity failed");
    RegisterWindowsBackend();
    const auto meta = microtex::MicroTeX::init(microtex::FontSrcData(fontData.size(), fontData.data()));
    mathFont = meta.name;
    microtex::MicroTeX::setPrivilegedEnvironment(false);
}
Runtime::~Runtime() {
    microtex::MicroTeX::release();
    ReleaseWindowsBackend();
}
} // namespace detail

struct Formula::Data {
    struct Variant {
        int fontPx = 0;
        std::unique_ptr<microtex::Render> render;
        Layout layout;
    };
    std::shared_ptr<detail::Runtime> runtime;
    std::unique_ptr<microtex::Formula> expression;
    mutable std::array<Variant, 2> variants;

    const Variant& Prepare(int fontPx, RenderStyle style) const {
        auto& variant = variants[style == RenderStyle::Display ? 1 : 0];
        if (variant.render && variant.fontPx == fontPx) return variant;
        auto render = std::unique_ptr<microtex::Render>(microtex::RenderBuilder()
            .setStyle(style == RenderStyle::Display ? microtex::TexStyle::display : microtex::TexStyle::text)
            .setTextSize(static_cast<float>(fontPx)).setMathFontName(runtime->mathFont)
            .setForeground(microtex::black).build(*expression));
        const int width = render->getWidth();
        const int height = render->getHeight();
        const int depth = render->getDepth();
        constexpr int bound = std::numeric_limits<int>::max() - kPadding * 2;
        if (width < 0 || height <= 0 || width > bound || height > bound || depth < 0 || depth > height) {
            throw std::runtime_error("Invalid math dimensions");
        }
        variant.layout = {width + kPadding * 2, height + kPadding * 2, height - depth + kPadding};
        variant.fontPx = fontPx;
        variant.render = std::move(render);
        return variant;
    }
};

Formula::Formula(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
Formula::~Formula() {
    const auto runtime = data_->runtime;
    std::lock_guard<std::recursive_mutex> guard(runtime->mutex);
    data_.reset();
}

std::unique_ptr<Formula> Parse(std::wstring_view source, std::string* diagnostic) noexcept {
    if (diagnostic) diagnostic->clear();
    try {
        if (!ValidGroups(source)) throw std::runtime_error("Invalid math grouping");
        const auto runtime = GetRuntime();
        std::lock_guard<std::recursive_mutex> guard(runtime->mutex);
        auto data = std::make_unique<Formula::Data>();
        data->runtime = runtime;
        data->expression = std::make_unique<microtex::Formula>();
        microtex::Parser parser(false, Utf8(source), data->expression.get(), true);
        parser.parse();
        return std::unique_ptr<Formula>(new Formula(std::move(data)));
    } catch (const std::exception& error) {
        if (diagnostic) {
            try { *diagnostic = error.what(); } catch (...) { diagnostic->clear(); }
        }
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

Layout Measure(const Formula& formula, HDC dc, int fontPx, RenderStyle style) noexcept {
    if (!dc || fontPx <= 0) return {};
    try {
        std::lock_guard<std::recursive_mutex> guard(formula.data_->runtime->mutex);
        return formula.data_->Prepare(fontPx, style).layout;
    } catch (...) {
        return {};
    }
}

bool Draw(const Formula& formula, const Layout& layout, HDC dc, int x, int y,
          int fontPx, COLORREF colour, RenderStyle style) noexcept {
    if (!dc || fontPx <= 0 || x > std::numeric_limits<int>::max() - kPadding ||
        y > std::numeric_limits<int>::max() - kPadding) return false;
    try {
        std::lock_guard<std::recursive_mutex> guard(formula.data_->runtime->mutex);
        const auto& prepared = formula.data_->Prepare(fontPx, style);
        if (layout.width != prepared.layout.width || layout.height != prepared.layout.height ||
            layout.baseline != prepared.layout.baseline) return false;
        return detail::DrawPrepared(*prepared.render, dc, x + kPadding, y + kPadding, colour);
    } catch (...) {
        return false;
    }
}
} // namespace mathrender
