// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/socket.h> // MSG_NOSIGNAL
#include <unistd.h>

namespace kas {
namespace proto {

// The TX buffer is a ring buffer of complete frames back to back:
//
//     [length][payload][length][payload]...
//
//   head_  — next byte to flush (read position)
//   tail_  — next byte to append (write position)
//   len_   — bytes queued but not yet flushed
//
// Write and read positions wrap around at capacity_, so space freed by a
// partial flush is immediately reusable: a linear buffer would have to wait
// until everything was drained before any queued space came back. Messages
// were validated at push() time (length <= 1 MB, capacity), so the flush path
// only needs to write and stop on EAGAIN — no re-validation.

TxBuffer::TxBuffer(size_t capacity) : capacity_(capacity) {
    if (capacity < kHeaderLen + 1) {
        throw std::invalid_argument("TxBuffer: capacity too small");
    }
    data_ = new char[capacity];
}

TxBuffer::~TxBuffer() {
    delete[] data_;
}

void TxBuffer::write_ring(const char* src, size_t n) {
    while (n > 0) {
        const size_t chunk = std::min(n, capacity_ - tail_);
        std::memcpy(data_ + tail_, src, chunk);
        tail_ = (tail_ + chunk) % capacity_;
        src += chunk;
        n -= chunk;
        len_ += chunk;
    }
}

int TxBuffer::push(const char* msg, size_t len) {
    if (len > kMaxMessageLen) {
        return -EMSGSIZE; // single-message maximum exceeded
    }
    if (len_ + kHeaderLen + len > capacity_) {
        return -ENOBUFS; // TX buffer full: cannot hold this frame
    }
    const uint32_t wire_len = static_cast<uint32_t>(len);
    write_ring(reinterpret_cast<const char*>(&wire_len), kHeaderLen);
    write_ring(msg, len);
    return 0;
}

ssize_t TxBuffer::flush(int fd) {
    ssize_t written = 0;
    while (len_ > 0) {
        // send() needs contiguous bytes: at most up to the wrap point.
        const size_t chunk = std::min(len_, capacity_ - head_);
        ssize_t n = ::send(fd, data_ + head_, chunk, MSG_NOSIGNAL);
        if (n > 0) {
            head_ = (head_ + static_cast<size_t>(n)) % capacity_;
            len_ -= static_cast<size_t>(n);
            written += n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // stop; the writable watcher will retry
        }
        // EPIPE / ECONNRESET / EBADF etc.
        return -errno;
    }
    if (len_ == 0) {
        // Everything flushed: rewind so the next push starts at the front.
        head_ = 0;
        tail_ = 0;
    }
    return written;
}

} // namespace proto
} // namespace kas
