#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace search {
// Private, versioned local protocol. Lengths are checked before allocation;
// no pointers, HWNDs, or executable paths cross this boundary.
class Writer {
public:
    std::string bytes;
    template<class T> void Put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    void Text(std::wstring_view value) {
        if (value.size() > 32 * 1024 * 1024) throw std::length_error("search text");
        Put<uint32_t>(static_cast<uint32_t>(value.size()));
        bytes.append(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
    }
};
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    template<class T> [[nodiscard]] T Get() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (bytes_.size() < sizeof(T)) throw std::runtime_error("short search record");
        T value; std::memcpy(&value, bytes_.data(), sizeof(T));
        bytes_.remove_prefix(sizeof(T)); return value;
    }
    [[nodiscard]] std::wstring Text(size_t limit = 32768) {
        const auto count = Get<uint32_t>();
        if (count > limit || count > bytes_.size() / sizeof(wchar_t))
            throw std::runtime_error("invalid search text");
        std::wstring value(count, L'\0');
        if (count) std::memcpy(value.data(), bytes_.data(), count * sizeof(wchar_t));
        bytes_.remove_prefix(count * sizeof(wchar_t));
        if (value.find(L'\0') != std::wstring::npos) throw std::runtime_error("nul search text");
        return value;
    }
    void End() const { if (!bytes_.empty()) throw std::runtime_error("extra search data"); }
private:
    std::string_view bytes_;
};
}
