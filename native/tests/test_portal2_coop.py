#!/usr/bin/env python3
"""Exercise issue #10 overrides and tracing with real i386 modules, no Steam/game."""
import os
from pathlib import Path
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def build(*command):
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


with tempfile.TemporaryDirectory(prefix="lp32-coop-") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _getenv, _dlopen, _dlsym, _strcmp, _memcmp, _memcpy, _memset,
      ___stack_chk_guard, ___stack_chk_fail, dyld_stub_binder ]
...
""")
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    flags = ["xcrun", "clang", "-target", "i386-apple-macos10.6", "-nostdlib", "-O2",
             "-fno-builtin", "-L" + str(root), "-lSystem", str(root / "helper.S")]
    if os.environ.get("LP32_I386_LINKER"):
        flags.append("-fuse-ld=" + os.environ["LP32_I386_LINKER"])
    build(*flags, "-Wl,-e,_start", str(root / "start.S"), "-o", str(root / "portal2_osx"))
    for module, exported in (("engine", "fixture_value"), ("server", "fixture_classes")):
        build(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/" + module + ".dylib",
              "-Wl,-exported_symbol,_" + exported, str(NATIVE / f"tests/fixtures/coop_{module}.c"),
              "-o", str(root / f"bin/{module}.dylib"))
    build(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/launcher.dylib",
          str(NATIVE / "tests/fixtures/coop_launcher.c"), str(root / "bin/engine.dylib"),
          str(root / "bin/server.dylib"), "-o", str(root / "bin/launcher.dylib"))
    build("xcrun", "clang", "-arch", "x86_64", "-O2", "-dynamiclib",
          str(NATIVE / "tests/fixtures/coop_steam_host.c"), "-o", str(root / "steamclient.dylib"))

    def check(label, value="7", classes="7", contains=(), absent=(), **options):
        environment = {k: v for k, v in os.environ.items() if not k.startswith(("LP32_", "COOP_"))}
        environment.update(LP32_GAME="portal2", LP32_DYLD_SELFTEST="1", LP32_DYLD_FIXTURE_SELFTEST="1",
                           LP32_LOG_DIR=str(root / "logs"), LP32_STEAM_FIXTURE=str(root / "steamclient.dylib"),
                           COOP_EXPECT_VALUE=value, COOP_EXPECT_CLASSES=classes)
        environment.update(options)
        result = subprocess.run(["arch", "-x86_64", str(NATIVE / "build/game_loader"), str(root / "portal2_osx")],
                                env=environment, capture_output=True, text=True, timeout=60)
        output = result.stdout + result.stderr
        assert result.returncode == 0 and "guest-dyld self-test: PASS" in output, label + "\n" + output
        for text in contains:
            assert text in output, label + " missing " + text + "\n" + output
        for text in (*absent, "fixture-ticket-sensitive"):
            assert text not in output, label + " unexpected " + text + "\n" + output
        print("Portal 2 co-op: PASS (" + label + ")")
        return output

    check("off by default", absent=("compat32: convar", "dropped server class", "steam method", "steam callback"))
    check("constructor ordering, private symbols, middle removal, reopen", value="1", classes="5",
          LP32_SET_CONVARS="sv_sendtables=1", LP32_DROP_SERVER_CLASSES="CPointSurvey",
          contains=("convar sv_sendtables = 1", "dropped server class CPointSurvey"))
    check("head removal", classes="4", COOP_CLASS_MODE="head", LP32_DROP_SERVER_CLASSES="CPointSurvey")
    check("multiple overrides and removals", value="0", classes="0",
          LP32_SET_CONVARS=" sv_sendtables = 1 , sv_sendtables=0 ",
          LP32_DROP_SERVER_CLASSES=" CPointSurvey, CFirst, CLast, CPointSurvey ",
          contains=("is not in the active list",))
    check("malformed and unknown settings", LP32_SET_CONVARS="missing,=1,bad-name=1,unknown=1",
          LP32_DROP_SERVER_CLASSES="bad-name,MissingClass", contains=("requires name=value", "invalid convar", "missing or incompatible"))
    check("overlong input rejected without partial changes", LP32_SET_CONVARS="sv_sendtables=1," + "x" * 1024,
          LP32_DROP_SERVER_CLASSES="CPointSurvey," + "x" * 1024, contains=("exceeds 1023 bytes",))
    check("invalid convar pointer", COOP_BAD_CONVAR="1", LP32_SET_CONVARS="sv_sendtables=1",
          contains=("missing or incompatible",))
    for mode in ("cycle", "bad-next", "bad-name"):
        check("invalid class list: " + mode, COOP_CLASS_MODE=mode, LP32_DROP_SERVER_CLASSES="CPointSurvey",
              contains=("left unchanged",))
    output = check("filtered methods, packed returns, bounded callbacks, all denies preserved",
          LP32_TRACE_STEAM_METHODS="GameServer", LP32_TRACE_STEAM_CALLBACKS="1",
          LP32_IGNORE_STEAM_DENY="11,12,13,14", LP32_IGNORE_STEAM_DENY_UNAPPROVED="1",
          contains=("SteamGameServer014::SendUserConnectAndAuthenticate_DEPRECATED(ipip) begin i=01000000",
                    "steamid=76561197979354471", "SteamGameServer014::GetPublicIP", "ValidateAuthTicketResponse",
                    "GSClientKick", 'text="X"'), absent=("steam method SteamUtils010",))
    for reason in range(15):
        assert f"reason={reason} " in output, "Steam deny was not delivered: " + str(reason)
    check("wildcard methods and disabled callback trace", LP32_TRACE_STEAM_METHODS="*", LP32_TRACE_STEAM_CALLBACKS="0",
          contains=("steam method SteamUtils010::GetAppID",), absent=("steam callback",))
    check("unmatched method filter", LP32_TRACE_STEAM_METHODS="NoSuchInterface", absent=("steam method", "steam callback"))
    for module, exported in (("engine", "fixture_value"), ("server", "fixture_classes")):
        build(*flags, "-dynamiclib", "-Wl,-seg1addr,0x90000000",
              "-Wl,-install_name,@loader_path/" + module + ".dylib",
              "-Wl,-exported_symbol,_" + exported, str(NATIVE / f"tests/fixtures/coop_{module}.c"),
              "-o", str(root / f"bin/{module}.dylib"))
    check("private symbols with downward relocation", value="1", classes="5",
          LP32_SET_CONVARS="sv_sendtables=1", LP32_DROP_SERVER_CLASSES="CPointSurvey")
