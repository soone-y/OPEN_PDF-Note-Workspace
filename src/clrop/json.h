#pragma once

#include <filesystem>
#include <string>

#include "types.h"

namespace clrop {

class ItemLoadSink {
public:
    virtual ~ItemLoadSink() = default;
    virtual bool OnClropItem(int page, clrop::Item&& item, std::wstring& err) = 0;
};

enum class LoadFileFailureKind {
    None,
    Read,
    Parse,
};

// まだ非接続フェーズのスタブ。実装はフェーズ1の一部として拡張する。
bool ParseClropFromJson(const std::string& json, clrop::Document& out, std::wstring& err);
bool ParseClropFromJsonToSink(const std::string& json,
                              clrop::PdfId* outPdfId,
                              clrop::ItemLoadSink& sink,
                              std::wstring& err);
bool LoadClropFile(const std::wstring& path,
                   clrop::Document& out,
                   std::wstring& err,
                   LoadFileFailureKind* failureKind = nullptr);
bool LoadClropFileToSink(const std::wstring& path,
                         clrop::PdfId* outPdfId,
                         clrop::ItemLoadSink& sink,
                         std::wstring& err,
                         LoadFileFailureKind* failureKind = nullptr);
bool SerializeClrop(const clrop::Document& doc, std::string& out, std::wstring& err);
bool SaveClropFile(const std::wstring& path, const clrop::Document& doc, std::wstring& err);
bool SaveClropFile(const std::wstring& path,
                   const clrop::Document& doc,
                   std::wstring& err,
                   const std::filesystem::path& preferredTempDir,
                   const std::filesystem::path& quarantineDir);

} // namespace clrop
