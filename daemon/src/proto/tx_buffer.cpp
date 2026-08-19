// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/socket.h> // MSG_NOSIGNAL
#include <unistd.h>

namespace kas {
namespace proto {

// The TX buffer stores complete frames back to back:
//
//     [length][payload][length][payload]...
//
// `offset` is how much of the queued bytes has already been written to the
// socket; once offset == len the buffer is reset to empty. Messages were
// validated at push() time (length <= 1 MB, capacity), so the flush path only
// needs to write and stop on EAGAIN — no re-validation.

TxBuffer::TxBuffer(size_t capacity) : capacity_(capacity) {
    if (capacity < kHeaderLen + 1) {
        throw std::invalid_argument("TxBuffer: capacity too small");
    }
    data_ = new char[capacity];
}

TxBuffer::~TxBuffer() {
    delete[] data_;
}

int TxBuffer::push(const char* msg, size_t len) {
    if (len > kMaxMessageLen) {
        return -EMSGSIZE; // single-message maximum exceeded
    }
    if (len_ + kHeaderLen + len > capacity_) {
        return -ENOBUFS; // TX buffer full: cannot hold this frame
    }
    const uint32_t wire_len = static_cast<uint32_t>(len);
    std::memcpy(data_ + len_, &wire_len, kHeaderLen);
    std::memcpy(data_ + len_ + kHeaderLen, msg, len);
    len_ += kHeaderLen + len;
    return 0;
}

ssize_t TxBuffer::flush(int fd) {
    ssize_t written = 0;
    while (offset_ < len_) {
        const size_t remaining = len_ - offset_;
        ssize_t n = ::send(fd, data_ + offset_, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            offset_ += static_cast<size_t>(n);
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
    if (offset_ == len_) {
        len_ = 0;
        offset_ = 0;
    }
    return written;
}

} // namespace proto
} // namespace kas
