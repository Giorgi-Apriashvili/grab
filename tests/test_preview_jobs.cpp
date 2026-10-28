#include <doctest/doctest.h>

#include "conn_budget.hpp"
#include "fake_sftp.hpp"
#include "preview.hpp"
#include "preview_jobs.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_read.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;

namespace {

// A server's files for the previews: sessions on in-memory SFTP servers sharing them.
struct Server {
    std::map<std::string, std::string> files;
    std::map<std::string, std::uint64_t> claimed; // sizes STAT claims instead
    std::set<std::string> fifos, no_size;
    std::optional<budget::Failure> login_fails;
    std::string login_error = "Fake refused the login: the key was refused";
    std::atomic<int> logins{0};
    std::atomic<std::uint64_t> asked{0}; // bytes READs asked for, as sessions close

    struct Session : ssh::Session {
        Session(Server& s) : owner(s), client(server) {
            server.shared_files = &s.files;
            server.listed_size = s.claimed;
            server.fifos = s.fifos;
            server.no_size = s.no_size;
            client.start();
        }
        ~Session() override {
            for (const auto& [off, len] : server.reads) owner.asked += len;
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        sftp::Client& sftp() override { return client; }
        bool reusable() override { return client.pending() == 0; }
        Server& owner;
        test::FakeServer server;
        sftp::Client client;
    };

    std::shared_ptr<ssh::Pool> pool() {
        auto o = ssh::Pools::download_options(4);
        o.idle_timer = false;
        return std::make_shared<ssh::Pool>([this]() -> std::unique_ptr<ssh::Session> {
            ++logins;
            if (login_fails) throw ssh::Error(*login_fails, login_error);
            return std::make_unique<Session>(*this);
        }, o);
    }

    preview::Fetched fetch(const std::string& path) {
        auto p = pool();
        ssh::Reader reader(p, path, {});
        return preview::fetch(reader, "Main", path);
    }
};

std::string text(const preview::Answer& a) { return a.body ? *a.body : std::string(); }

preview::Answer ok(std::string body) {
    return preview::Answer{.status = 200,
                           .content_type = "image/png",
                           .headers = {},
                           .body = std::make_shared<const std::string>(std::move(body))};
}

} // namespace

TEST_CASE("preview fetch: images and PDFs whole, within their caps; over a cap refused before anything is read") {
    Server s;
    s.files["/p/a.png"] = test::pattern(300'000);
    s.files["/p/doc.pdf"] = test::pattern(70'000);
    s.files["/p/huge.jpg"] = "small, but STAT says otherwise";
    s.claimed["/p/huge.jpg"] = std::uint64_t{65} << 20;
    auto png = s.fetch("/p/a.png");
    CHECK(png.answer.status == 200);
    CHECK(png.answer.content_type == "image/png");
    CHECK(text(png.answer) == s.files["/p/a.png"]);
    CHECK(png.keep);
    CHECK(png.stamp == preview::Stamp{.size = 300'000, .modified = 1700000000});
    auto pdf = s.fetch("/p/doc.pdf");
    CHECK(pdf.answer.status == 200);
    CHECK(pdf.answer.content_type == "application/pdf");

    s.asked = 0;
    auto huge = s.fetch("/p/huge.jpg");
    CHECK(huge.answer.status == 413);
    CHECK(text(huge.answer) == "Too big to preview (65.0 MiB); download it instead.");
    CHECK_FALSE(huge.keep);
    CHECK(s.asked == 0);
}

TEST_CASE("preview fetch: a file cut while it was read: an image refused, text shown but not kept") {
    Server s;
    s.files["/p/cut.png"] = test::pattern(5'000);
    s.claimed["/p/cut.png"] = 9'000; // STAT's size, before the file was cut
    auto png = s.fetch("/p/cut.png");
    CHECK(png.answer.status == 502);
    CHECK(text(png.answer) == "cut.png changed on Main while it was read.");
    CHECK_FALSE(png.keep);

    s.files["/p/cut.txt"] = "first lines";
    s.claimed["/p/cut.txt"] = 9'000;
    auto txt = s.fetch("/p/cut.txt");
    CHECK(txt.answer.status == 200);
    CHECK(text(txt.answer) == "first lines");
    CHECK(txt.answer.headers.empty()); // no X-Grab-Total from a size that no longer holds
    CHECK_FALSE(txt.keep);
}

TEST_CASE("preview fetch: text's first MiB, its size told; .nfo as the DOS code page; no size from the server") {
    Server s;
    std::string big((std::size_t{1} << 20) + 500, 'a');
    s.files["/t/big.log"] = big;
    s.files["/t/art.nfo"] = "\xDB\xB0 hi";
    s.files["/t/nosize.txt"] = big;
    s.no_size.insert("/t/nosize.txt");
    s.files["/t/small.txt"] = "short";
    s.no_size.insert("/t/small.txt");

    auto cut = s.fetch("/t/big.log");
    CHECK(cut.answer.status == 200);
    CHECK(text(cut.answer).size() == preview::max_text);
    CHECK(cut.answer.headers == std::format("X-Grab-Total: {}", big.size()));
    CHECK(s.asked == preview::max_text); // only the first MiB was asked for

    auto nfo = s.fetch("/t/art.nfo");
    CHECK(text(nfo.answer) == "\xE2\x96\x88\xE2\x96\x91 hi"); // █░

    auto nosize = s.fetch("/t/nosize.txt");
    CHECK(nosize.answer.status == 200);
    CHECK(text(nosize.answer).size() == preview::max_text); // cut, though the total isn't known
    CHECK(nosize.answer.headers.empty());
    CHECK(nosize.stamp.size == std::nullopt);

    auto small = s.fetch("/t/small.txt");
    CHECK(text(small.answer) == "short");
    CHECK(small.answer.headers.empty());
}

TEST_CASE("preview fetch: failures in the window's words, never kept") {
    Server s;
    s.fifos.insert("/f/pipe.png");
    auto missing = s.fetch("/f/gone.png");
    CHECK(missing.answer.status == 404);
    CHECK(text(missing.answer) == "Not found on Main.");
    CHECK_FALSE(missing.keep);
    auto pipe = s.fetch("/f/pipe.png");
    CHECK(pipe.answer.status == 415);
    CHECK(text(pipe.answer) == "pipe.png on Main isn't a regular file.");
    auto kind = s.fetch("/f/movie.mkv");
    CHECK(kind.answer.status == 415);

    s.login_fails = budget::Failure::fatal;
    s.login_error = "65.21.224.123's host key isn't trusted yet";
    auto login = s.fetch("/f/a.png");
    CHECK(login.answer.status == 502);
    CHECK(text(login.answer) ==
          "Can't log in to Main: 65.21.224.123's host key isn't trusted yet. If the key is right, use Trust… in Settings.");
    CHECK_FALSE(login.keep);
    s.login_error = "Fake refused the login: ssh-agent can't be reached";
    auto agent = s.fetch("/f/a.png");
    CHECK(text(agent.answer) ==
          "Can't log in to Main: Fake refused the login: ssh-agent can't be reached. Test the server in Settings to try again.");
    s.login_fails = budget::Failure::other;
    s.login_error = "Fake: the connection timed out";
    auto refused = s.fetch("/f/a.png");
    CHECK(text(refused.answer) == "Can't log in to Main: Fake: the connection timed out.");

    s.files["/f/x.svg"] = "<svg/>";
    s.login_fails.reset();
    auto svg = s.fetch("/f/x.svg");
    CHECK(svg.answer.status == 200);
    CHECK(svg.answer.headers == "Content-Security-Policy: sandbox");
}

TEST_CASE("preview fetch: a stop answers cancelled") {
    Server s;
    s.files["/a.png"] = test::pattern(std::size_t{3} << 20);
    auto p = s.pool();
    std::stop_source stop;
    ssh::ReadOptions o;
    o.stop = stop.get_token();
    o.progress = [&](std::uint64_t, std::uint64_t) { stop.request_stop(); };
    ssh::Reader reader(p, "/a.png", o);
    const auto f = preview::fetch(reader, "Main", "/a.png");
    CHECK(f.answer.status == 503);
    CHECK(text(f.answer) == "cancelled");
    CHECK_FALSE(f.keep);
}

TEST_CASE("preview cache: fresh, or older with the row's size and time unchanged; bytes capped; a server forgotten") {
    using Clock = preview::Cache::Clock;
    const auto t0 = Clock::now();
    preview::Cache cache(1000, 60s);
    const preview::Stamp stamp{.size = 10, .modified = 1700000000};
    cache.put("Main\n/a.png", ok("aaaa"), stamp, t0);
    CHECK(cache.find("Main\n/a.png", {}, t0 + 59s)); // fresh: no row needed
    CHECK(cache.find("Main\n/a.png", stamp, t0 + 300s)); // older, the row unchanged
    CHECK_FALSE(cache.find("Main\n/a.png", {.size = 11, .modified = 1700000000}, t0 + 300s)); // changed: dropped
    CHECK(cache.size() == 0);

    cache.put("Main\n/b.png", ok("bbbb"), stamp, t0);
    CHECK_FALSE(cache.find("Main\n/b.png", {}, t0 + 61s)); // older, nothing to compare with
    CHECK_FALSE(cache.find("Main\n/b.png", {.size = 10, .modified = {}}, t0 + 61s));

    cache.put("Main\n/1", ok(std::string(400, '1')), stamp, t0);
    cache.put("Main\n/2", ok(std::string(400, '2')), stamp, t0);
    cache.put("Box\n/3", ok(std::string(400, '3')), stamp, t0); // over 1000: the oldest goes
    CHECK(cache.bytes() == 800);
    CHECK_FALSE(cache.find("Main\n/1", {}, t0));
    CHECK(cache.find("Main\n/2", {}, t0));
    cache.forget_server("Main");
    CHECK_FALSE(cache.find("Main\n/2", {}, t0));
    CHECK(cache.find("Box\n/3", {}, t0));
    cache.forget_server("Bo"); // a prefix of another server's name isn't that server
    CHECK(cache.find("Box\n/3", {}, t0));
}

TEST_CASE("previews: a second request joins the fetch; failures aren't kept; a kept answer comes at once") {
    preview::Previews previews;
    std::vector<int> got;
    auto reply = [&](int tag) { return [&got, tag](preview::Answer a) { got.push_back(tag * 1000 + a.status); }; };
    auto job = previews.ask({.server = "Main", .path = "/a.png", .row = {}, .ahead = true}, reply(1));
    REQUIRE(job);
    CHECK(job->ahead);
    CHECK_FALSE(previews.ask({.server = "Main", .path = "/a.png", .row = {}, .ahead = false}, reply(2)));
    CHECK_FALSE(previews.ahead(*job)); // the one shown joined it
    CHECK(previews.jobs() == 1);
    REQUIRE(previews.begin(job));
    previews.end(job, preview::Fetched{.answer = preview::failed(502, "Lost the connection"), .stamp = {}, .keep = false});
    CHECK(got == std::vector<int>{1502, 2502});
    CHECK(previews.jobs() == 0);
    CHECK(previews.cached_bytes() == 0);

    got.clear();
    auto again = previews.ask({.server = "Main", .path = "/a.png", .row = {}, .ahead = false}, reply(3));
    REQUIRE(again);
    REQUIRE(previews.begin(again));
    previews.end(again, preview::Fetched{.answer = ok("png"), .stamp = {}, .keep = true});
    CHECK(previews.ask({.server = "Main", .path = "/a.png", .row = {}, .ahead = false}, reply(4)) == nullptr);
    CHECK(got == std::vector<int>{3200, 4200});
    CHECK(previews.cached_bytes() == 3);
    previews.forget_server("Main");
    CHECK(previews.cached_bytes() == 0);
}

TEST_CASE("previews: a job kept meanwhile isn't fetched again; one cancelled answers at once, or once it stops") {
    preview::Previews previews;
    std::vector<std::string> got;
    auto reply = [&](std::string tag) {
        return [&got, tag](preview::Answer a) { got.push_back(tag + ":" + std::to_string(a.status)); };
    };
    // Two jobs for one file can't both wait, but a job queued while another one's answer was kept can.
    auto first = previews.ask({.server = "S", .path = "/a", .row = {}, .ahead = false}, reply("a1"));
    REQUIRE(first);
    REQUIRE(previews.begin(first));
    previews.cancel({}); // the viewer moved: the running one stops at its next READs
    CHECK(first->stop.stop_requested());
    CHECK(got.empty());
    auto second = previews.ask({.server = "S", .path = "/a", .row = {}, .ahead = false}, reply("a2"));
    REQUIRE(second); // a new job: the stopped one takes no requests
    previews.end(first, preview::Fetched{.answer = ok("done anyway"), .stamp = {}, .keep = true});
    CHECK(got == std::vector<std::string>{"a1:200"});
    CHECK_FALSE(previews.begin(second)); // kept by the first one meanwhile: answered from it
    CHECK(got == std::vector<std::string>{"a1:200", "a2:200"});

    got.clear();
    auto queued = previews.ask({.server = "S", .path = "/b", .row = {}, .ahead = true}, reply("b"));
    auto wanted = previews.ask({.server = "S", .path = "/c", .row = {}, .ahead = true}, reply("c"));
    REQUIRE(queued);
    REQUIRE(wanted);
    previews.cancel({preview::key_of("S", "/c")});
    CHECK(got == std::vector<std::string>{"b:503"}); // queued: answered at once
    CHECK(queued->stop.stop_requested());
    CHECK_FALSE(previews.begin(queued)); // and never runs
    CHECK_FALSE(wanted->stop.stop_requested());
    CHECK(previews.jobs() == 1);

    previews.stop_all();
    CHECK(wanted->stop.stop_requested());
    CHECK(previews.jobs() == 0);
}

TEST_CASE("previews: the page's URLs") {
    const auto r = preview::parse_url("https://grab.ui/preview/Box%20Guest/music%2Fa%20b.png?size=123&modified=1700000000&ahead");
    REQUIRE(r);
    CHECK(r->server == "Box Guest");
    CHECK(r->path == "music/a b.png");
    CHECK(r->row == preview::Stamp{.size = 123, .modified = 1700000000});
    CHECK(r->ahead);
    const auto plain = preview::parse_url("https://grab.ui/preview/Main/%2Fsrv%2Fx.txt");
    REQUIRE(plain);
    CHECK(plain->path == "/srv/x.txt");
    CHECK(plain->row == preview::Stamp{});
    CHECK_FALSE(plain->ahead);
    const auto odd = preview::parse_url("https://grab.ui/preview/Main/x.txt?size=12x&modified=");
    REQUIRE(odd);
    CHECK(odd->row == preview::Stamp{});
    CHECK_FALSE(preview::parse_url("https://grab.ui/preview/Main"));
    CHECK_FALSE(preview::parse_url("https://grab.ui/preview//x"));
    CHECK_FALSE(preview::parse_url("https://grab.ui/preview/Main/%zz"));
    CHECK_FALSE(preview::parse_url("https://grab.ui/index.html"));
    CHECK(preview::key_of("Main", "/a") == "Main\n/a");
}
