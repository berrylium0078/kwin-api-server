// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include <systemd/sd-bus.h>

namespace kas {

struct Config;

// The daemon's own D-Bus service on the session bus. It opens an sd-bus
// connection (sd_bus_open_user: honors $DBUS_SESSION_BUS_ADDRESS and falls
// back to $XDG_RUNTIME_DIR/bus), requests the well-known name configured via
// KWIN_API_SERVICE_NAME and serves two objects: the derived object path (a
// Status() method, see object_path()) and "/daemon" (log(level, msg), used by
// the loaded KWin script to write journal lines). The connection is attached
// to the shared sd-event loop, so one loop drives the unix socket *and*
// D-Bus. The name is released automatically when the connection closes (and
// the session bus cleans up on logout).
class DbusService {
public:
    explicit DbusService(const Config& config);
    ~DbusService();

    DbusService(const DbusService&) = delete;
    DbusService& operator=(const DbusService&) = delete;

    // sd_bus_open_user + bus configuration. Returns 0 / negative errno.
    int open();
    // sd_bus_request_name. Returns 0 / negative errno.
    int request_name();
    // Attach the bus to the event loop and install the object vtable.
    // Returns 0 / negative errno.
    int attach(sd_event* event);

    // Detach from the event loop (required before synchronous sd_bus_* calls
    // once the loop has exited).
    void detach();

    sd_bus* bus() { return bus_; }
    const std::string& service_name() const { return service_name_; }
    const std::string& object_path() const { return object_path_; }

    // Detach from the event loop and close the connection (releases the name).
    void close();

public:
    // sd-bus vtable / match callbacks (public so the vtable table in the .cpp
    // can reference them).
    static int method_status(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int method_log(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int on_name_lost(sd_bus_message* message, void* userdata, sd_bus_error* error);

private:
    sd_bus* bus_ = nullptr;
    sd_event* event_ = nullptr;
    std::string service_name_;
    std::string object_path_;
    std::string status_;
};

} // namespace kas
