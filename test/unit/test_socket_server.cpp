// SPDX-License-Identifier: MIT
// Unit tests for SocketServer: bind, the line protocol (ping/status/echo),
// concurrent clients, quit/EOF handling and cleanup of the socket file.
//
// Note: the server and the test client run in the same thread/process, so the
// client side must pump the sd-event loop while waiting for data.

#include "socket_server.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "test_main.hpp" // TEST / CHECK / CHECK_EQ

namespace {

std::filesystem::path make_temp_dir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) /
               ("kas-sock-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

int connect_client(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Pump the event loop until `predicate` is satisfied or `max_iters` elapsed.
bool pump_until(sd_event* event, int max_iters, const std::function<bool()>& predicate) {
    for (int i = 0; i < max_iters; ++i) {
        if (predicate()) {
            return true;
        }
        sd_event_run(event, 10 * 1000); // 10 ms
    }
    return predicate();
}

// Read from `fd` until EOF while continuously pumping the event loop, so the
// in-process server can make progress. Returns whatever was read.
std::string read_all_pumped(sd_event* event, int fd, int max_iters = 1000) {
    std::string data;
    char buf[256];
    for (int i = 0; i < max_iters; ++i) {
        // A small nonzero timeout is required: sd_event_run(event, 0) returns
        // without dispatching anything, so the in-process server would starve.
        sd_event_run(event, 1000); // 1 ms
        pollfd pfd{fd, static_cast<short>(POLLIN), 0};
        int pr = ::poll(&pfd, 1, 5);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                data.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                return data; // EOF: server closed the connection
            } else if (errno != EINTR && errno != EAGAIN) {
                return data;
            }
        }
    }
    return data;
}

} // namespace

TEST(socket_server_bind_and_ping) {
    auto dir = make_temp_dir();
    auto path = (dir / "service.socket").string();

    sd_event* event = nullptr;
    CHECK(sd_event_default(&event) >= 0);

    kas::SocketServer server(path, 4);
    CHECK(server.start(event) >= 0);
    CHECK(server.listen_fd() >= 0);
    CHECK(std::filesystem::exists(path));

    int fd = connect_client(path);
    CHECK(fd >= 0);
    CHECK_EQ(::write(fd, "ping\n", 5), 5);
    CHECK_EQ(::write(fd, "quit\n", 5), 5);

    std::string reply = read_all_pumped(event, fd);
    CHECK(reply.find("pong") != std::string::npos);

    ::close(fd);
    server.stop();
    CHECK(!std::filesystem::exists(path)); // socket file removed on stop()
    sd_event_unref(event);
    std::filesystem::remove_all(dir);
}

TEST(socket_server_status_and_echo) {
    auto dir = make_temp_dir();
    auto path = (dir / "service.socket").string();

    sd_event* event = nullptr;
    CHECK(sd_event_default(&event) >= 0);

    kas::SocketServer server(path, 4);
    server.set_status_callback([] { return "service=org.test.Server"; });
    CHECK(server.start(event) >= 0);

    int fd = connect_client(path);
    CHECK(fd >= 0);
    CHECK_EQ(::write(fd, "status\nquit\n", 11), 11);
    std::string reply = read_all_pumped(event, fd);
    CHECK(reply.find("service=org.test.Server") != std::string::npos);
    CHECK(reply.find("echo") == std::string::npos);

    ::close(fd);
    server.stop();
    sd_event_unref(event);
    std::filesystem::remove_all(dir);
}

TEST(socket_server_echo_unknown_line) {
    auto dir = make_temp_dir();
    auto path = (dir / "service.socket").string();

    sd_event* event = nullptr;
    CHECK(sd_event_default(&event) >= 0);

    kas::SocketServer server(path, 4);
    CHECK(server.start(event) >= 0);

    int fd = connect_client(path);
    CHECK(fd >= 0);
    CHECK_EQ(::write(fd, "hello world\nquit\n", 17), 17);
    std::string reply = read_all_pumped(event, fd);
    CHECK(reply.find("echo hello world") != std::string::npos);

    ::close(fd);
    server.stop();
    sd_event_unref(event);
    std::filesystem::remove_all(dir);
}

TEST(socket_server_concurrent_clients) {
    auto dir = make_temp_dir();
    auto path = (dir / "service.socket").string();

    sd_event* event = nullptr;
    CHECK(sd_event_default(&event) >= 0);

    kas::SocketServer server(path, 4);
    CHECK(server.start(event) >= 0);

    // two clients connected at the same time, both ping before reading
    int a = connect_client(path);
    int b = connect_client(path);
    CHECK(a >= 0);
    CHECK(b >= 0);
    CHECK(pump_until(event, 200, [&] { return server.connected_clients() == 2; }));

    CHECK_EQ(::write(a, "ping\nquit\n", 10), 10);
    CHECK_EQ(::write(b, "ping\nquit\n", 10), 10);

    std::string reply_a = read_all_pumped(event, a);
    std::string reply_b = read_all_pumped(event, b);
    CHECK(reply_a.find("pong") != std::string::npos);
    CHECK(reply_b.find("pong") != std::string::npos);

    CHECK(pump_until(event, 200, [&] { return server.connected_clients() == 0; }));

    ::close(a);
    ::close(b);
    server.stop();
    sd_event_unref(event);
    std::filesystem::remove_all(dir);
}

TEST(socket_server_eof_closes_client) {
    auto dir = make_temp_dir();
    auto path = (dir / "service.socket").string();

    sd_event* event = nullptr;
    CHECK(sd_event_default(&event) >= 0);

    kas::SocketServer server(path, 4);
    CHECK(server.start(event) >= 0);

    int fd = connect_client(path);
    CHECK(fd >= 0);
    ::close(fd); // immediate EOF

    bool closed = pump_until(event, 200, [&] { return server.connected_clients() == 0; });
    CHECK(closed);

    server.stop();
    sd_event_unref(event);
    std::filesystem::remove_all(dir);
}

TEST(socket_server_max_clients) {
    auto dir = make_temp_dir();
    auto path = (dir / "service.socket").string();

    sd_event* event = nullptr;
    CHECK(sd_event_default(&event) >= 0);

    kas::SocketServer server(path, 1);
    CHECK(server.start(event) >= 0);

    int a = connect_client(path);
    CHECK(a >= 0);
    CHECK(pump_until(event, 100, [&] { return server.connected_clients() == 1; }));

    // second client: connect() succeeds (kernel backlog) but the server
    // rejects it immediately, so it never gets a "pong"
    int b = connect_client(path);
    CHECK(b >= 0);
    CHECK_EQ(::write(b, "ping\n", 5), 5);
    std::string reply = read_all_pumped(event, b);
    CHECK(reply.find("pong") == std::string::npos);

    ::close(a);
    ::close(b);
    server.stop();
    sd_event_unref(event);
    std::filesystem::remove_all(dir);
}
