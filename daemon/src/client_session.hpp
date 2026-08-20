// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "client_dbus_object.hpp"
#include "proto/protocol.hpp"

namespace kas {

class Server;

// One accepted client connection. Owns the protocol `Client` (fd + RX/TX
// buffers), the per-client D-Bus object /cli{id} (ClientDBusObject) and the
// sd-event IO source multiplexing reads and writes on the same loop.
//
// Read path: drains the socket into the RX buffer; a pending /cli{id} poll()
// is answered as soon as messages arrive. If the RX buffer fills up
// (client.read() -> paused), socket reads are suspended until a poll() drains
// the buffer (on_rx_drained()).
//
// Write path: push() (script -> client) appends a frame to the TX queue and
// arms EPOLLOUT; the writable callback flushes until EAGAIN.
//
// On EOF/error the session notifies the Server, which logs the disconnect
// event and destroys the session.
class ClientSession {
public:
    // rx_buffer_cap / tx_buffer_cap: per-client RX/TX buffer capacities in
    // bytes; 0 -> the protocol default (16 MB, see proto::Client).
    ClientSession(uint64_t id, int fd, sd_event* event, sd_bus* bus, Server* server,
                  size_t rx_buffer_cap = 0, size_t tx_buffer_cap = 0);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    uint64_t id() const { return id_; }
    Server* server() const { return server_; }
    int fd() const { return fd_; }
    proto::Client& client() { return client_; }

    // False if the session could not be fully set up (e.g. no io source);
    // the caller should drop it immediately.
    bool ok() const { return io_source_ != nullptr; }

    // Queue one message for this client's socket (script -> client).
    // Returns 0 / -EMSGSIZE / -ENOBUFS / -ENOTCONN.
    int push(const char* msg, size_t len);

    // Called after a poll() drained the RX buffer: resume socket reads if
    // they were paused because the buffer was full.
    void on_rx_drained();

private:
    static int on_io(sd_event_source* source, int fd, uint32_t revents, void* userdata);
    // Returns true if the connection should be closed.
    bool handle_io(uint32_t revents);
    void update_io_events();

    uint64_t id_;
    Server* server_ = nullptr;
    sd_event* event_ = nullptr;
    int fd_ = -1;
    sd_event_source* io_source_ = nullptr;
    proto::Client client_;
    std::unique_ptr<ClientDBusObject> dbus_;
    bool rx_paused_ = false;
    uint64_t discarded_seen_ = 0; // oversized frames already logged
};

} // namespace kas
