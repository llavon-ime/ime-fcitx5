#pragma once

#include <sodium.h>

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ime::unix_service::commit_crypto {

inline void initialize() {
    static const int result = sodium_init();
    if (result < 0) throw std::runtime_error("crypto initialization failed");
}

template <std::size_t Size>
struct Secret final {
    std::array<unsigned char, Size> bytes{};
    Secret() = default;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret() { sodium_memzero(bytes.data(), bytes.size()); }
};

struct PublicParameters {
    std::array<unsigned char, crypto_pwhash_SALTBYTES> salt{};
    std::array<unsigned char, crypto_box_PUBLICKEYBYTES> key{};
};

// Version 1 fixes these parameters so upgrades cannot change the derived key.
inline constexpr unsigned long long operations = 3;
inline constexpr std::size_t memory = 64 * 1024 * 1024;
using PrivateKey = Secret<crypto_box_SECRETKEYBYTES>;

inline void derive(std::string_view password, PublicParameters& parameters,
                   PrivateKey& private_key, bool verify) {
    initialize();
    if (password.empty()) throw std::runtime_error("password required");
    Secret<crypto_box_SEEDBYTES> seed;
    if (crypto_pwhash(seed.bytes.data(), seed.bytes.size(), password.data(),
                      password.size(), parameters.salt.data(), operations, memory,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("password derivation failed");
    }
    std::array<unsigned char, crypto_box_PUBLICKEYBYTES> public_key{};
    crypto_box_seed_keypair(public_key.data(), private_key.bytes.data(), seed.bytes.data());
    if (verify && sodium_memcmp(public_key.data(), parameters.key.data(), public_key.size()) != 0) {
        throw std::runtime_error("incorrect password");
    }
    parameters.key = public_key;
}

// Decryption material for the training and review paths. `configured` means
// the caller supplied a verified password, so every row must be encrypted.
struct Decryption {
    bool configured = false;
    const PublicParameters* parameters = nullptr;
    const PrivateKey* private_key = nullptr;
};

// Ciphertexts are bound to the record ID and the field name (readings also
// bind their position), so swapping a value between records or columns fails.
inline std::string field_identity(std::string_view id, std::string_view field) {
    return std::string(id) + "/" + std::string(field);
}

inline std::string reading_identity(std::string_view id, int position) {
    return std::string(id) + "/reading/" + std::to_string(position);
}

inline std::string hex(std::span<const unsigned char> bytes) {
    std::string result(bytes.size() * 2 + 1, '\0');
    sodium_bin2hex(result.data(), result.size(), bytes.data(), bytes.size());
    result.pop_back();
    return result;
}

inline void unhex(std::string_view text, std::span<unsigned char> destination) {
    std::size_t size = 0;
    if (text.size() != destination.size() * 2 ||
        sodium_hex2bin(destination.data(), destination.size(), text.data(), text.size(),
                       nullptr, &size, nullptr) != 0 || size != destination.size()) {
        throw std::runtime_error("invalid encrypted data");
    }
}

// The identity binds the ciphertext to one record field, so swapping columns
// or records fails authentication.
inline std::string seal(std::string_view value, std::string_view identity,
                        const PublicParameters& parameters) {
    std::string message(identity);
    message.push_back('\0');
    message.append(value);
    std::string ciphertext(message.size() + crypto_box_SEALBYTES, '\0');
    const int result = crypto_box_seal(reinterpret_cast<unsigned char*>(ciphertext.data()),
        reinterpret_cast<const unsigned char*>(message.data()), message.size(), parameters.key.data());
    sodium_memzero(message.data(), message.size());
    if (result != 0) throw std::runtime_error("commit encryption failed");
    return hex({reinterpret_cast<const unsigned char*>(ciphertext.data()), ciphertext.size()});
}

inline std::string open(std::string_view ciphertext, std::string_view identity,
                        const PublicParameters& parameters, const PrivateKey& private_key) {
    if (ciphertext.size() % 2 != 0 || ciphertext.size() / 2 < crypto_box_SEALBYTES) {
        throw std::runtime_error("invalid encrypted commit");
    }
    std::string binary(ciphertext.size() / 2, '\0');
    unhex(ciphertext, {reinterpret_cast<unsigned char*>(binary.data()), binary.size()});
    std::string message(binary.size() - crypto_box_SEALBYTES, '\0');
    const int result = crypto_box_seal_open(reinterpret_cast<unsigned char*>(message.data()),
        reinterpret_cast<const unsigned char*>(binary.data()), binary.size(),
        parameters.key.data(), private_key.bytes.data());
    const bool valid = result == 0 && message.size() > identity.size() &&
        message.compare(0, identity.size(), identity) == 0 && message[identity.size()] == '\0';
    std::string value;
    if (valid) value = message.substr(identity.size() + 1);
    sodium_memzero(message.data(), message.size());
    if (!valid) throw std::runtime_error("commit authentication failed for " + std::string(identity));
    return value;
}

}  // namespace ime::unix_service::commit_crypto
