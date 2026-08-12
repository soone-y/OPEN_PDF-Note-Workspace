#pragma once

#include <filesystem>
#include <string>

namespace office {

std::string ProtectJapaneseTokensAfterIdeographicSpaceUtf8(const std::string& text);

bool ValidateOfficePackageForOfflineConversion(const std::filesystem::path& source,
                                               std::wstring* outErr);

bool TransformDocxForSpaceProtection(const std::filesystem::path& source,
                                     const std::filesystem::path& dest,
                                     std::wstring* outErr);

} // namespace office
