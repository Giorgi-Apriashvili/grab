// secrets::reveal and secrets::obscure, in a file of their own: they need OpenSSL, and the linker
// brings in what a source file refers to before it drops unused functions, so in secrets.cpp they
// made every program about 4 MB bigger (OpenSSL's algorithm tables) whether it logs in by itself or
// not.

#include "secrets.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grab::secrets {

namespace {

// rclone's fixed key (lib/obscure/obscure.go).
constexpr std::array<unsigned char, 32> obscure_key = {0x9c, 0x93, 0x5b, 0x48, 0x73, 0x0a, 0x55, 0x4d,
                                                       0x6b, 0xfd, 0x7c, 0x63, 0xc8, 0x86, 0xa9, 0x2b,
                                                       0xd3, 0x90, 0x19, 0x8e, 0xb8, 0x12, 0x8a, 0xfb,
                                                       0xf4, 0xde, 0x16, 0x2b, 0x8b, 0x95, 0xf6, 0x38};

// `in` under AES-256-CTR with rclone's key and the 16-byte `iv` into `out` (as long as `in`); CTR
// encrypts and decrypts alike.
bool crypt(const unsigned char* iv, const unsigned char* in, unsigned char* out, std::size_t size) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int n = 0;
    const bool ok = ctx != nullptr &&
                    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, obscure_key.data(), iv) == 1 &&
                    EVP_EncryptUpdate(ctx, out, &n, in, static_cast<int>(size)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok && std::cmp_equal(n, size);
}

} // namespace

std::optional<std::string> reveal(std::string_view obscured) {
    std::string raw;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : obscured) {
        int v = c >= 'A' && c <= 'Z'   ? c - 'A'
                : c >= 'a' && c <= 'z' ? c - 'a' + 26
                : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '-'             ? 62
                : c == '_'             ? 63
                                       : -1;
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    if (raw.size() < 16) return std::nullopt;
    std::string plain(raw.size() - 16, '\0');
    const auto* bytes = reinterpret_cast<const unsigned char*>(raw.data());
    const bool ok = crypt(bytes, bytes + 16, reinterpret_cast<unsigned char*>(plain.data()), plain.size());
    OPENSSL_cleanse(raw.data(), raw.size());
    if (!ok) return std::nullopt;
    return plain;
}

std::optional<std::string> obscure(std::string_view plain) {
    // The IV, then the text encrypted under it, in URL-safe base64 without padding.
    std::string raw(16 + plain.size(), '\0');
    auto* bytes = reinterpret_cast<unsigned char*>(raw.data());
    if (RAND_bytes(bytes, 16) != 1 ||
        !crypt(bytes, reinterpret_cast<const unsigned char*>(plain.data()), bytes + 16, plain.size())) {
        return std::nullopt;
    }
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string text;
    text.reserve((raw.size() * 4 + 2) / 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const unsigned char c : raw) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            text.push_back(alphabet[(acc >> bits) & 0x3F]);
        }
    }
    if (bits > 0) text.push_back(alphabet[(acc << (6 - bits)) & 0x3F]);
    return text;
}

} // namespace grab::secrets
