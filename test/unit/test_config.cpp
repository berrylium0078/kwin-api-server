// SPDX-License-Identifier: MIT
#include "config.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include "test_main.hpp" // TEST / CHECK / CHECK_EQ

TEST(config_defaults_and_validation) {
    kas::Config cfg = kas::load_config({}, {});
    // default service name
    CHECK_EQ(cfg.service_name, "org.example.KwinApiServer");
    // work dir falls back to cwd
    CHECK_EQ(cfg.work_dir, std::filesystem::current_path());
    // script path and plugin name are required
    CHECK(cfg.errors.size() == 2);
    bool has_script = false, has_plugin = false;
    for (const auto& e : cfg.errors) {
        has_script = has_script || e.find("KWIN_SCRIPT_PATH") != std::string::npos;
        has_plugin = has_plugin || e.find("KWIN_PLUGIN_NAME") != std::string::npos;
    }
    CHECK(has_script);
    CHECK(has_plugin);
}

TEST(config_full_environment) {
    std::map<std::string, std::string> env = {
        {"KWIN_API_SERVICE_NAME", "org.example.Custom"},
        {"KWIN_SCRIPT_PATH", "/tmp/script.js"},
        {"KWIN_PLUGIN_NAME", "my-plugin"},
        {"KWIN_WORK_DIR", "/tmp/work"},
        {"KWIN_LOAD_RETRIES", "5"},
        {"KWIN_LOAD_RETRY_DELAY_MS", "250"},
        {"KWIN_MAX_CLIENTS", "8"},
        {"KWIN_RX_BUFFER_CAP", "65536"},
        {"KWIN_TX_BUFFER_CAP", "131072"},
        {"KWIN_DEBUG", "1"},
    };
    kas::Config cfg = kas::load_config(env, {});
    CHECK(cfg.errors.empty());
    CHECK_EQ(cfg.service_name, "org.example.Custom");
    CHECK_EQ(cfg.script_path.string(), "/tmp/script.js");
    CHECK_EQ(cfg.plugin_name, "my-plugin");
    CHECK_EQ(cfg.work_dir.string(), "/tmp/work");
    CHECK_EQ(cfg.load_retries, 5);
    CHECK_EQ(cfg.load_retry_delay_ms, 250);
    CHECK_EQ(cfg.max_clients, 8);
    CHECK_EQ(cfg.rx_buffer_cap, 65536u);
    CHECK_EQ(cfg.tx_buffer_cap, 131072u);
    CHECK(cfg.debug);
}

TEST(config_flags) {
    kas::Config check = kas::load_config({}, {"--check"});
    CHECK(check.check_only);
    CHECK(check.errors.empty()); // --check does not require script/plugin vars
    kas::Config help = kas::load_config({}, {"-h"});
    CHECK(help.show_help);
    kas::Config version = kas::load_config({}, {"--version"});
    CHECK(version.show_version);
    kas::Config unknown = kas::load_config({}, {"--nope"});
    CHECK(unknown.errors.size() == 3); // unknown arg + 2 required vars
}

TEST(config_bad_values) {
    std::map<std::string, std::string> env = {
        {"KWIN_SCRIPT_PATH", "/x.js"},
        {"KWIN_PLUGIN_NAME", "p"},
        {"KWIN_LOAD_RETRIES", "abc"},
        {"KWIN_MAX_CLIENTS", "0"},
        {"KWIN_RX_BUFFER_CAP", "1"},     // below the 64-byte minimum
        {"KWIN_TX_BUFFER_CAP", "-5"},    // negative
        {"KWIN_API_SERVICE_NAME", "org..bad"},
    };
    kas::Config cfg = kas::load_config(env, {});
    bool has_retries = false, has_clients = false, has_name = false;
    bool has_rx_cap = false, has_tx_cap = false;
    for (const auto& e : cfg.errors) {
        has_retries = has_retries || e.find("KWIN_LOAD_RETRIES") != std::string::npos;
        has_clients = has_clients || e.find("KWIN_MAX_CLIENTS") != std::string::npos;
        has_name = has_name || e.find("KWIN_API_SERVICE_NAME") != std::string::npos;
        has_rx_cap = has_rx_cap || e.find("KWIN_RX_BUFFER_CAP") != std::string::npos;
        has_tx_cap = has_tx_cap || e.find("KWIN_TX_BUFFER_CAP") != std::string::npos;
    }
    CHECK(has_retries);
    CHECK(has_clients);
    CHECK(has_name);
    CHECK(has_rx_cap);
    CHECK(has_tx_cap);
    // a failed parse must not leak a bogus capacity
    CHECK_EQ(cfg.rx_buffer_cap, 0u);
    CHECK_EQ(cfg.tx_buffer_cap, 0u);
}

TEST(dbus_name_validation) {
    CHECK(kas::is_valid_dbus_name("org.example.KwinApiServer"));
    CHECK(kas::is_valid_dbus_name("a.b"));
    CHECK(kas::is_valid_dbus_name("_private.Name"));
    CHECK(!kas::is_valid_dbus_name(""));
    CHECK(!kas::is_valid_dbus_name("no-dots"));
    CHECK(!kas::is_valid_dbus_name(".leading"));
    CHECK(!kas::is_valid_dbus_name("trailing."));
    CHECK(!kas::is_valid_dbus_name("a..b"));
    CHECK(!kas::is_valid_dbus_name("1a.b"));
}

TEST(object_path_derivation) {
    CHECK_EQ(kas::object_path_for_name("org.example.KwinApiServer"),
             "/org/example/KwinApiServer");
    CHECK_EQ(kas::object_path_for_name("com.example.Foo"), "/com/example/Foo");
}
