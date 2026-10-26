#include <doctest/doctest.h>

#include <libssh2.h>

#include <string_view>

TEST_CASE("libssh2 is built in, on OpenSSL") {
    REQUIRE(libssh2_init(0) == 0);
    CHECK(std::string_view(libssh2_version(0)).starts_with("1.11"));
    CHECK(libssh2_crypto_engine() == libssh2_openssl);
    libssh2_exit();
}
