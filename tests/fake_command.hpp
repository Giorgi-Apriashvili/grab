#pragma once

// A command's channel in memory for the tests of exec (test_ssh.cpp): canned output, and a server
// that refuses commands, a command that waits for its stdin, stays silent, never ends or never
// stops talking, or a link that dies. It records what was done to it.

#include "ssh.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace grab::test {

class FakeCommand : public ssh::CommandChannel {
public:
    std::vector<std::string> out;        // stdout, piece by piece
    std::string err;                     // stderr
    int status = 0;
    std::string signal;                  // ended by this signal instead (no exit status)
    bool refuse = false;                 // the server refuses the exec request
    std::string refuse_channel;          // or the channel, saying this (MaxSessions 1)
    int start_waits = 0;                 // start() can't tell yet this often first
    bool needs_eof = false;              // says nothing until its stdin ends (cat, sftp-server)
    std::chrono::milliseconds silent{0}; // says nothing this long after the start (sleep)
    bool endless = false;                // stdout never ends (yes)
    bool forever = false;                // never ends by itself (sleep 60)
    bool answers = true;                 // keepalives get an answer (false: the link is dead)

    std::string command;                       // what it was asked to run
    std::vector<std::string> log;              // start, eof, keepalive, TERM, drop, drop dead, close
    std::chrono::milliseconds longest_wait{0}; // the longest wait() asked for
    bool dropped = false;

    std::optional<bool> start(std::string_view c) override {
        if (start_waits-- > 0) return std::nullopt;
        command = c;
        log.emplace_back("start");
        started_ = std::chrono::steady_clock::now();
        running_ = !refuse && refuse_channel.empty();
        received_ += 100; // the channel's confirmation and the exec request's answer
        return running_;
    }
    std::string refusal() override { return refuse_channel; }
    bool send_eof() override {
        log.emplace_back("eof");
        eof_ = true;
        return true;
    }
    std::size_t read(bool from_err, std::span<char> buffer) override {
        if (!talking()) return 0;
        std::size_t n = 0;
        if (from_err) {
            n = std::min(err.size() - err_at_, buffer.size());
            std::memcpy(buffer.data(), err.data() + err_at_, n);
            err_at_ += n;
        } else if (endless) {
            n = buffer.size();
            for (std::size_t i = 0; i < n; ++i) buffer[i] = i % 2 ? '\n' : 'y';
        } else if (piece_ < out.size()) {
            const std::string& piece = out[piece_];
            n = std::min(piece.size() - at_, buffer.size());
            std::memcpy(buffer.data(), piece.data() + at_, n);
            at_ += n;
            if (at_ == piece.size()) {
                ++piece_;
                at_ = 0;
            }
        }
        received_ += n;
        return n;
    }
    bool ended() override { return talking() && !forever && !endless && piece_ == out.size() && err_at_ == err.size(); }
    bool close() override {
        log.emplace_back("close");
        return true;
    }
    int exit_status() override { return signal.empty() ? status : -1; }
    std::string exit_signal() override { return signal; }
    void terminate() override { log.emplace_back("TERM"); }
    void drop(bool dead) override {
        log.emplace_back(dead ? "drop dead" : "drop");
        dropped = true;
    }
    void keepalive() override {
        log.emplace_back("keepalive");
        if (answers) received_ += 60;
    }
    std::uint64_t received() override { return received_; }
    void wait(std::chrono::milliseconds most) override {
        longest_wait = std::max(longest_wait, most);
        std::this_thread::sleep_for(std::min(most, std::chrono::milliseconds(2)));
    }

    [[nodiscard]] std::ptrdiff_t count(std::string_view what) const { return std::ranges::count(log, what); }

private:
    [[nodiscard]] bool talking() const {
        return running_ && (eof_ || !needs_eof) && std::chrono::steady_clock::now() - started_ >= silent;
    }

    std::chrono::steady_clock::time_point started_;
    bool running_ = false;
    bool eof_ = false;
    std::size_t piece_ = 0, at_ = 0, err_at_ = 0;
    std::uint64_t received_ = 0;
};

} // namespace grab::test
