// SPDX-License-Identifier: MIT
#include "socket_server.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.hpp"

namespace kas {

namespace {

constexpr size_t kReadChunk = 4096;
constexpr size_t kMaxReadBuf = 64 * 1024;

std::string strerror_msg(int err) {
    return std::strerror(err);
}

} // namespace

SocketServer::SocketServer(std::filesystem::path socket_path, int max_clients)
    : socket_path_(socket_path.string()), max_clients_(max_clients > 0 ? max_clients : 64) {}

SocketServer::~SocketServer() { stop(); }

int SocketServer::bind_and_listen() {
    if (listen_fd_ >= 0) {
        return 0;
    }

    // Remove a stale socket file from a previous (non-systemd) run. Under
    // systemd the whole RuntimeDirectory is fresh, so this is normally a no-op.
    ::unlink(socket_path_.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_error("socket(): " + strerror_msg(errno));
        return -errno;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        log_error("socket path too long: " + socket_path_);
        ::close(fd);
        return -ENAMETOOLONG;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_error("bind(" + socket_path_ + "): " + strerror_msg(errno));
        ::close(fd);
        return -errno;
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        log_error("listen(): " + strerror_msg(errno));
        ::close(fd);
        return -errno;
    }
    listen_fd_ = fd;
    return 0;
}

int SocketServer::start(sd_event* event) {
    event_ = event;
    int r = bind_and_listen();
    if (r < 0) {
        return r;
    }
    r = sd_event_add_io(event, &listen_source_, listen_fd_, EPOLLIN, on_accept, this);
    if (r < 0) {
        log_error("sd_event_add_io(listen): " + strerror_msg(-r));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return r;
    }
    return 0;
}

void SocketServer::stop() {
    // Close every client connection.
    while (!clients_.empty()) {
        close_client(*clients_.begin()->second);
    }
    // Stop listening.
    if (listen_source_) {
        sd_event_source_set_enabled(listen_source_, SD_EVENT_OFF);
        sd_event_source_unref(listen_source_);
        listen_source_ = nullptr;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    // The socket file is owned by the daemon's working directory; systemd
    // removes the RuntimeDirectory on stop. We remove the file ourselves too
    // so manual runs do not leave stale sockets behind.
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
}

int SocketServer::on_accept(sd_event_source* /*source*/, int /*fd*/, uint32_t /*revents*/,
                            void* userdata) {
    return static_cast<SocketServer*>(userdata)->accept_clients();
}

int SocketServer::accept_clients() {
    for (;;) {
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; // drained
            }
            if (errno == EINTR) {
                continue;
            }
            // EMFILE/ENFILE etc.: log and stop accepting for now; the next
            // EPOLLIN on the listener will retry.
            log_warn("accept(): " + strerror_msg(errno));
            return 0;
        }

        if (static_cast<int>(clients_.size()) >= max_clients_) {
            log_warn("client rejected: connection limit (" + std::to_string(max_clients_) +
                     ") reached");
            ::close(client_fd);
            continue;
        }

        auto client = std::make_unique<Client>();
        client->fd = client_fd;

        // userdata is `this` (the server), never the client: the io source may
        // fire once more with a stale fd after close_client() ran, and looking
        // the client up by fd keeps that safe.
        sd_event_source* source = nullptr;
        int r = sd_event_add_io(event_, &source, client_fd, EPOLLIN, on_client_io, this);
        if (r < 0) {
            log_error("sd_event_add_io(client): " + strerror_msg(-r));
            ::close(client_fd);
            return 0;
        }
        client->io_source = source;
        log_debug("client connected: fd=" + std::to_string(client_fd));
        clients_.emplace(client_fd, std::move(client));
    }
}

int SocketServer::on_client_io(sd_event_source* /*source*/, int fd, uint32_t revents,
                               void* userdata) {
    auto* server = static_cast<SocketServer*>(userdata);
    auto it = server->clients_.find(fd);
    if (it == server->clients_.end()) {
        return 0; // already closed
    }
    return server->handle_client_io(*it->second, revents);
}

int SocketServer::handle_client_io(Client& client, uint32_t revents) {
    bool readable = (revents & EPOLLIN) != 0;
    bool hangup = (revents & (EPOLLHUP | EPOLLERR)) != 0;
    bool saw_eof = false;

    // 1) drain as much input as is available. Note: on EOF we do NOT close
    //    immediately — the peer may have written data right before closing
    //    (e.g. "quit\n" + shutdown), so process what we have first.
    if (readable) {
        for (;;) {
            char buf[kReadChunk];
            ssize_t n = ::read(client.fd, buf, sizeof(buf));
            if (n > 0) {
                client.read_buf.append(buf, static_cast<size_t>(n));
                if (client.read_buf.size() > kMaxReadBuf) {
                    log_warn("client fd=" + std::to_string(client.fd) +
                             " exceeded read buffer, closing");
                    close_client(client);
                    return 0;
                }
                continue;
            }
            if (n == 0) {
                saw_eof = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            log_debug("read(): " + strerror_msg(errno));
            close_client(client);
            return 0;
        }
    }

    // 2) handle complete lines, which may queue responses ("quit" sets the
    //    closing flag so no further lines are processed)
    if (!client.closing) {
        process_lines(client);
    }

    // 3) push whatever is queued — also on the way out, so replies queued
    //    before a "quit" line are flushed before the connection is closed
    flush_writes(client);

    if (client.closing || saw_eof) {
        close_client(client);
        return 0;
    }

    // 4) after this callback closes, if there are bytes queued we must be
    //    woken for EPOLLOUT; otherwise we only wait for more input.
    uint32_t events = EPOLLIN;
    if (!client.write_buf.empty()) {
        events |= EPOLLOUT;
    }
    if (hangup && client.write_buf.empty()) {
        close_client(client);
        return 0;
    }
    sd_event_source_set_io_events(client.io_source, events);
    return 0;
}

void SocketServer::process_lines(Client& client) {
    for (;;) {
        size_t nl = client.read_buf.find('\n');
        if (nl == std::string::npos) {
            return;
        }
        std::string line = client.read_buf.substr(0, nl);
        client.read_buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line == "ping") {
            queue_response(client, "pong\n");
        } else if (line == "status") {
            queue_response(client,
                           (status_callback_ ? status_callback_() : std::string("ok")) + "\n");
        } else if (line == "quit") {
            client.closing = true;
            return;
        } else {
            queue_response(client, "echo " + line + "\n");
        }
    }
}

void SocketServer::queue_response(Client& client, std::string data) {
    client.write_buf += data;
}

void SocketServer::flush_writes(Client& client) {
    while (!client.write_buf.empty()) {
        ssize_t n = ::send(client.fd, client.write_buf.data(), client.write_buf.size(),
                           MSG_NOSIGNAL);
        if (n > 0) {
            client.write_buf.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; // wait for EPOLLOUT
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        log_debug("send(): " + strerror_msg(errno));
        client.closing = true;
        return;
    }
}

void SocketServer::close_client(Client& client) {
    int fd = client.fd;
    log_debug("client disconnected: fd=" + std::to_string(fd));
    if (client.io_source) {
        // Safe to disable/unref from within the source's own callback;
        // sd-event defers the actual free until the callback returns.
        sd_event_source_set_userdata(client.io_source, nullptr);
        sd_event_source_set_enabled(client.io_source, SD_EVENT_OFF);
        sd_event_source_unref(client.io_source);
        client.io_source = nullptr;
    }
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    client.fd = -1;
    clients_.erase(fd);
}

} // namespace kas
