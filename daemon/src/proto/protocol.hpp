// SPDX-License-Identifier: MIT
#pragma once

// Phase 1: pure socket protocol layer — no D-Bus, no libsystemd.
//
// Three modules (see doc/PROTOCOL.md for the frozen semantics):
//   1. frame decoder    byte stream -> zero or more JSON message payloads
//                       (explicit state machine: RX_HEADER / RX_PAYLOAD /
//                        RX_DISCARD_OVERSIZED)
//   2. rx_buffer        RX message queue: stores payloads as a JSON array
//                       splice ("[msg1,msg2,") without parsing; poll() turns
//                       the trailing ',' into ']'
//   3. tx_buffer        TX queue: complete frames "[len][payload][len]..." to
//                       be flushed to the socket
//
// All lengths follow the frozen protocol: 1 MB = 1,000,000 payload bytes
// (decimal), 16 MB = 16,000,000 bytes per direction (decimal), and the frame
// header is a uint32 in native byte order.

#include <cstddef>
#include <cstdint>

#include <sys/types.h> // ssize_t

namespace kas {
namespace proto {

// ---------------------------------------------------------------------------
// Frozen protocol constants
// ---------------------------------------------------------------------------
constexpr size_t kMaxMessageLen = 1'000'000;      // 1 MB payload (decimal)
constexpr size_t kHeaderLen = 4;                  // uint32 native length prefix
constexpr size_t kMaxFrameLen = kMaxMessageLen + kHeaderLen;
constexpr size_t kDefaultBufferCap = 16'000'000;  // 16 MB per direction

// ---------------------------------------------------------------------------
// Module 1: frame decoder — explicit state machine
// ---------------------------------------------------------------------------
enum rx_state {
    RX_HEADER,            // accumulating the 4-byte length header
    RX_PAYLOAD,           // accumulating payload bytes into the RX buffer
    RX_DISCARD_OVERSIZED, // consuming + discarding an oversized frame
};

// Result of one client_read() call.
enum class rx_result {
    kProgress, // read some bytes; call again while the socket is readable
    kAgain,    // EAGAIN/EWOULDBLOCK: stop until the socket is readable again
    kPaused,   // RX buffer full: stop reading until poll() drains, then resume
    kEof,      // peer closed the connection
    kError,    // read error (errno set)
};

// ---------------------------------------------------------------------------
// Module 2: RX message queue
// ---------------------------------------------------------------------------
struct rx_buffer {
    char* data = nullptr;     // owned buffer of `capacity` bytes
    size_t len = 1;           // bytes used; data[0] is always '['
    size_t capacity = 0;      // <= 16 MB
    unsigned messages = 0;    // complete messages queued
    size_t complete_len = 1;  // end of the complete-message prefix
};

int rx_buffer_init(rx_buffer* rx, size_t capacity);   // 0 / -ENOMEM / -EINVAL
void rx_buffer_free(rx_buffer* rx);
// Can a payload of `payload_len` bytes (+ trailing ',') still fit?
bool rx_buffer_can_fit(const rx_buffer* rx, size_t payload_len);
// Copy one complete payload into the queue (convenience; the decoder itself
// writes zero-copy and calls rx_buffer_message_complete instead).
int rx_buffer_append(rx_buffer* rx, const char* payload, size_t len);
// Append ',' after a payload the decoder wrote directly at the tail.
void rx_buffer_message_complete(rx_buffer* rx);
// Borrowed pointer + length of the JSON array string ("[]" when empty). Valid
// until the next append or drain. Mutates the trailing ',' into ']'.
const char* rx_buffer_poll(rx_buffer* rx, size_t* out_len);
// Drop the complete messages (call after copying the poll() result out).
void rx_buffer_drain(rx_buffer* rx);

// ---------------------------------------------------------------------------
// Module 3: TX queue
// ---------------------------------------------------------------------------
struct tx_buffer {
    char* data = nullptr;     // owned buffer of `capacity` bytes
    size_t len = 0;           // queued bytes
    size_t offset = 0;        // bytes already flushed to the socket
    size_t capacity = 0;
};

int tx_buffer_init(tx_buffer* tx, size_t capacity);  // 0 / -ENOMEM / -EINVAL
void tx_buffer_free(tx_buffer* tx);
// Queue one message payload as a complete frame. Returns 0 / -EMSGSIZE
// (> 1 MB) / -ENOBUFS (TX buffer full).
int tx_buffer_push(tx_buffer* tx, const char* msg, size_t len);
// Flush queued bytes to the socket; returns bytes written (>= 0, stops on
// EAGAIN) or a negative errno on error. Resets once everything is flushed.
ssize_t tx_buffer_flush(tx_buffer* tx, int fd);
bool tx_buffer_pending(const tx_buffer* tx);

// ---------------------------------------------------------------------------
// Client session — ties the three modules together
// ---------------------------------------------------------------------------
struct client {
    int fd = -1;
    enum rx_state rx_state = RX_HEADER;
    // If RX_HEADER: byte buffer for the length header (native endian);
    // otherwise: the payload length announced by the header. (uint32_t, not
    // the sketch's int32_t, because the wire header is a uint32.)
    uint32_t msg_len = 0;
    uint32_t have_read = 0;   // bytes consumed in the current stage
    bool rx_paused = false;   // RX buffer full; socket reads suspended
    uint64_t discarded_frames = 0; // oversized frames dropped (caller logs)
    rx_buffer rx;
    tx_buffer tx;
};

// rx_cap/tx_cap 0 -> kDefaultBufferCap (16 MB each).
int client_init(client* c, int fd, size_t rx_cap = 0, size_t tx_cap = 0);
// Free the buffers; does NOT close the fd (the caller owns it), sets fd = -1.
void client_destroy(client* c);
// One non-blocking read step; see rx_result. When kPaused is returned the
// frame is NOT consumed: the caller must stop reading until poll() has
// drained the RX buffer and client_rx_resume() has been called.
rx_result client_read(client* c);
// Clear the RX-buffer pause; call after poll() drained the queue. The next
// client_read() re-checks whether the pending frame now fits.
void client_rx_resume(client* c);
// Queue one message for this client's socket (see tx_buffer_push).
// Returns -ENOTCONN if the client is gone (fd < 0).
int client_push(client* c, const char* msg, size_t len);
ssize_t client_flush(client* c);

} // namespace proto
} // namespace kas
