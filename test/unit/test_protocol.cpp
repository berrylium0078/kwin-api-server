// SPDX-License-Identifier: MIT
// Unit tests for the pure socket protocol layer (daemon/src/proto).
//
// A socketpair() stands in for a real client connection: one end drives the
// `Client` session (the daemon side), the other end acts as the peer that
// writes raw frames. The protocol itself has no D-Bus and no libsystemd, so
// these tests run without an event loop.

#include "protocol.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "test_main.hpp" // TEST / CHECK / CHECK_EQ

namespace {

using kas::proto::Client;
using kas::proto::read_result;
using kas::proto::RxBuffer;
using kas::proto::TxBuffer;

// --- helpers ----------------------------------------------------------------

int make_socketpair(int sv[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv);
}

// Append one frame ([uint32 native length][payload]) to a wire buffer.
void frame(std::string& wire, const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    wire.append(reinterpret_cast<const char*>(&len), sizeof(len));
    wire.append(payload);
}

// Write everything to the peer, pumping the session whenever the socket
// buffer fills up (non-blocking, so large frames need interleaved reads).
void write_all(Client* c, int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // socket buffer full: drain the other side, then retry
            while (c->read() == read_result::progress) {
            }
            continue;
        }
        CHECK(false); // unexpected write error
    }
}

// Drain the session until it reports again (nothing more to read).
read_result pump(Client* c) {
    read_result r;
    do {
        r = c->read();
    } while (r == read_result::progress);
    return r;
}

// Copy the poll() result out and drain the queue.
std::string poll_and_drain(RxBuffer* rx) {
    size_t n = 0;
    const char* s = rx->poll(&n);
    std::string out(s, n);
    rx->drain();
    return out;
}

// Feed one full frame from the peer and drain the session.
void feed_frame(Client* c, int fd, const std::string& payload) {
    std::string wire;
    frame(wire, payload);
    write_all(c, fd, wire.data(), wire.size());
    pump(c);
}

} // namespace

// --- module 2: RX buffer splice --------------------------------------------

TEST(rx_buffer_splice) {
    RxBuffer rx(64);
    CHECK_EQ(rx.append("aa", 2), 0);
    CHECK_EQ(rx.append("bb", 2), 0);
    CHECK_EQ(rx.append("cc", 2), 0);
    CHECK_EQ(rx.messages(), 3u);
    CHECK_EQ(poll_and_drain(&rx), std::string("[aa,bb,cc]"));
    CHECK_EQ(rx.messages(), 0u);
    // empty -> "[]"
    size_t n = 0;
    const char* s = rx.poll(&n);
    CHECK_EQ(std::string(s, n), std::string("[]"));
}

TEST(rx_buffer_append_enobufs) {
    RxBuffer rx(16);
    // '[' (1) + per message (payload + ','): "abcdef" (6) + 1 = 7.
    // 1 + 7 = 8 ok; 8 + 7 = 15 ok; 15 + 7 = 22 > 16 -> -ENOBUFS.
    CHECK_EQ(rx.append("abcdef", 6), 0);
    CHECK_EQ(rx.append("abcdef", 6), 0);
    CHECK_EQ(rx.append("abcdef", 6), -ENOBUFS);
}

// --- module 3: TX buffer ---------------------------------------------------

TEST(tx_buffer_push_limits) {
    TxBuffer tx(32);
    const char* msg = "hello";
    // frame = 4 + 5 = 9 bytes; three fit (27 <= 32), the fourth does not.
    CHECK_EQ(tx.push(msg, 5), 0);
    CHECK_EQ(tx.push(msg, 5), 0);
    CHECK_EQ(tx.push(msg, 5), 0);
    CHECK_EQ(tx.push(msg, 5), -ENOBUFS);
    // oversized message
    std::string big(kas::proto::kMaxMessageLen + 1, 'x');
    CHECK_EQ(tx.push(big.data(), big.size()), -EMSGSIZE);
}

// --- module 1: frame decoder (state machine) -------------------------------

TEST(proto_header_read_full) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // Write the whole 4-byte header in one go; one read() consumes it.
    uint32_t len = 5;
    CHECK_EQ(::write(sv[1], &len, sizeof(len)), static_cast<ssize_t>(sizeof(len)));
    CHECK_EQ(c.read(), read_result::progress);
    CHECK(c.rx_state() == Client::state::payload); // header fully consumed
    CHECK_EQ(c.msg_len(), 5u);
    CHECK_EQ(c.have_read(), 0u); // reset for the payload stage

    // now the payload arrives and the message completes
    CHECK_EQ(::write(sv[1], "hello", 5), 5);
    CHECK_EQ(pump(&c), read_result::again);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[hello]"));
}

TEST(proto_header_split_1_1_1_1) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    uint32_t len = 5;
    const char* bytes = reinterpret_cast<const char*>(&len);
    for (size_t i = 0; i < sizeof(len); ++i) {
        CHECK_EQ(::write(sv[1], bytes + i, 1), 1);
        CHECK_EQ(c.read(), read_result::progress);
        if (i + 1 < sizeof(len)) {
            CHECK_EQ(c.have_read(), static_cast<uint32_t>(i + 1)); // partial header
        }
    }
    // header complete after the 4th byte
    CHECK(c.rx_state() == Client::state::payload);
    CHECK_EQ(c.msg_len(), 5u);
    CHECK_EQ(c.have_read(), 0u);

    CHECK_EQ(::write(sv[1], "hello", 5), 5);
    CHECK_EQ(pump(&c), read_result::again);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[hello]"));
}

TEST(proto_header_split_2_2) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    uint32_t len = 5;
    const char* bytes = reinterpret_cast<const char*>(&len);
    CHECK_EQ(::write(sv[1], bytes, 2), 2);
    CHECK_EQ(c.read(), read_result::progress);
    CHECK_EQ(c.have_read(), 2u);
    CHECK_EQ(::write(sv[1], bytes + 2, 2), 2);
    CHECK_EQ(c.read(), read_result::progress);
    CHECK(c.rx_state() == Client::state::payload);
    CHECK_EQ(c.msg_len(), 5u);

    CHECK_EQ(::write(sv[1], "hello", 5), 5);
    CHECK_EQ(pump(&c), read_result::again);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[hello]"));
}

TEST(proto_payload_split) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // header first
    uint32_t len = 6;
    CHECK_EQ(::write(sv[1], &len, sizeof(len)), static_cast<ssize_t>(sizeof(len)));
    CHECK_EQ(c.read(), read_result::progress);

    // payload in 2-byte chunks
    const char* payload = "abcdef";
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(::write(sv[1], payload + i * 2, 2), 2);
        CHECK_EQ(c.read(), read_result::progress);
    }
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK(c.rx_state() == Client::state::header);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[abcdef]"));
}

TEST(proto_multiple_frames_one_write) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // Two complete frames in a single write().
    std::string wire;
    frame(wire, "one");
    frame(wire, "two");
    CHECK_EQ(::write(sv[1], wire.data(), wire.size()),
             static_cast<ssize_t>(wire.size()));

    CHECK_EQ(pump(&c), read_result::again);
    CHECK_EQ(c.rx().messages(), 2u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[one,two]"));
}

TEST(proto_empty_json_values) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // "Empty" JSON values are still non-empty payloads: null, 0, "", [].
    std::string wire;
    frame(wire, "null");
    frame(wire, "0");
    frame(wire, "\"\"");
    frame(wire, "[]");
    CHECK_EQ(::write(sv[1], wire.data(), wire.size()),
             static_cast<ssize_t>(wire.size()));

    CHECK_EQ(pump(&c), read_result::again);
    CHECK_EQ(c.rx().messages(), 4u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[null,0,\"\",[]]"));
}

TEST(proto_near_one_megabyte) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    std::string payload(kas::proto::kMaxMessageLen - 1, 'a');
    feed_frame(&c, sv[1], payload);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[") + payload + "]");
}

TEST(proto_exactly_one_megabyte) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    std::string payload(kas::proto::kMaxMessageLen, 'a');
    feed_frame(&c, sv[1], payload);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[") + payload + "]");
}

TEST(proto_over_one_megabyte_discarded) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // 1 MB + 1: consume and discard the whole frame, keep the connection.
    std::string big(kas::proto::kMaxMessageLen + 1, 'x');
    feed_frame(&c, sv[1], big);
    CHECK_EQ(c.discarded_frames(), 1u);
    CHECK_EQ(c.rx().messages(), 0u);

    // connection still alive: a normal frame is delivered afterwards
    feed_frame(&c, sv[1], "ok");
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[ok]"));
}

TEST(proto_oversized_then_normal) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // Oversized frame immediately followed by a normal frame in one burst.
    std::string wire;
    frame(wire, std::string(kas::proto::kMaxMessageLen + 1, 'x'));
    frame(wire, "ok");
    write_all(&c, sv[1], wire.data(), wire.size());
    pump(&c);

    CHECK_EQ(c.discarded_frames(), 1u);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[ok]"));
}

TEST(proto_rx_buffer_full_pauses) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    // tiny RX buffer: '[' (1) + (6 payload + 1 ',') per message = 7 bytes each
    Client c(sv[0], /*rx_cap=*/32);

    // 4 messages fit (1 + 4*7 = 29 <= 32); a 5th cannot (29 + 7 = 36 > 32).
    std::string wire;
    for (int i = 0; i < 4; ++i) {
        frame(wire, "abcdef");
    }
    write_all(&c, sv[1], wire.data(), wire.size());
    pump(&c);
    CHECK_EQ(c.rx().messages(), 4u);

    // The 5th frame's header is readable, but the payload must NOT be
    // consumed while the RX buffer is full.
    std::string fifth;
    frame(fifth, "abcdef");
    write_all(&c, sv[1], fifth.data(), fifth.size());
    CHECK_EQ(c.read(), read_result::paused);
    CHECK(c.rx_paused());
    CHECK_EQ(c.rx().messages(), 4u); // nothing consumed

    // drain via poll() -> resume -> the pending frame is delivered
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[abcdef,abcdef,abcdef,abcdef]"));
    c.rx_resume();
    CHECK_EQ(pump(&c), read_result::again);
    CHECK_EQ(c.rx().messages(), 1u);
    CHECK_EQ(poll_and_drain(&c.rx()), std::string("[abcdef]"));
}

TEST(proto_tx_buffer_full) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0], 0, /*tx_cap=*/16);

    // frame = 4 + 5 = 9; one fits, the second does not (9 + 9 = 18 > 16).
    const char* msg = "hello";
    CHECK_EQ(c.push(msg, 5), 0);
    CHECK_EQ(c.push(msg, 5), -ENOBUFS);
    CHECK(c.tx().pending());
}

TEST(proto_peer_close) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    ::close(sv[1]);
    CHECK_EQ(c.read(), read_result::eof);
    CHECK_EQ(c.read(), read_result::eof);
}

TEST(proto_eagain) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    // nothing written: non-blocking read reports EAGAIN
    CHECK_EQ(c.read(), read_result::again);
}

TEST(proto_tx_flush_roundtrip) {
    int sv[2];
    CHECK_EQ(make_socketpair(sv), 0);
    Client c(sv[0]);

    CHECK_EQ(c.push("hello", 5), 0);
    CHECK_EQ(c.push("world", 5), 0);
    CHECK(c.tx().pending());

    ssize_t n = c.flush();
    CHECK(n > 0);
    CHECK(!c.tx().pending());

    // peer receives [5]hello[5]world
    char buf[32];
    std::string got;
    for (;;) {
        ssize_t r = ::read(sv[1], buf, sizeof(buf));
        if (r > 0) {
            got.append(buf, static_cast<size_t>(r));
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    CHECK_EQ(got, std::string("\x05\x00\x00\x00hello\x05\x00\x00\x00world", 18));
}
