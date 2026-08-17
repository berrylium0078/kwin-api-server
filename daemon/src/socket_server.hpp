// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <systemd/sd-event.h>

namespace kas {

// Unix-domain socket server driven by sd-event. The listening socket and every
// accepted client connection get their own sd_event_source, so read/write on
// any number of clients is multiplexed concurrently (non-blocking, single
// thread) from the *same* event loop that also drives the D-Bus connection
// (sd-bus is attached to the same sd_event).
//
// The socket file (<work_dir>/service.socket) is created by bind() and removed
// on stop(). While the daemon runs as a systemd service the file lives inside
// RuntimeDirectory=%t/kwin-api-server, so systemd also cleans it up.
//
// Line protocol (useful for probing / debugging, one command per line):
//   ping    -> "pong"
//   status  -> status line (see set_status_callback)
//   quit    -> server closes the connection
//   <other> -> "echo <other>" (the line is echoed back)
class SocketServer {
public:
    SocketServer(std::filesystem::path socket_path, int max_clients);
    ~SocketServer();

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    // Bind + listen. Returns 0 on success, negative errno-style code on failure.
    int bind_and_listen();

    // Add the listening socket to the event loop. Returns 0 / negative errno.
    int start(sd_event* event);

    // Close every client, stop listening and remove the socket file.
    void stop();

    const std::string& socket_path() const { return socket_path_; }
    int listen_fd() const { return listen_fd_; }
    int connected_clients() const { return static_cast<int>(clients_.size()); }

    // Callback used to answer the "status" command.
    void set_status_callback(std::function<std::string()> callback) {
        status_callback_ = std::move(callback);
    }

private:
    struct Client {
        int fd = -1;
        sd_event_source* io_source = nullptr;
        std::string read_buf;
        std::string write_buf;
        bool closing = false;
    };

    static int on_accept(sd_event_source* source, int fd, uint32_t revents, void* userdata);
    static int on_client_io(sd_event_source* source, int fd, uint32_t revents, void* userdata);

    int accept_clients();
    int handle_client_io(Client& client, uint32_t revents);
    void process_lines(Client& client);
    void queue_response(Client& client, std::string data);
    void flush_writes(Client& client);
    void close_client(Client& client);

    std::string socket_path_;
    int listen_fd_ = -1;
    int max_clients_ = 64;
    sd_event* event_ = nullptr;
    sd_event_source* listen_source_ = nullptr;
    std::unordered_map<int, std::unique_ptr<Client>> clients_;
    std::function<std::string()> status_callback_;
};

} // namespace kas
