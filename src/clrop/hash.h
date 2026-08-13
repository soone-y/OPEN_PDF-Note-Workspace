#pragma once

#include <string>

#include "types.h"

namespace clrop {

// PDFファイルから size + page_count + page_sizes_pt を計算する。path は参照情報として格納。
// 取得できない値がある場合は、利用できた範囲まで返す。
clrop::PdfId ComputePdfFastId(const std::wstring& pdfPath);

// PDFファイルから size + page_count + page_sizes_pt + sha256 を計算する。
// sha256 は path + size + mtime ベースのプロセス内キャッシュで再利用する。
clrop::PdfId ComputePdfId(const std::wstring& pdfPath);

bool PdfIdHasFastFingerprint(const clrop::PdfId& id);
bool PdfIdFastMatches(const clrop::PdfId& lhs, const clrop::PdfId& rhs);
bool PdfIdStrongMatches(const clrop::PdfId& lhs, const clrop::PdfId& rhs);

} // namespace clrop
