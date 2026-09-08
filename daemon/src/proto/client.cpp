// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <algorithm>
#include <cerrno>

#include <unistd.h>

namespace kas {
namespace proto {

namespace {

// Global scratch buffer used to swallow oversized payloads: the daemon keeps
// consuming the frame but stores nothing (one global buffer for the garbage).
constexpr size_t kDiscardChunk = 4096;
char g_discard[kDiscardChunk];

// Map a read() result to a read_result. On error, errno is already set.
read_result map_read_result(ssize_t n) {
    if (n > 0) {
        return read_result::progress;
    }
    if (n == 0) {
        return read_result::eof;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return read_result::again;
    }
    return read_result::error;
}

} // namespace

Client::Client(int fd, size_t rx_cap, size_t tx_cap)
    : fd_(fd),
      rx_(rx_cap == 0 ? kDefaultBufferCap : rx_cap),
      tx_(tx_cap == 0 ? kDefaultBufferCap : tx_cap) {}

Client::~Client() = default;

void Client::rx_resume() {
    rx_paused_ = false;
}

int Client::push(const char* msg, size_t len) {
    if (fd_ < 0) {
        return -ENOTCONN;
    }
    return tx_.push(msg, len);
}

ssize_t Client::flush() {
    if (fd_ < 0) {
        return -ENOTCONN;
    }
    return tx_.flush(fd_);
}

// Explicit state machine: byte stream -> zero or more JSON message payloads.
//
//   header              accumulate the 4-byte native-endian length header
//     | length == 0 or > 1 MiB  -> discard_oversized (consume, never store)
//     | buffer full     -> paused (do NOT consume the frame; wait for
//     |                   poll() to drain, then rx_resume())
//     | otherwise       -> payload (zero-copy read into the RX buffer)
//
// Contract: a stage completion (header read fully, payload complete, frame
// discarded) returns progress so callers loop; again means "would block" and
// paused means "stop reading until the RX buffer drains".
read_result Client::read() {
    if (fd_ < 0) {
        errno = EBADF;
        return read_result::error;
    }
    if (rx_paused_) {
        // RX buffer could not hold the pending frame; the payload must not be
        // consumed. The caller stops reading until poll() drained the queue.
        return read_result::paused;
    }

    for (;;) {
        switch (rx_state_) {
        case state::header: {
            // have_read_ == kHeaderLen only when resuming after a pause: the
            // header was already consumed, re-run the transition decision.
            if (have_read_ < kHeaderLen) {
                char* dst = reinterpret_cast<char*>(&msg_len_) + have_read_;
                const size_t want = kHeaderLen - have_read_;
                ssize_t n;
                do {
                    n = ::read(fd_, dst, want);
                } while (n < 0 && errno == EINTR);
                read_result r = map_read_result(n);
                if (r != read_result::progress) {
                    return r;
                }
                have_read_ += static_cast<uint32_t>(n);
                if (have_read_ < kHeaderLen) {
                    return read_result::progress; // partial header
                }
                // msg_len_ now holds the payload length (native byte order).
            }
            if (msg_len_ == 0) {
                // A zero-length payload is not a valid JSON value; treat it
                // like an oversized frame (discard, keep the connection).
                ++discarded_frames_;
                have_read_ = 0;
                continue;
            }
            if (msg_len_ > kMaxMessageLen) {
                rx_state_ = state::discard_oversized;
                have_read_ = 0;
                return read_result::progress; // start consuming next call
            }
            if (rx_.can_fit(msg_len_)) {
                rx_state_ = state::payload;
                have_read_ = 0;
                return read_result::progress; // payload read next call
            }
            // Legal frame but the RX buffer cannot hold it: pause. The frame
            // is NOT consumed — its bytes stay in the socket until the buffer
            // drains and rx_resume() is called.
            rx_paused_ = true;
            return read_result::paused;
        }

        case state::payload: {
            const size_t want = msg_len_ - have_read_;
            // Zero-copy: read straight into the RX buffer tail.
            char* dst = rx_.tail();
            ssize_t n;
            do {
                n = ::read(fd_, dst, want);
            } while (n < 0 && errno == EINTR);
            read_result r = map_read_result(n);
            if (r != read_result::progress) {
                return r;
            }
            have_read_ += static_cast<uint32_t>(n);
            rx_.advance(static_cast<size_t>(n));
            if (have_read_ == msg_len_) {
                rx_.message_complete();
                rx_state_ = state::header;
                have_read_ = 0;
            }
            return read_result::progress;
        }

        case state::discard_oversized: {
            const size_t remaining = msg_len_ - have_read_;
            const size_t want = std::min(remaining, kDiscardChunk);
            ssize_t n;
            do {
                n = ::read(fd_, g_discard, want);
            } while (n < 0 && errno == EINTR);
            read_result r = map_read_result(n);
            if (r != read_result::progress) {
                return r;
            }
            have_read_ += static_cast<uint32_t>(n);
            if (have_read_ == msg_len_) {
                ++discarded_frames_; // caller logs the error
                rx_state_ = state::header;
                have_read_ = 0;
            }
            return read_result::progress;
        }
        }
    }
}

} // namespace proto
} // namespace kas
