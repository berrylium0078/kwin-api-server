// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

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

int tx_buffer_init(tx_buffer* tx, size_t capacity) {
    if (!tx || capacity < kHeaderLen + 1) {
        return -EINVAL;
    }
    tx->data = static_cast<char*>(std::malloc(capacity));
    if (!tx->data) {
        return -ENOMEM;
    }
    tx->len = 0;
    tx->offset = 0;
    tx->capacity = capacity;
    return 0;
}

void tx_buffer_free(tx_buffer* tx) {
    if (!tx) {
        return;
    }
    std::free(tx->data);
    tx->data = nullptr;
    tx->len = 0;
    tx->offset = 0;
    tx->capacity = 0;
}

int tx_buffer_push(tx_buffer* tx, const char* msg, size_t len) {
    if (len > kMaxMessageLen) {
        return -EMSGSIZE; // single-message maximum exceeded
    }
    if (tx->len + kHeaderLen + len > tx->capacity) {
        return -ENOBUFS; // TX buffer full: cannot hold this frame
    }
    const uint32_t wire_len = static_cast<uint32_t>(len);
    std::memcpy(tx->data + tx->len, &wire_len, kHeaderLen);
    std::memcpy(tx->data + tx->len + kHeaderLen, msg, len);
    tx->len += kHeaderLen + len;
    return 0;
}

ssize_t tx_buffer_flush(tx_buffer* tx, int fd) {
    ssize_t written = 0;
    while (tx->offset < tx->len) {
        const size_t remaining = tx->len - tx->offset;
        ssize_t n = ::send(fd, tx->data + tx->offset, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            tx->offset += static_cast<size_t>(n);
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
    if (tx->offset == tx->len) {
        tx->len = 0;
        tx->offset = 0;
    }
    return written;
}

bool tx_buffer_pending(const tx_buffer* tx) {
    return tx->len > tx->offset;
}

int client_push(client* c, const char* msg, size_t len) {
    if (!c || c->fd < 0) {
        return -ENOTCONN;
    }
    return tx_buffer_push(&c->tx, msg, len);
}

ssize_t client_flush(client* c) {
    if (!c || c->fd < 0) {
        return -ENOTCONN;
    }
    return tx_buffer_flush(&c->tx, c->fd);
}

} // namespace proto
} // namespace kas
