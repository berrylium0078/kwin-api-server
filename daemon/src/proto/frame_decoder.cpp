// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <algorithm>
#include <cerrno>

#include <unistd.h>

namespace kas {
namespace proto {

namespace {

// Global scratch buffer used to swallow oversized payloads: the daemon keeps
// consuming the frame but stores nothing ("用一个全局缓冲区接废消息").
constexpr size_t kDiscardChunk = 4096;
char g_discard[kDiscardChunk];

// Map a read() result to an rx_result. On error, errno is already set.
rx_result map_read_result(ssize_t n) {
    if (n > 0) {
        return rx_result::kProgress;
    }
    if (n == 0) {
        return rx_result::kEof;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return rx_result::kAgain;
    }
    return rx_result::kError;
}

} // namespace

int client_init(client* c, int fd, size_t rx_cap, size_t tx_cap) {
    if (!c || fd < 0) {
        return -EINVAL;
    }
    if (rx_cap == 0) {
        rx_cap = kDefaultBufferCap;
    }
    if (tx_cap == 0) {
        tx_cap = kDefaultBufferCap;
    }
    int r = rx_buffer_init(&c->rx, rx_cap);
    if (r < 0) {
        return r;
    }
    r = tx_buffer_init(&c->tx, tx_cap);
    if (r < 0) {
        rx_buffer_free(&c->rx);
        return r;
    }
    c->fd = fd;
    c->rx_state = RX_HEADER;
    c->msg_len = 0;
    c->have_read = 0;
    c->rx_paused = false;
    c->discarded_frames = 0;
    return 0;
}

void client_destroy(client* c) {
    if (!c) {
        return;
    }
    rx_buffer_free(&c->rx);
    tx_buffer_free(&c->tx);
    c->fd = -1;
    c->rx_state = RX_HEADER;
    c->msg_len = 0;
    c->have_read = 0;
    c->rx_paused = false;
}

void client_rx_resume(client* c) {
    if (c) {
        c->rx_paused = false;
    }
}

// Explicit state machine: byte stream -> zero or more JSON message payloads.
//
//   RX_HEADER           accumulate the 4-byte native-endian length header
//     | length == 0 or > 1 MiB  -> RX_DISCARD_OVERSIZED (consume, never store)
//     | buffer full     -> kPaused (do NOT consume the frame; wait for
//     |                   poll() to drain, then client_rx_resume())
//     | otherwise       -> RX_PAYLOAD (zero-copy read into the RX buffer)
//
// Contract: a stage completion (header read fully, payload complete, frame
// discarded) returns kProgress so callers loop; kAgain means "would block"
// and kPaused means "stop reading until the RX buffer drains".
rx_result client_read(client* c) {
    if (!c || c->fd < 0) {
        errno = EBADF;
        return rx_result::kError;
    }
    if (c->rx_paused) {
        // RX buffer could not hold the pending frame; the payload must not be
        // consumed. The caller stops reading until poll() drained the queue.
        return rx_result::kPaused;
    }

    for (;;) {
        switch (c->rx_state) {
        case RX_HEADER: {
            // have_read == kHeaderLen only when resuming after a pause: the
            // header was already consumed, re-run the transition decision.
            if (c->have_read < kHeaderLen) {
                char* dst = reinterpret_cast<char*>(&c->msg_len) + c->have_read;
                const size_t want = kHeaderLen - c->have_read;
                ssize_t n;
                do {
                    n = ::read(c->fd, dst, want);
                } while (n < 0 && errno == EINTR);
                rx_result r = map_read_result(n);
                if (r != rx_result::kProgress) {
                    return r;
                }
                c->have_read += static_cast<uint32_t>(n);
                if (c->have_read < kHeaderLen) {
                    return rx_result::kProgress; // partial header
                }
                // msg_len now holds the payload length (native byte order).
            }
            if (c->msg_len == 0) {
                // A zero-length payload is not a valid JSON value; treat it
                // like an oversized frame (discard, keep the connection).
                ++c->discarded_frames;
                c->have_read = 0;
                continue;
            }
            if (c->msg_len > kMaxMessageLen) {
                c->rx_state = RX_DISCARD_OVERSIZED;
                c->have_read = 0;
                return rx_result::kProgress; // start consuming next call
            }
            if (rx_buffer_can_fit(&c->rx, c->msg_len)) {
                c->rx_state = RX_PAYLOAD;
                c->have_read = 0;
                return rx_result::kProgress; // payload read next call
            }
            // Legal frame but the RX buffer cannot hold it: pause. The frame
            // is NOT consumed — its bytes stay in the socket until the buffer
            // drains and client_rx_resume() is called.
            c->rx_paused = true;
            return rx_result::kPaused;
        }

        case RX_PAYLOAD: {
            const size_t want = c->msg_len - c->have_read;
            // Zero-copy: read straight into the RX buffer tail.
            char* dst = c->rx.data + c->rx.len;
            ssize_t n;
            do {
                n = ::read(c->fd, dst, want);
            } while (n < 0 && errno == EINTR);
            rx_result r = map_read_result(n);
            if (r != rx_result::kProgress) {
                return r;
            }
            c->have_read += static_cast<uint32_t>(n);
            c->rx.len += static_cast<size_t>(n);
            if (c->have_read == c->msg_len) {
                rx_buffer_message_complete(&c->rx);
                c->rx_state = RX_HEADER;
                c->have_read = 0;
            }
            return rx_result::kProgress;
        }

        case RX_DISCARD_OVERSIZED: {
            const size_t remaining = c->msg_len - c->have_read;
            const size_t want = std::min(remaining, kDiscardChunk);
            ssize_t n;
            do {
                n = ::read(c->fd, g_discard, want);
            } while (n < 0 && errno == EINTR);
            rx_result r = map_read_result(n);
            if (r != rx_result::kProgress) {
                return r;
            }
            c->have_read += static_cast<uint32_t>(n);
            if (c->have_read == c->msg_len) {
                ++c->discarded_frames; // caller logs the error
                c->rx_state = RX_HEADER;
                c->have_read = 0;
            }
            return rx_result::kProgress;
        }
        }
    }
}

} // namespace proto
} // namespace kas
