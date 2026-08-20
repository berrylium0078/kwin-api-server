// SPDX-License-Identifier: MIT
#include "client_session.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/epoll.h>
#include <unistd.h>

#include "log.hpp"
#include "server.hpp"

namespace kas {

namespace {

using kas::proto::read_result;

} // namespace

ClientSession::ClientSession(uint64_t id, int fd, sd_event* event, sd_bus* bus, Server* server,
                             size_t rx_buffer_cap, size_t tx_buffer_cap)
    : id_(id), server_(server), event_(event), fd_(fd), client_(fd, rx_buffer_cap, tx_buffer_cap) {
    dbus_ = std::make_unique<ClientDBusObject>(*this, bus, event, id);
    int r = sd_event_add_io(event, &io_source_, fd_, EPOLLIN, on_io, this);
    if (r < 0) {
        log_error("client " + std::to_string(id_) + ": sd_event_add_io: " + std::strerror(-r));
        io_source_ = nullptr;
    }
}

ClientSession::~ClientSession() {
    // Unregister /cli{id} and cancel any pending poll (the caller's D-Bus
    // call will time out; the object is gone, per PROTOCOL.md).
    dbus_.reset();
    if (io_source_) {
        // Safe from within the source's own callback: sd-event defers the
        // actual free until the callback returns.
        sd_event_source_set_userdata(io_source_, nullptr);
        sd_event_source_set_enabled(io_source_, SD_EVENT_OFF);
        sd_event_source_unref(io_source_);
        io_source_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int ClientSession::push(const char* msg, size_t len) {
    int r = client_.push(msg, len);
    if (r == 0) {
        update_io_events(); // arm EPOLLOUT if the TX queue became non-empty
    }
    return r;
}

void ClientSession::on_rx_drained() {
    if (rx_paused_) {
        rx_paused_ = false;
        client_.rx_resume();
        update_io_events();
    }
}

int ClientSession::on_io(sd_event_source* /*source*/, int /*fd*/, uint32_t revents,
                         void* userdata) {
    auto* self = static_cast<ClientSession*>(userdata);
    if (self->handle_io(revents)) {
        // Last use of `self`: the Server removes (and destroys) this session.
        self->server()->on_client_disconnected(self->id());
    }
    return 0;
}

bool ClientSession::handle_io(uint32_t revents) {
    bool readable = (revents & EPOLLIN) != 0;
    bool writable = (revents & EPOLLOUT) != 0;
    bool hangup = (revents & (EPOLLHUP | EPOLLERR)) != 0;
    bool close_conn = false;

    // 1) drain whatever input is available; the pending frame is NOT consumed
    //    while the RX buffer is full (kPaused).
    if (readable) {
        for (;;) {
            read_result r = client_.read();
            if (r == read_result::progress) {
                continue;
            }
            if (r == read_result::again) {
                break;
            }
            if (r == read_result::paused) {
                rx_paused_ = true;
                break; // stop reading until a poll() drains the buffer
            }
            close_conn = true; // eof / error
            break;
        }
        if (client_.rx().messages() > 0) {
            dbus_->on_rx_messages(); // answer a pending poll, if any
        }
        // The frozen protocol requires an error log for discarded frames.
        if (client_.discarded_frames() > discarded_seen_) {
            discarded_seen_ = client_.discarded_frames();
            log_warn("client " + std::to_string(id_) + ": discarded oversized frame (" +
                     std::to_string(discarded_seen_) + " total)");
        }
    }

    // 2) flush the TX queue if the socket is writable. flush() stops on
    //    EAGAIN; a hard error (EPIPE etc.) closes the connection.
    if (writable && !close_conn) {
        ssize_t n = client_.flush();
        if (n < 0) {
            close_conn = true;
        }
    }

    // 3) HUP with nothing left to write means the peer is gone.
    if (hangup && !client_.tx().pending()) {
        close_conn = true;
    }

    if (!close_conn) {
        update_io_events();
    } else {
        log_debug("client " + std::to_string(id_) + ": connection closed");
    }
    return close_conn;
}

void ClientSession::update_io_events() {
    uint32_t events = 0;
    if (!rx_paused_) {
        events |= EPOLLIN;
    }
    if (client_.tx().pending()) {
        events |= EPOLLOUT;
    }
    if (events == 0) {
        sd_event_source_set_enabled(io_source_, SD_EVENT_OFF);
    } else {
        sd_event_source_set_enabled(io_source_, SD_EVENT_ON);
        sd_event_source_set_io_events(io_source_, events);
    }
}

} // namespace kas
