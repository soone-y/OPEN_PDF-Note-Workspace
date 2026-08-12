#pragma once

#include <windows.h>

#include <optional>
#include <string>

inline void SecureClearString(std::string& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
    }
    value.clear();
}

inline void SecureClearString(std::wstring& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    }
    value.clear();
}

inline void SecureResetOptionalString(std::optional<std::string>& value) noexcept {
    if (value.has_value()) {
        SecureClearString(*value);
    }
    value.reset();
}

class SecureStringScope final {
public:
    explicit SecureStringScope(std::string* value) noexcept : value_(value) {}
    ~SecureStringScope() {
        if (value_) SecureClearString(*value_);
    }
    SecureStringScope(const SecureStringScope&) = delete;
    SecureStringScope& operator=(const SecureStringScope&) = delete;

private:
    std::string* value_ = nullptr;
};

class SecureWideStringScope final {
public:
    explicit SecureWideStringScope(std::wstring* value) noexcept : value_(value) {}
    ~SecureWideStringScope() {
        if (value_) SecureClearString(*value_);
    }
    SecureWideStringScope(const SecureWideStringScope&) = delete;
    SecureWideStringScope& operator=(const SecureWideStringScope&) = delete;

private:
    std::wstring* value_ = nullptr;
};

class SecureOptionalStringScope final {
public:
    explicit SecureOptionalStringScope(std::optional<std::string>* value) noexcept : value_(value) {}
    ~SecureOptionalStringScope() {
        if (value_) SecureResetOptionalString(*value_);
    }
    SecureOptionalStringScope(const SecureOptionalStringScope&) = delete;
    SecureOptionalStringScope& operator=(const SecureOptionalStringScope&) = delete;

private:
    std::optional<std::string>* value_ = nullptr;
};
