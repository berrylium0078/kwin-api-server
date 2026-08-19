// SPDX-License-Identifier: MIT
#pragma once

// Phase 1: pure socket protocol layer — no D-Bus, no libsystemd.
//
// Three C++ classes (see doc/PROTOCOL.md for the frozen semantics):
//   1. Client    frame decoder + session: byte stream -> zero or more JSON
//                message payloads (explicit state machine: header / payload /
//                discard_oversized)
//   2. RxBuffer  RX message queue: stores payloads as a JSON array splice
//                ("[msg1,msg2,") without parsing; poll() turns the trailing
//                ',' into ']'
//   3. TxBuffer  TX queue: complete frames "[len][payload][len]..." to be
//                flushed to the socket; circular buffer (flushed space is
//                reused immediately)
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

// Result of one Client::read() call.
enum class read_result {
    progress, // read some bytes; call again while the socket is readable
    again,    // EAGAIN/EWOULDBLOCK: stop until the socket is readable again
    paused,   // RX buffer full: stop reading until poll() drains, then resume
    eof,      // peer closed the connection
    error,    // read error (errno set)
};

// ---------------------------------------------------------------------------
// Module 2: RX message queue
// ---------------------------------------------------------------------------
class RxBuffer {
public:
    // Allocates `capacity` bytes (throws std::bad_alloc on OOM,
    // std::invalid_argument if capacity < 2). Capacity is <= 16 MB.
    explicit RxBuffer(size_t capacity = kDefaultBufferCap);
    ~RxBuffer();

    RxBuffer(const RxBuffer&) = delete;
    RxBuffer& operator=(const RxBuffer&) = delete;

    // Can a payload of `payload_len` bytes (+ trailing ',') still fit?
    bool can_fit(size_t payload_len) const;

    // Copy one complete payload into the queue (convenience; the decoder
    // writes zero-copy via tail()/advance() and calls message_complete()).
    // Returns 0 / -ENOBUFS.
    int append(const char* payload, size_t len);
    // Append ',' after a payload the decoder wrote directly at the tail.
    void message_complete();

    // Zero-copy write slot used by the frame decoder: write payload bytes at
    // tail(), then advance(n), then message_complete().
    char* tail();
    void advance(size_t n);

    // Borrowed pointer + length of the JSON array string ("[]" when empty).
    // Valid until the next append or drain. Mutates the trailing ',' into ']'.
    const char* poll(size_t* out_len);
    // Drop the complete messages (call after copying the poll() result out).
    void drain();

    unsigned messages() const { return messages_; }
    size_t len() const { return len_; }
    size_t capacity() const { return capacity_; }

private:
    char* data_ = nullptr;      // owned buffer of capacity_ bytes
    size_t len_ = 1;            // bytes used; data_[0] is always '['
    size_t capacity_ = 0;       // <= 16 MB
    size_t complete_len_ = 1;   // end of the complete-message prefix
    unsigned messages_ = 0;     // complete messages queued
};

// ---------------------------------------------------------------------------
// Module 3: TX queue
// ---------------------------------------------------------------------------
class TxBuffer {
public:
    // Allocates `capacity` bytes (throws std::bad_alloc on OOM,
    // std::invalid_argument if capacity < kHeaderLen + 1).
    explicit TxBuffer(size_t capacity = kDefaultBufferCap);
    ~TxBuffer();

    TxBuffer(const TxBuffer&) = delete;
    TxBuffer& operator=(const TxBuffer&) = delete;

    // Queue one message payload as a complete frame. Returns 0 / -EMSGSIZE
    // (> 1 MB) / -ENOBUFS (TX buffer full).
    int push(const char* msg, size_t len);
    // Flush queued bytes to the socket; returns bytes written (>= 0, stops on
    // EAGAIN) or a negative errno on error. Resets once everything is flushed.
    ssize_t flush(int fd);

    // Bytes queued but not yet flushed to the socket.
    bool pending() const { return len_ > 0; }
    size_t len() const { return len_; }
    size_t capacity() const { return capacity_; }

private:
    // Copy `n` bytes into the ring at the write position, wrapping around at
    // `capacity_`; `len_` grows accordingly. The caller has verified that the
    // frame fits (len_ + n <= capacity_).
    void write_ring(const char* src, size_t n);

    // Ring buffer: `head_` is the next byte to flush, `tail_` the next byte
    // to write, `len_` the queued (not yet flushed) bytes. Wrapping means
    // space freed by a partial flush is immediately reusable — a linear
    // buffer would have to wait until everything was drained.
    char* data_ = nullptr;      // owned buffer of capacity_ bytes
    size_t capacity_ = 0;
    size_t head_ = 0;           // read position (next byte to send)
    size_t tail_ = 0;           // write position (next byte to append)
    size_t len_ = 0;            // queued bytes
};

// ---------------------------------------------------------------------------
// Client session — ties the three modules together
// ---------------------------------------------------------------------------
class Client {
public:
    // Stage of the RX frame decoder (exposed for tests/logging).
    enum class state { header, payload, discard_oversized };

    // `fd` is owned by the caller; the client only reads/writes it. rx_cap /
    // tx_cap 0 -> kDefaultBufferCap. Throws std::bad_alloc on OOM.
    Client(int fd, size_t rx_cap = 0, size_t tx_cap = 0);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // One non-blocking read step; see read_result. When paused is returned
    // the frame is NOT consumed: the caller must stop reading until poll()
    // has drained the RX buffer and rx_resume() has been called.
    read_result read();
    // Clear the RX-buffer pause; call after poll() drained the queue. The
    // next read() re-checks whether the pending frame now fits.
    void rx_resume();

    // Queue one message for this client's socket (see TxBuffer::push).
    // Returns -ENOTCONN if the client is gone (fd < 0).
    int push(const char* msg, size_t len);
    // Flush the TX queue to the socket (see TxBuffer::flush).
    ssize_t flush();

    // --- accessors (tests / daemon) ---
    int fd() const { return fd_; }
    state rx_state() const { return rx_state_; }
    uint32_t msg_len() const { return msg_len_; }
    uint32_t have_read() const { return have_read_; }
    bool rx_paused() const { return rx_paused_; }
    uint64_t discarded_frames() const { return discarded_frames_; }
    RxBuffer& rx() { return rx_; }
    const RxBuffer& rx() const { return rx_; }
    TxBuffer& tx() { return tx_; }
    const TxBuffer& tx() const { return tx_; }

private:
    int fd_ = -1;
    state rx_state_ = state::header;
    // If header: byte buffer for the length header (native endian);
    // otherwise: the payload length announced by the header. (uint32_t, not
    // the sketch's int32_t, because the wire header is a uint32.)
    uint32_t msg_len_ = 0;
    uint32_t have_read_ = 0;      // bytes consumed in the current stage
    bool rx_paused_ = false;      // RX buffer full; socket reads suspended
    uint64_t discarded_frames_ = 0; // oversized frames dropped (caller logs)
    RxBuffer rx_;
    TxBuffer tx_;
};

} // namespace proto
} // namespace kas
