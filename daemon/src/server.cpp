// SPDX-License-Identifier: MIT
#include "server.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.hpp"

namespace kas {

namespace {

std::string strerror_msg(int err) {
    return std::strerror(err);
}

std::string control_message(const char* event_name, uint64_t id) {
    return std::string("{\"event\":\"") + event_name + "\",\"id\":" + std::to_string(id) + "}";
}

} // namespace

Server::Server(std::filesystem::path socket_path, int max_clients, size_t rx_buffer_cap,
               size_t tx_buffer_cap)
    : socket_path_(socket_path.string()),
      max_clients_(max_clients > 0 ? max_clients : 64),
      rx_buffer_cap_(rx_buffer_cap),
      tx_buffer_cap_(tx_buffer_cap) {}

Server::~Server() {
    stop();
}

int Server::start(sd_event* event, sd_bus* bus, const std::string& interface_name) {
    event_ = event;
    bus_ = bus;

    // Bind + listen (mirrors the former line-protocol SocketServer).
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

    int r = sd_event_add_io(event_, &listen_source_, listen_fd_, EPOLLIN, on_accept, this);
    if (r < 0) {
        log_error("sd_event_add_io(listen): " + strerror_msg(-r));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return r;
    }

    daemon_dbus_ = std::make_unique<DaemonDBusObject>(bus_, event_, interface_name);
    return 0;
}

void Server::stop() {
    // Destroy every session (closes fds, unregisters /cli{id} objects).
    while (!sessions_.empty()) {
        sessions_.erase(sessions_.begin());
    }
    daemon_dbus_.reset(); // unregister /daemon
    if (listen_source_) {
        sd_event_source_set_enabled(listen_source_, SD_EVENT_OFF);
        sd_event_source_unref(listen_source_);
        listen_source_ = nullptr;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
}

int Server::on_accept(sd_event_source* /*source*/, int /*fd*/, uint32_t /*revents*/,
                      void* userdata) {
    return static_cast<Server*>(userdata)->accept_clients();
}

int Server::accept_clients() {
    for (;;) {
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; // drained
            }
            if (errno == EINTR) {
                continue;
            }
            log_warn("accept(): " + strerror_msg(errno));
            return 0;
        }

        if (static_cast<int>(sessions_.size()) >= max_clients_) {
            log_warn("client rejected: connection limit (" + std::to_string(max_clients_) +
                     ") reached");
            ::close(client_fd);
            continue;
        }

        const uint64_t id = next_id_++;
        auto session = std::make_unique<ClientSession>(id, client_fd, event_, bus_, this,
                                                       rx_buffer_cap_, tx_buffer_cap_);
        if (!session->ok()) {
            log_error("client " + std::to_string(id) + ": session setup failed, dropping");
            continue; // ClientSession destructor closes client_fd
        }
        log_info("client connected: id=" + std::to_string(id) +
                 " fd=" + std::to_string(client_fd));
        sessions_.emplace(id, std::move(session));
        queue_control("client_connected", id);
    }
}

void Server::on_client_disconnected(uint64_t id) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return; // already removed
    }
    log_info("client disconnected: id=" + std::to_string(id));
    queue_control("client_disconnected", id);
    sessions_.erase(it); // destroys the ClientSession
}

void Server::queue_control(const char* event_name, uint64_t id) {
    if (daemon_dbus_) {
        daemon_dbus_->enqueue_control(control_message(event_name, id));
    }
}

} // namespace kas
