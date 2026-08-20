// test/daemon — intentionally EMPTY KWin script.
//
// The mock service (kwin-api-server-test.service, see the .in template) points
// KWIN_SCRIPT_PATH at this file instead of the real kwinscript bundle, so the
// daemon's loadScript/run has no effect inside KWin: no shortcuts, no D-Bus
// traffic, nothing to clean up. The Python test (test_daemon.py) plays the
// role of the script itself over the daemon's session-bus objects.
//
// `run` is defined because KWin scripts conventionally have one; it does
// nothing.
function run() {}
