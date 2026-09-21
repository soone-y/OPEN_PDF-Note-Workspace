#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Small header-only SHA-256 utility for deterministic local integrity guards.
// It neither opens files nor performs network I/O.  Keeping the state owned by
// the caller makes it usable for virtual concatenations such as a text edit
// without allocating the complete post-edit text.
namespace core_hash {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 final {
public:
    Sha256() { Reset(); }

    void Reset() {
        h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        buffer_.fill(0);
        bitCount_ = 0;
        used_ = 0;
    }

    void Update(const std::uint8_t* data, size_t size) {
        if (!data || size == 0) return;
        for (size_t index = 0; index < size; ++index) {
            buffer_[used_++] = data[index];
            bitCount_ += 8;
            if (used_ == buffer_.size()) {
                ProcessBlock(buffer_.data());
                used_ = 0;
            }
        }
    }

    Sha256Digest Finalize() {
        buffer_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < buffer_.size()) buffer_[used_++] = 0;
            ProcessBlock(buffer_.data());
            used_ = 0;
        }
        while (used_ < 56) buffer_[used_++] = 0;
        for (int index = 7; index >= 0; --index) {
            buffer_[used_++] = static_cast<std::uint8_t>((bitCount_ >> (index * 8)) & 0xffu);
        }
        ProcessBlock(buffer_.data());

        Sha256Digest digest{};
        for (size_t index = 0; index < h_.size(); ++index) {
            digest[index * 4 + 0] = static_cast<std::uint8_t>((h_[index] >> 24) & 0xffu);
            digest[index * 4 + 1] = static_cast<std::uint8_t>((h_[index] >> 16) & 0xffu);
            digest[index * 4 + 2] = static_cast<std::uint8_t>((h_[index] >> 8) & 0xffu);
            digest[index * 4 + 3] = static_cast<std::uint8_t>(h_[index] & 0xffu);
        }
        return digest;
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRoundConstants = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    static std::uint32_t RotateRight(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void ProcessBlock(const std::uint8_t* block) {
        std::uint32_t words[64]{};
        for (int index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                           (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                           (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                           static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = RotateRight(words[index - 15], 7) ^
                                     RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const std::uint32_t s1 = RotateRight(words[index - 2], 17) ^
                                     RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[static_cast<size_t>(index)] + words[index];
            const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t bitCount_ = 0;
    size_t used_ = 0;
};

} // namespace core_hash
