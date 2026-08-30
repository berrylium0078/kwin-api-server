--!A cross-platform build utility based on Lua
--
-- kwin-api-server: a desktop background service that
--   * binds a unix socket (service.socket) in its working directory,
--   * registers a D-Bus service on the session bus,
--   * loads & runs a KWin script (built by the kwinscript TS subproject).
--
-- Everything is managed from a single sd-event loop (libsystemd: sd-bus + sd-event).
--
-- Build:   xmake f -m release && xmake            (ninja engine: xmake project -k ninja && ninja)
-- Test:    xmake test                              (unit tests)
-- Install: sudo xmake install                      (prefix /usr, see install paths below)

set_project("kwin-api-server")
set_version("0.1.0")
set_xmakever("2.9.0")

set_languages("c++20")
set_warnings("all", "extra")
-- The build directory defaults to ./build (project layout requirement);
-- override with `xmake f --builddir=<dir>` if needed.

add_rules("mode.debug", "mode.release")

-- Install prefix. `xmake install` (as root) installs to $KAS_INSTALLDIR:
--   <prefix>/bin/kwin-api-daemon
--   <prefix>/share/kwin-api-server/kwinscript.js
--   <prefix>/lib/systemd/user/kwin-api-server.service
--   <prefix>/libexec/kwin-api-server/kwinscript-unload.sh
-- Use `xmake install -o <dir>` (or --installdir / $DESTDIR) to override the
-- prefix for a given invocation.
--
-- The effective install prefix is also baked into the generated systemd unit
-- file at install time (see the kwin-api-daemon target's on_install): ExecStart,
-- ExecStopPost and KWIN_SCRIPT_PATH always follow where xmake actually
-- installs, including `-o`/--installdir overrides.
local KAS_INSTALLDIR = "/usr"
set_installdir(KAS_INSTALLDIR)

-- ---------------------------------------------------------------------------
-- kwinscript: the TypeScript subproject.
--
-- Managed by pnpm, bundled by esbuild into a single file
-- (kwinscript/dist/kwinscript.js) which the daemon installs to /usr/share.
-- Declared as a headeronly target (the ninja generator treats it like a phony
-- edge, and it is skipped in dependency edges, so `xmake project -k ninja`
-- still generates a valid build.ninja) with a custom on_build so `xmake`
-- builds the bundle before the daemon binary (kwin-api-daemon add_deps).
-- ---------------------------------------------------------------------------
target("kwinscript")
    set_kind("headeronly")
    on_build(function(target)
        local kwindir = path.join(os.projectdir(), "kwinscript")
        -- pnpm 11 keeps its settings in pnpm-workspace.yaml; the store path is
        -- machine specific, so generate it (with the esbuild build script
        -- allowed) at build time. This keeps the build working even when
        -- $HOME is not writable (containers, CI, sandboxes).
        local wsconfig = path.join(kwindir, "pnpm-workspace.yaml")
        if not os.isfile(wsconfig) then
            io.writefile(wsconfig, "allowBuilds:\n  esbuild: true\nstoreDir: " ..
                path.join(os.projectdir(), ".pnpm-store") .. "\n")
        end
        os.setenv("npm_config_cache", path.join(os.projectdir(), ".npm-cache"))
        if not os.isdir(path.join(kwindir, "node_modules")) then
            print("kwinscript: installing dependencies with pnpm ...")
            os.exec("pnpm --dir %s install", kwindir)
        end
        -- Rebuild the bundle only when a *non-git-ignored* input changed (see
        -- .gitignore: dist/, node_modules/ and the generated pnpm-workspace.yaml
        -- are ignored and never trigger a rebuild). Inputs are the sources
        -- (src/**), the kwin-ts declarations and the build config files.
        local bundle = path.join(kwindir, "dist", "kwinscript.js")
        local bundle_mtime = os.mtime(bundle) -- 0 when the bundle is missing
        local need_rebuild = bundle_mtime == 0
        if not need_rebuild then
            local inputs = {}
            for _, list in ipairs({
                os.files(path.join(kwindir, "src", "**")),
                os.files(path.join(kwindir, "kwin-ts", "**")),
                { path.join(kwindir, "tsconfig.json"),
                  path.join(kwindir, "package.json"),
                  path.join(kwindir, "pnpm-lock.yaml"),
                  path.join(kwindir, "esbuild.build.mjs") },
            }) do
                for _, file in ipairs(list) do
                    table.insert(inputs, file)
                end
            end
            for _, file in ipairs(inputs) do
                local mtime = os.mtime(file)
                if mtime > 0 and mtime >= bundle_mtime then
                    need_rebuild = true
                    break
                end
            end
        end
        if need_rebuild then
            print("kwinscript: bundling with esbuild ...")
            os.exec("pnpm --dir %s build", kwindir)
        else
            print("kwinscript: dist/kwinscript.js is up to date")
        end
    end)
    on_install(function(target)
        -- /usr/share/kwin-api-server/kwinscript.js
        local prefix = target:installdir()
        local dst = path.join(prefix, "share", "kwin-api-server")
        os.mkdir(dst)
        os.cp(path.join(os.projectdir(), "kwinscript", "dist", "kwinscript.js"),
              path.join(dst, "kwinscript.js"))
    end)
    on_uninstall(function(target)
        os.tryrm(path.join(target:installdir(), "share", "kwin-api-server", "kwinscript.js"))
    end)

-- ---------------------------------------------------------------------------
-- kwin-api-proto: the pure socket protocol layer (phase 1) — framing, RX/TX
-- queues, the frame-decoder state machine. Deliberately has NO libsystemd or
-- D-Bus dependency; the daemon's event loop integration comes in a later
-- phase. Unit-tested via socketpair in kwin-api-test.
-- ---------------------------------------------------------------------------
target("kwin-api-proto")
    set_kind("static")
    add_files("daemon/src/proto/*.cpp")
    add_includedirs("daemon/src/proto", {public = true})
    on_install(function() end)

-- ---------------------------------------------------------------------------
-- kwin-api-core: static library with the reusable parts (unit-testable).
-- Linked statically into the daemon, so nothing of it is installed.
-- ---------------------------------------------------------------------------
target("kwin-api-core")
    set_kind("static")
    add_files("daemon/src/*.cpp")
    remove_files("daemon/src/main.cpp")
    add_includedirs("daemon/src", {public = true})
    add_syslinks("systemd")
    on_install(function() end)

-- ---------------------------------------------------------------------------
-- kwin-api-daemon: the actual background service.
-- ---------------------------------------------------------------------------
target("kwin-api-daemon")
    set_kind("binary")
    set_basename("kwin-api-daemon")
    add_deps("kwin-api-core", "kwin-api-proto", "kwinscript")
    add_files("daemon/src/main.cpp")
    add_syslinks("systemd")
    on_load(function(target)
        -- expose the project version to the binary (see main.cpp --version)
        target:add("defines", string.format("KAS_VERSION=\"%s\"", tostring(target:version() or "unknown")))
    end)
    on_install(function(target)
        -- The effective install prefix: set_installdir() by default, overridden
        -- by `xmake install -o <dir>` / --installdir / $DESTDIR. xmake resolves
        -- a *relative* prefix against the project directory (regardless of the
        -- CWD the command was run from); mirror that so the paths baked into
        -- the unit file below are absolute and stable.
        local prefix = target:installdir()
        local abs_prefix = path.absolute(prefix, os.projectdir())
        -- <prefix>/bin/kwin-api-daemon
        os.cp(target:targetfile(), path.join(abs_prefix, "bin", "kwin-api-daemon"))
        -- <prefix>/lib/systemd/user/kwin-api-server.service
        --
        -- Generated here (not at build time) from the .service.in template so
        -- the paths inside it — ExecStart, ExecStopPost, KWIN_SCRIPT_PATH —
        -- always follow the *effective* install prefix above.
        local unitdir = path.join(abs_prefix, "lib", "systemd", "user")
        os.mkdir(unitdir)
        local template = io.readfile(path.join(os.projectdir(), "daemon", "systemd",
                                               "kwin-api-server.service.in"))
        assert(template, "cannot read daemon/systemd/kwin-api-server.service.in")
        local content = template
            :gsub("%${KAS_BINDIR}", path.join(abs_prefix, "bin"))
            :gsub("%${KAS_SHAREDIR}", path.join(abs_prefix, "share"))
            :gsub("%${KAS_LIBEXECDIR}", path.join(abs_prefix, "libexec"))
        io.writefile(path.join(unitdir, "kwin-api-server.service"), content)
        -- <prefix>/libexec/kwin-api-server/kwinscript-unload.sh
        local libexec = path.join(abs_prefix, "libexec", "kwin-api-server")
        os.mkdir(libexec)
        os.cp(path.join(os.projectdir(), "daemon", "systemd", "kwinscript-unload.sh"),
              path.join(libexec, "kwinscript-unload.sh"))
    end)
    on_uninstall(function(target)
        local prefix = target:installdir()
        local abs_prefix = path.absolute(prefix, os.projectdir())
        os.tryrm(path.join(abs_prefix, "bin", "kwin-api-daemon"))
        os.tryrm(path.join(abs_prefix, "lib", "systemd", "user", "kwin-api-server.service"))
        os.tryrm(path.join(abs_prefix, "libexec", "kwin-api-server", "kwinscript-unload.sh"))
    end)

-- ---------------------------------------------------------------------------
-- kwin-api-test: C++ unit tests (run with `xmake test`). Not installed.
-- ---------------------------------------------------------------------------
target("kwin-api-test")
    set_kind("binary")
    add_deps("kwin-api-core", "kwin-api-proto")
    add_files("test/unit/*.cpp")
    add_syslinks("systemd")
    add_tests("unit", {pass_outputs = ".*ALL TESTS PASSED.*"})
    on_install(function() end)

-- ---------------------------------------------------------------------------
-- kwin-api-native: the Firefox native messaging host (native/).
--
-- Installs the host script and the rendered native messaging manifest:
--   <prefix>/libexec/kwin-api-server/kwin-api-host.py
--   <prefix>/lib/firefox/native-messaging-hosts/org.plasma_tweak.kwin_api.json
--
-- Firefox scans system-wide native messaging manifests in
-- /usr/lib/mozilla/native-messaging-hosts and /usr/lib64/mozilla/
-- native-messaging-hosts, so when the effective prefix is the real system
-- prefix (/usr) the manifest is additionally installed into every existing
-- directory of that pair — `sudo xmake install` alone is then enough for
-- Firefox to find the host. For staged installs (`xmake install -o <dir>`)
-- the manifest stays under the prefix; copy it to
-- ~/.mozilla/native-messaging-hosts/ to use it without root (see
-- native/README.md).
-- ---------------------------------------------------------------------------
target("kwin-api-native")
    set_kind("headeronly")
    on_install(function(target)
        -- The effective install prefix: set_installdir() by default, overridden
        -- by `xmake install -o <dir>` / --installdir / $DESTDIR. xmake resolves
        -- a *relative* prefix against the project directory (regardless of the
        -- CWD the command was run from); mirror that like the daemon target.
        local prefix = target:installdir()
        local abs_prefix = path.absolute(prefix, os.projectdir())
        -- <prefix>/libexec/kwin-api-server/kwin-api-host.py (Firefox execs it)
        local libexec = path.join(abs_prefix, "libexec", "kwin-api-server")
        os.mkdir(libexec)
        local host = path.join(libexec, "kwin-api-host.py")
        os.cp(path.join(os.projectdir(), "native", "kwin-api-host.py"), host)
        os.runv("chmod", {"755", host}) -- os.cp does not preserve modes
        -- <prefix>/lib/firefox/native-messaging-hosts/<name>.json (rendered
        -- from the template so the `path` always follows the effective prefix)
        local template = io.readfile(path.join(os.projectdir(), "native",
                                               "host-manifest.json"))
        assert(template, "cannot read native/host-manifest.json")
        local manifest = template:gsub("@HOST_PATH@", host)
        local mdir = path.join(abs_prefix, "lib", "firefox", "native-messaging-hosts")
        os.mkdir(mdir)
        local manifest_file = path.join(mdir, "org.plasma_tweak.kwin_api.json")
        io.writefile(manifest_file, manifest)
        -- System-wide install: also drop the manifest where Firefox looks.
        if abs_prefix == "/usr" then
            for _, sysdir in ipairs({
                "/usr/lib/mozilla/native-messaging-hosts",
                "/usr/lib64/mozilla/native-messaging-hosts",
            }) do
                if os.isdir(sysdir) then
                    os.cp(manifest_file,
                          path.join(sysdir, "org.plasma_tweak.kwin_api.json"))
                end
            end
        end
    end)
    on_uninstall(function(target)
        local prefix = target:installdir()
        local abs_prefix = path.absolute(prefix, os.projectdir())
        os.tryrm(path.join(abs_prefix, "libexec", "kwin-api-server",
                           "kwin-api-host.py"))
        os.tryrm(path.join(abs_prefix, "lib", "firefox", "native-messaging-hosts",
                           "org.plasma_tweak.kwin_api.json"))
        if abs_prefix == "/usr" then
            for _, sysdir in ipairs({
                "/usr/lib/mozilla/native-messaging-hosts",
                "/usr/lib64/mozilla/native-messaging-hosts",
            }) do
                os.tryrm(path.join(sysdir, "org.plasma_tweak.kwin_api.json"))
            end
        end
    end)
