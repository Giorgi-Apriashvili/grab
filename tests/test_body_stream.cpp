#include <doctest/doctest.h>

#include "body_stream.hpp"
#include "fake_sftp.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace grab;
using Microsoft::WRL::ComPtr;

namespace {

// Everything from the stream's position, 32 KiB per Read as WebView2 reads it.
std::string read_all(IStream* s) {
    std::string out;
    std::vector<char> buffer(std::size_t{32} * 1024);
    for (;;) {
        ULONG n = 0;
        const HRESULT hr = s->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &n);
        if (FAILED(hr)) return "failed";
        out.append(buffer.data(), n);
        if (hr == S_FALSE || n == 0) return out;
    }
}

} // namespace

TEST_CASE("body stream: reads the shared body, seeks, says its size, refuses writes") {
    const auto body = std::make_shared<const std::string>(test::pattern(100'000));
    ComPtr<IStream> s;
    s.Attach(new_body_stream(body));
    CHECK(read_all(s.Get()) == *body);

    STATSTG st{};
    REQUIRE(s->Stat(&st, STATFLAG_DEFAULT) == S_OK);
    CHECK(st.cbSize.QuadPart == 100'000u);
    CHECK(st.type == STGTY_STREAM);
    CHECK(st.pwcsName == nullptr);

    LARGE_INTEGER to{};
    ULARGE_INTEGER now{};
    to.QuadPart = -10;
    REQUIRE(s->Seek(to, STREAM_SEEK_END, &now) == S_OK);
    CHECK(now.QuadPart == 99'990u);
    CHECK(read_all(s.Get()) == body->substr(99'990));
    to.QuadPart = -1;
    CHECK(s->Seek(to, STREAM_SEEK_SET, &now) == STG_E_INVALIDFUNCTION); // before the start
    to.QuadPart = 200'000;
    REQUIRE(s->Seek(to, STREAM_SEEK_SET, &now) == S_OK); // past the end: nothing to read there
    CHECK(read_all(s.Get()).empty());

    ULONG wrote = 7;
    CHECK(s->Write("x", 1, &wrote) == STG_E_ACCESSDENIED);
    CHECK(wrote == 0);

    ComPtr<ISequentialStream> seq;
    CHECK(SUCCEEDED(s.As(&seq)));
    ComPtr<IUnknown> unknown;
    CHECK(SUCCEEDED(s.As(&unknown)));

    ComPtr<IStream> empty;
    empty.Attach(new_body_stream(nullptr));
    CHECK(read_all(empty.Get()).empty());
}

TEST_CASE("body stream: clones share the body with positions of their own; the body goes with the last one") {
    std::weak_ptr<const std::string> watch;
    {
        auto body = std::make_shared<const std::string>(test::pattern(1'000'000));
        watch = body;
        ComPtr<IStream> s;
        s.Attach(new_body_stream(body));
        body.reset();
        std::vector<char> head(1000);
        ULONG n = 0;
        REQUIRE(s->Read(head.data(), 1000, &n) == S_OK);
        ComPtr<IStream> clone;
        REQUIRE(s->Clone(&clone) == S_OK);
        // Two threads at once: the clone from where the stream was, the stream itself on and on.
        std::string from_clone;
        std::string from_stream;
        {
            std::jthread a([&] { from_clone = read_all(clone.Get()); });
            std::jthread b([&] { from_stream = read_all(s.Get()); });
        }
        const auto expected = watch.lock()->substr(1000);
        CHECK(from_clone == expected);
        CHECK(from_stream == expected);
        CHECK_FALSE(watch.expired());
    }
    CHECK(watch.expired());
}

TEST_CASE("body stream: one stream read by two threads gives each byte once") {
    const auto body = std::make_shared<const std::string>(test::pattern(4'000'000));
    ComPtr<IStream> s;
    s.Attach(new_body_stream(body));
    std::atomic<std::size_t> total{0};
    {
        std::vector<std::jthread> readers;
        for (int i = 0; i < 2; ++i) {
            readers.emplace_back([&] {
                std::vector<char> buffer(std::size_t{32} * 1024);
                for (;;) {
                    ULONG n = 0;
                    if (FAILED(s->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &n)) || n == 0) return;
                    total += n;
                }
            });
        }
    }
    CHECK(total == body->size());
}
