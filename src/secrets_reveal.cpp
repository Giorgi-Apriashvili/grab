// secrets::reveal, in a file of its own: it needs OpenSSL, and the linker brings in what a source
// file refers to before it drops unused functions, so in secrets.cpp it made every program about
// 4 MB bigger (OpenSSL's algorithm tables) whether it logs in by itself or not.

#include "secrets.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace grab::secrets {

std::optional<std::string> reveal(std::string_view obscured) {
    // rclone's fixed key (lib/obscure/obscure.go).
    static constexpr unsigned char key[32] = {0x9c, 0x93, 0x5b, 0x48, 0x73, 0x0a, 0x55, 0x4d, 0x6b, 0xfd, 0x7c,
                                              0x63, 0xc8, 0x86, 0xa9, 0x2b, 0xd3, 0x90, 0x19, 0x8e, 0xb8, 0x12,
                                              0x8a, 0xfb, 0xf4, 0xde, 0x16, 0x2b, 0x8b, 0x95, 0xf6, 0x38};
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
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int n = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key,
                                        reinterpret_cast<const unsigned char*>(raw.data())) == 1 &&
              EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &n,
                                reinterpret_cast<const unsigned char*>(raw.data() + 16),
                                static_cast<int>(raw.size() - 16)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(raw.data(), raw.size());
    if (!ok || static_cast<std::size_t>(n) != plain.size()) return std::nullopt;
    return plain;
}

} // namespace grab::secrets