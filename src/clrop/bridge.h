#pragma once

#include <string>
#include <vector>

#include "types.h"
#include "core/app_core.h"

namespace clrop_bridge {

enum class LoadAnnotationsValidation {
    None,
    Fast,
    Strong,
};

// clrop パスを決める: PDFと同じ場所で拡張子だけ .clrop にする。
std::wstring ClropPathForPdf(const std::wstring& pdfPath);

// clropファイルを読み、Annotation配列に変換する。
// mismatch には指定された validation における pdf_id 不一致を返す。err は致命エラー時。
bool LoadAnnotations(const std::wstring& clropPath,
                     const std::wstring& pdfPath,
                     std::vector<Annotation>& out,
                     bool& mismatch,
                     clrop::PdfId* loadedId,
                     std::wstring& err,
                     LoadAnnotationsValidation validation = LoadAnnotationsValidation::Strong,
                     const clrop::PdfId* expectedPdfId = nullptr);

// Annotation配列を clrop にシリアライズして保存する。
bool SaveAnnotations(const std::wstring& clropPath,
                     const std::wstring& pdfPath,
                     const std::vector<Annotation>& annots,
                     std::wstring& err);
bool SerializeAnnotations(const std::wstring& pdfPath,
                          const std::vector<Annotation>& annots,
                          std::string& outJson,
                          std::wstring& err);

} // namespace clrop_bridge
