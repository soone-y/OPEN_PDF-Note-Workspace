#include "pdf_view/worker_render.h"
#include "pdf_view/text_coverage.h"
#include "fpdf_edit.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    FPDF_InitLibrary();
    FPDF_DOCUMENT document = FPDF_CreateNewDocument(); assert(document);
    FPDF_PAGE page = FPDFPage_New(document, 0, 256, 192); assert(page);
    for (int i = 0; i < 2400; ++i) {
        const float x = (i * 29) % 248, y = (i * 17) % 184;
        FPDF_PAGEOBJECT object = FPDFPageObj_CreateNewRect(x, y, 8, 8); assert(object);
        assert(FPDFPageObj_SetFillColor(object, (i * 7) % 256, (i * 13) % 256, (i * 23) % 256, 127));
        assert(FPDFPath_SetDrawMode(object, FPDF_FILLMODE_WINDING, false));
        FPDFPage_InsertObject(page, object);
    }
    FPDF_PAGEOBJECT text = FPDFPageObj_NewTextObj(document, "Helvetica", 18); assert(text);
    const unsigned short label[] = {'T','h','r','e','a','d',0};
    assert(FPDFText_SetText(text, label));
    FPDFPageObj_Transform(text, 1, 0, 0, 1, 20, 90); FPDFPage_InsertObject(page, text);
    assert(FPDFPage_GenerateContent(page));
    const int objects = FPDFPage_CountObjects(page);
    std::recursive_mutex mutex;
    size_t yields = 0;
    for (int rotation = 0; rotation < 4; ++rotation) {
        std::vector<uint8_t> expected(256 * 192 * 4, 255), actual(expected.size(), 255);
        FPDF_BITMAP reference = FPDFBitmap_CreateEx(256, 192, FPDFBitmap_BGRA, expected.data(), 256 * 4);
        FPDF_BITMAP progressive = FPDFBitmap_CreateEx(256, 192, FPDFBitmap_BGRA, actual.data(), 256 * 4);
        assert(reference && progressive);
        FPDF_RenderPageBitmap(reference, page, 0, 0, 256, 192, rotation, FPDF_ANNOT | FPDF_LCD_TEXT);
        std::unique_lock<std::recursive_mutex> engine(mutex);
        bool uiAcquired = false;
        auto yield = [&] {
            ++yields;
            assert(!engine.owns_lock());
            if (!uiAcquired) {
                std::thread ui([&] { std::lock_guard<std::recursive_mutex> lock(mutex); uiAcquired = true; });
                ui.join(); // Completion proves another engine user acquired the released lock.
            }
        };
        assert(pdf_worker_render::Render(engine, progressive, page, 256, 192, rotation,
            FPDF_ANNOT | FPDF_LCD_TEXT, [] { return false; }, yield, 0));
        assert(uiAcquired && engine.owns_lock() && actual == expected);
        bool canceled = false;
        assert(!pdf_worker_render::Render(engine, progressive, page, 256, 192, rotation,
            0, [&] { return canceled; }, [&] { canceled = true; }, 0));
        assert(canceled && engine.owns_lock());
        bool caught = false;
        try {
            (void)pdf_worker_render::Render(engine, progressive, page, 256, 192, rotation,
                0, [] { return false; }, [] { throw std::runtime_error("yield fault"); }, 0);
        } catch (const std::runtime_error&) { caught = true; }
        assert(caught && engine.owns_lock());
        std::fill(actual.begin(), actual.end(), 255);
        assert(pdf_worker_render::Render(engine, progressive, page, 256, 192, rotation,
            FPDF_ANNOT | FPDF_LCD_TEXT, [] { return false; }, yield, 0) && actual == expected);
        std::vector<uint8_t> coverage, progressiveCoverage;
        assert(pdf_text_coverage::Render(document, 0, 256, 192, rotation, coverage));
        assert(pdf_text_coverage::RenderWith(document, 0, 256, 192, rotation, progressiveCoverage,
            [&](FPDF_BITMAP bitmap, FPDF_PAGE copied, int w, int h, int rotate) {
                return pdf_worker_render::Render(engine, bitmap, copied, w, h, rotate, 0, [] { return false; }, yield, 0);
            }));
        assert(coverage == progressiveCoverage && FPDFPage_CountObjects(page) == objects);
        FPDFBitmap_Destroy(progressive); FPDFBitmap_Destroy(reference);
    }
    assert(yields > 0);
    FPDF_ClosePage(page); FPDF_CloseDocument(document); FPDF_DestroyLibrary();
    std::cout << "PASS pdf_worker_render rotations=4 pixels_equal=1 coverage_equal=1 engine_yield=1 cancel=1 exception_cleanup=1\n";
}
