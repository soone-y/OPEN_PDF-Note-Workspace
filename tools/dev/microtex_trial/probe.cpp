// Offscreen comparison only. The caller owns fresh input/output paths and the
// process timeout. Input is read-only, and failures return nonzero with evidence.
#include <windows.h>
#include <gdiplus.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

constexpr int kTransparentBackground = TRANSPARENT;
#undef TRANSPARENT

#include "math/math_render.h"
#ifdef MICROTEX_TRIAL_OPENMATH
#include "microtex.h"
#include "graphic_win32.h"
#include "core/formula.h"
#include "render/builder.h"
#include "openmath_gdi_path.h"
namespace tex = microtex;
using TrialRender = microtex::Render;
#else
#include "latex.h"
#include "core/formula.h"
#include "platform/gdi_win/graphic_win32.h"
using TrialRender = tex::TeXRender;
#endif

namespace {
using Clock = std::chrono::steady_clock;

[[nodiscard]] double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::wstring ReadFormula(const std::filesystem::path& path) {
    const auto length = std::filesystem::file_size(path);
    if (length == 0 || length > 1024 * 1024) throw std::runtime_error("Invalid input size");
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(length), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Input read failed");
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                       static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Input must be UTF-8");
    std::wstring text(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                            static_cast<int>(bytes.size()), text.data(), size) != size) {
        throw std::runtime_error("Input decoding failed");
    }
    return text;
}

struct GdiPlusSession {
    ULONG_PTR token = 0;
    GdiPlusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("GDI+ startup failed");
        }
    }
    ~GdiPlusSession() { Gdiplus::GdiplusShutdown(token); }
    GdiPlusSession(const GdiPlusSession&) = delete;
    GdiPlusSession& operator=(const GdiPlusSession&) = delete;
};

class Surface {
public:
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;

    Surface() {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("CreateCompatibleDC failed");
    }
    ~Surface() {
        if (oldFont) SelectObject(dc, oldFont);
        if (oldBitmap) SelectObject(dc, oldBitmap);
        if (font) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    void SetFontSize(int pixels) {
        font = CreateFontW(-pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Cambria Math");
        if (!font) throw std::runtime_error("CreateFont failed");
        oldFont = SelectObject(dc, font);
        if (!oldFont || oldFont == HGDI_ERROR) throw std::runtime_error("Select font failed");
        SetBkMode(dc, kTransparentBackground);
        SetTextColor(dc, RGB(0, 0, 0));
    }

    void Allocate(int width, int height) {
        if (width <= 0 || height <= 0 || width > 12000 || height > 12000 ||
            static_cast<int64_t>(width) * height > 24000000) {
            throw std::runtime_error("Layout exceeds trial image budget");
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) throw std::runtime_error("CreateDIBSection failed");
        oldBitmap = SelectObject(dc, bitmap);
        if (!oldBitmap || oldBitmap == HGDI_ERROR) throw std::runtime_error("Select bitmap failed");
        if (!PatBlt(dc, 0, 0, width, height, WHITENESS)) throw std::runtime_error("Clear failed");
    }

    void Save(const std::filesystem::path& path) {
        GdiFlush();
        Gdiplus::Bitmap image(bitmap, nullptr);
        const CLSID pngEncoder{0x557cf406, 0x1a04, 0x11d3,
                              {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        if (image.Save(path.c_str(), &pngEncoder, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("PNG save failed");
        }
    }
};

struct Measurements {
    int width = 0;
    int height = 0;
    int baseline = 0;
    double initMs = 0;
    double firstPrepareMs = 0;
    double prepareMs = 0;
    double drawMs = 0;
};

struct MicrotexSession {
#ifdef MICROTEX_TRIAL_OPENMATH
    // FontSrcFile uses narrow fopen on Windows. Keep the explicit UTF-16 path
    // at the application boundary and retain the bytes for the context lifetime.
    std::vector<tex::u8> fontData;
    explicit MicrotexSession(const std::string& resources) {
        const auto path = std::filesystem::u8path(resources) / "XITSMath-Regular.clm2";
        const auto size = std::filesystem::file_size(path);
        if (size == 0 || size > 8 * 1024 * 1024) throw std::runtime_error("Invalid font data size");
        fontData.resize(static_cast<size_t>(size));
        std::ifstream file(path, std::ios::binary);
        if (!file.read(reinterpret_cast<char*>(fontData.data()),
                       static_cast<std::streamsize>(fontData.size()))) {
            throw std::runtime_error("Font data read failed");
        }
        tex::MicroTeX::init(tex::FontSrcData(fontData.size(), fontData.data()));
        tex::PlatformFactory::registerFactory("gdi", std::make_unique<tex::PlatformFactory_gdi>());
        tex::PlatformFactory::activate("gdi");
        tex::MicroTeX::setPrivilegedEnvironment(false);
    }
    ~MicrotexSession() { tex::MicroTeX::release(); }
#else
    explicit MicrotexSession(const std::string& resources) { tex::LaTeX::init(resources); }
    ~MicrotexSession() { tex::LaTeX::release(); }
#endif
    MicrotexSession(const MicrotexSession&) = delete;
    MicrotexSession& operator=(const MicrotexSession&) = delete;
};

[[nodiscard]] std::unique_ptr<TrialRender> PrepareMicrotex(
    const std::wstring& input, int pixels, bool display) {
#ifdef MICROTEX_TRIAL_OPENMATH
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("UTF-8 conversion failed");
    std::string utf8(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), utf8.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    tex::Formula formula;
    tex::Parser parser(false, utf8, &formula, true);
    parser.parse();
    return std::unique_ptr<TrialRender>(tex::RenderBuilder()
        .setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
        .setTextSize(static_cast<float>(pixels))
        .setMathFontName(tex::MicroTeX::mathFontNames().at(0))
        .setForeground(tex::black).build(formula));
#else
    tex::Formula formula;
    // The default Formula parser accepts partial input and unknown commands.
    // Use its public strict parser for the comparison's error boundary.
    tex::TeXParser parser(false, input, &formula);
    parser.parse();
    tex::TeXRenderBuilder builder;
    return std::unique_ptr<tex::TeXRender>(builder
        .setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
        .setTextSize(static_cast<float>(pixels))
        .setForeground(tex::black)
        .build(formula));
#endif
}

[[nodiscard]] Measurements RunMicrotex(const std::wstring& input, const std::string& resources,
                                       Surface& surface, int pixels, bool display, int iterations) {
    Measurements result;
    auto start = Clock::now();
    MicrotexSession session(resources);
    result.initMs = Milliseconds(start);
#ifndef MICROTEX_TRIAL_OPENMATH
    if (std::filesystem::weakly_canonical(std::filesystem::u8path(tex::LaTeX::getResRootPath())) !=
        std::filesystem::weakly_canonical(std::filesystem::u8path(resources))) {
        throw std::runtime_error("MicroTeX selected an unexpected resource directory");
    }
#endif
    start = Clock::now();
    auto prepared = PrepareMicrotex(input, pixels, display);
    result.firstPrepareMs = Milliseconds(start);
    result.width = prepared->getWidth();
    result.height = prepared->getHeight();
    // MicroTeX returns a baseline ratio. The application expects pixels from top.
    result.baseline = result.height - prepared->getDepth();
    surface.Allocate(result.width + 64, result.height + 64);
    start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto temporary = PrepareMicrotex(input, pixels, display);
        if (!temporary) throw std::runtime_error("MicroTeX preparation failed");
    }
    result.prepareMs = Milliseconds(start) / iterations;
    {
        Gdiplus::Graphics graphics(surface.dc);
#ifdef MICROTEX_TRIAL_OPENMATH
        OpenmathPathGraphics context(&graphics);
#else
        tex::Graphics2D_win32 context(&graphics);
#endif
        prepared->draw(context, 32, 32); // Warm drawing and font resources.
        start = Clock::now();
        for (int i = 0; i < iterations; ++i) prepared->draw(context, 32, 32);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        GdiFlush();
        result.drawMs = Milliseconds(start) / iterations;
        graphics.Clear(Gdiplus::Color(255, 255, 255, 255));
        prepared->draw(context, 32, 32);
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    prepared.reset();
    return result;
}

[[nodiscard]] Measurements RunCurrent(const std::wstring& input, Surface& surface,
                                      int pixels, bool display, int iterations) {
    Measurements result;
    const auto style = display ? mathrender::RenderStyle::Display : mathrender::RenderStyle::Inline;
    auto start = Clock::now();
    auto prepared = mathrender::Parse(input);
    if (!prepared) throw std::runtime_error("Current preparation failed");
    auto layout = mathrender::Measure(*prepared, surface.dc, pixels, style);
    result.firstPrepareMs = Milliseconds(start);
    result.width = layout.width;
    result.height = layout.height;
    result.baseline = layout.baseline;
    surface.Allocate(result.width + 64, result.height + 64);
    start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto temporary = mathrender::Parse(input);
        if (!temporary) throw std::runtime_error("Current preparation failed");
        (void)mathrender::Measure(*temporary, surface.dc, pixels, style);
    }
    result.prepareMs = Milliseconds(start) / iterations;
    mathrender::Draw(*prepared, layout, surface.dc, 32, 32, pixels, RGB(0, 0, 0), style);
    start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        mathrender::Draw(*prepared, layout, surface.dc, 32, 32, pixels, RGB(0, 0, 0), style);
    }
    GdiFlush();
    result.drawMs = Milliseconds(start) / iterations;
    if (!PatBlt(surface.dc, 0, 0, result.width + 64, result.height + 64, WHITENESS)) {
        throw std::runtime_error("Clear failed");
    }
    mathrender::Draw(*prepared, layout, surface.dc, 32, 32, pixels, RGB(0, 0, 0), style);
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 8) throw std::runtime_error("Expected engine resources input output pixels style iterations");
        const std::wstring engine(argv[1]);
        if (engine != L"current" && engine != L"microtex") throw std::runtime_error("Invalid engine");
        const std::filesystem::path resources(argv[2]), inputPath(argv[3]), outputPath(argv[4]);
        if (!resources.is_absolute() || !inputPath.is_absolute() || !outputPath.is_absolute() ||
            std::filesystem::exists(outputPath)) throw std::runtime_error("Use absolute paths and a fresh output");
#ifdef MICROTEX_TRIAL_OPENMATH
        const auto marker = resources / L"XITSMath-Regular.clm2";
#else
        const auto marker = resources / L".clatexmath-res_root";
#endif
        if (engine == L"microtex" &&
            (!std::filesystem::is_directory(resources) ||
             !std::filesystem::is_regular_file(marker))) {
            throw std::runtime_error("Missing MicroTeX resource directory or marker");
        }
        const int pixels = std::stoi(argv[5]);
        const std::wstring style(argv[6]);
        const int iterations = std::stoi(argv[7]);
        if (pixels < 8 || pixels > 120 || iterations < 1 || iterations > 1000 ||
            (style != L"display" && style != L"inline")) throw std::runtime_error("Invalid trial parameters");
        const auto input = ReadFormula(inputPath);
        GdiPlusSession session;
        Surface surface;
        surface.SetFontSize(pixels);
        const auto result = engine == L"current"
            ? RunCurrent(input, surface, pixels, style == L"display", iterations)
            : RunMicrotex(input, resources.u8string(), surface, pixels, style == L"display", iterations);
        if (result.width <= 0 || result.height <= 0 || result.baseline < 0 ||
            result.baseline > result.height) throw std::runtime_error("Invalid measured bounds");
        surface.Save(outputPath);
        std::cout << "{\"width\":" << result.width << ",\"height\":" << result.height
                  << ",\"baseline\":" << result.baseline << ",\"init_ms\":" << result.initMs
                  << ",\"first_prepare_ms\":" << result.firstPrepareMs
                  << ",\"prepare_ms\":" << result.prepareMs << ",\"draw_ms\":" << result.drawMs << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
