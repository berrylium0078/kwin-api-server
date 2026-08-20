// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "client_session.hpp"
#include "daemon_dbus_object.hpp"

namespace kas {

// Listens on the server socket and manages every client session on the
// daemon's single sd-event loop.
//
// Each accepted connection gets an increasing positive integer id (1, 2, 3,
// ...), unique among live clients, and its own ClientSession (which registers
// the /cli{id} D-Bus object). Connect and disconnect events are logged and
// queued as control messages on the /daemon object, where the script receives
// them via poll() ({"event":"client_connected","id":N} /
// {"event":"client_disconnected","id":N}).
class Server {
public:
    // rx_buffer_cap / tx_buffer_cap: per-client RX/TX buffer capacities in
    // bytes; 0 -> the protocol default (16 MB, see proto::Client).
    Server(std::filesystem::path socket_path, int max_clients,
           size_t rx_buffer_cap = 0, size_t tx_buffer_cap = 0);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind + listen, register the /daemon D-Bus object and arm the accept
    // source. `interface_name` is the D-Bus interface served at /daemon (the
    // service name). Returns 0 / negative errno.
    int start(sd_event* event, sd_bus* bus, const std::string& interface_name);

    // Close every client session, stop listening and remove the socket file.
    void stop();

    const std::string& socket_path() const { return socket_path_; }
    int connected_clients() const { return static_cast<int>(sessions_.size()); }

    // Called by ClientSession when its connection ended (EOF/error). Logs the
    // disconnect event, queues the control message and destroys the session.
    void on_client_disconnected(uint64_t id);

private:
    static int on_accept(sd_event_source* source, int fd, uint32_t revents, void* userdata);
    int accept_clients();
    void queue_control(const char* event_name, uint64_t id);

    std::string socket_path_;
    int max_clients_ = 64;
    size_t rx_buffer_cap_ = 0;
    size_t tx_buffer_cap_ = 0;
    int listen_fd_ = -1;
    sd_event* event_ = nullptr;
    sd_bus* bus_ = nullptr;
    sd_event_source* listen_source_ = nullptr;
    uint64_t next_id_ = 1; // increasing positive client ids
    std::map<uint64_t, std::unique_ptr<ClientSession>> sessions_;
    std::unique_ptr<DaemonDBusObject> daemon_dbus_;
};

} // namespace kas
