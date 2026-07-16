#include "training/sha256.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace imesvc::training {
namespace {

constexpr std::array<std::uint32_t, 64> kConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotate_right(std::uint32_t value, unsigned int amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class Sha256 final {
public:
    Sha256() = default;

    void update(const std::uint8_t* data, std::size_t size) {
        if (size > std::numeric_limits<std::uint64_t>::max() / 8U - bit_count_ / 8U) {
            throw std::runtime_error("SHA-256 input is too large");
        }
        bit_count_ += static_cast<std::uint64_t>(size) * 8U;
        while (size != 0) {
            const auto count = std::min(size, buffer_.size() - buffer_size_);
            std::copy_n(data, count, buffer_.data() + buffer_size_);
            data += count;
            size -= count;
            buffer_size_ += count;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                buffer_size_ = 0;
            }
        }
    }

    std::string finish() {
        const auto original_bit_count = bit_count_;
        const std::uint8_t one = 0x80U;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (buffer_size_ != 56U) update(&zero, 1);
        std::array<std::uint8_t, 8> length{};
        for (int shift = 56; shift >= 0; shift -= 8) length[static_cast<std::size_t>((56 - shift) / 8)] = static_cast<std::uint8_t>(original_bit_count >> shift);
        update(length.data(), length.size());
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto value : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) result.push_back(hex[(value >> shift) & 0xfU]);
        }
        return result;
    }

private:
    void transform(const std::uint8_t* data) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(data[index * 4U]) << 24U) |
                           (static_cast<std::uint32_t>(data[index * 4U + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(data[index * 4U + 2U]) << 8U) |
                           data[index * 4U + 3U];
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            words[index] = words[index - 16] +
                           (rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3U)) +
                           words[index - 7] +
                           (rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10U));
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto t1 = h + (rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25)) + ((e & f) ^ (~e & g)) +
                            kConstants[index] + words[index];
            const auto t2 = (rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_count_ = 0;
};

void update_u64(Sha256& digest, std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
    digest.update(bytes.data(), bytes.size());
}

void update_string(Sha256& digest, std::string_view value) {
    update_u64(digest, value.size());
    digest.update(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

void update_file(Sha256& digest, const std::filesystem::path& path, bool frame_size) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("SHA-256 input is not a regular non-symlink file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path, status_error);
    if (status_error) throw std::runtime_error("read SHA-256 input size failed: " + path.string());
    if (frame_size) update_u64(digest, size);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("open file for SHA-256 failed: " + path.string());
    std::array<std::uint8_t, 1024U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) digest.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) throw std::runtime_error("read file for SHA-256 failed: " + path.string());
}

}  // namespace

std::string sha256_file(const std::filesystem::path& path) {
    Sha256 digest;
    update_file(digest, path, false);
    return digest.finish();
}

std::string sha256_bytes(const void* data, std::size_t size) {
    if (data == nullptr && size != 0) throw std::invalid_argument("SHA-256 byte input is null");
    Sha256 digest;
    digest.update(static_cast<const std::uint8_t*>(data), size);
    return digest.finish();
}

std::string sha256_file_set(const std::filesystem::path& root,
                            const std::vector<std::filesystem::path>& relative_paths) {
    if (relative_paths.empty()) throw std::invalid_argument("SHA-256 file set is empty");
    Sha256 digest;
    update_string(digest, "llavon-ime-file-set-v1");
    for (const auto& relative : relative_paths) {
        if (relative.empty() || relative.is_absolute()) throw std::invalid_argument("SHA-256 file-set path must be relative");
        const auto normalized = relative.lexically_normal();
        if (normalized.empty() || *normalized.begin() == "..") throw std::invalid_argument("SHA-256 file-set path escapes its root");
        update_string(digest, normalized.generic_string());
        update_file(digest, root / normalized, true);
    }
    return digest.finish();
}

}  // namespace imesvc::training
