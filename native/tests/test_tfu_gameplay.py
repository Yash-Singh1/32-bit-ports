#!/usr/bin/env python3
"""Fresh TFU new-game/controller regression, behind other desktop applications.

Requires a built bundle and a previously accepted license. Uses a unique
preferences domain and save directory; simulated input goes only to XInput
inside this process. Captures the game framebuffer, never the desktop.
"""
import ctypes
import datetime
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import time
import uuid

NATIVE = Path(__file__).resolve().parents[1]
BUNDLE = NATIVE / "build/TFU-Compat.app"
OUTPUT = NATIVE / "build/test-tfu-gameplay"
OUTPUT.mkdir(exist_ok=True)
case = Path(tempfile.mkdtemp(prefix="new-game-", dir=OUTPUT))
domain = "com.aspyr.SWTFU.compat-gameplay-test-" + uuid.uuid4().hex
original = subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])
prefs = plistlib.loads(original)
assert prefs.get("AspyrEulaAccepted"), "Accept the supplied license before running this test."
cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
# Aspyr compares LowDetail to the STRING "0" to enable full-detail shaders.
# A plist Boolean False silently takes a different path and hides GLSL bugs.
prefs.update(RegistrationRequestDate=datetime.datetime(3999, 9, 4), PrefsDialogAlways=False,
             GameDisplayID=cg.CGMainDisplayID(), GameDisplayMode=0,
             GameDisplayWidth=1280, GameDisplayHeight=800, GameDisplayDepth=32,
             GameDisplayFrequency=0, IsKBAndMouse="FALSE", LowDetail="0")
# The Steam build opens Aspyr's Game Guide launcher first; skip it through
# this test's preference domain, as test_tfu_continue.py does.
guide = BUNDLE / "Contents/Resources/GameGuideInfo.plist"
if guide.exists():
    guide_key = plistlib.loads(guide.read_bytes()).get("PreferenceDoNotShowKey")
    if guide_key:
        prefs[guide_key] = True
loader = Path(os.environ.get("LP32_TEST_LOADER", str(BUNDLE / "Contents/MacOS/TFUCompat")))
process = None


def pad(lx=0, ly=0, rx=0, ry=0, lt=0, rt=0, buttons=0, pov=-1):
    temporary = case / "pad.tmp"
    temporary.write_text(f"{lx} {ly} {rx} {ry} {lt} {rt} {buttons} {pov}\n")
    temporary.replace(case / "pad")


def alive():
    assert process.poll() is None, f"Game exited ({process.returncode}); see {case / 'run.log'}"


def wait(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        alive()
        time.sleep(min(.25, max(0, end - time.monotonic())))


def pulse(button):
    pad(buttons=button)
    wait(.3)
    pad()
    wait(2)


def frame():
    try:
        raw = (case / "frame.ppm").read_bytes()
        magic, dimensions, maximum, pixels = raw.split(b"\n", 3)
        width, height = map(int, dimensions.split())
        assert magic == b"P6" and maximum == b"255" and len(pixels) == width * height * 3
        return raw, width, height, pixels
    except (FileNotFoundError, ValueError, AssertionError):
        return None


def health_bar(capture):
    if capture is None:
        return False
    _, width, height, pixels = capture
    # Kashyyyk's green health bar distinguishes a rendered playable level
    # from the title, loading screen, intro movie, and pause menu.
    green = 0
    for y in range(int(height * .07), int(height * .10)):
        for x in range(int(width * .08), int(width * .235)):
            i = (y * width + x) * 3
            r, g, b = pixels[i:i+3]
            green += g > 150 and g > r * 1.4 and g > b * 1.4
    return green > 200


def wait_health(expected, timeout=120):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        alive()
        capture = frame()
        if capture and health_bar(capture) == expected:
            return capture
        wait(.5)
    raise AssertionError(f"Expected gameplay HUD={expected}; see {case}")


def check_materials(capture):
    # The high-detail GLSL bug produced opaque black blocks across this known
    # Kashyyyk view (over 20% of pixels). The corrected view has under 0.1%;
    # allow 2% for animation, native shadows, and differences in frame timing.
    _, width, height, pixels = capture
    black = sum(pixels[i:i+3] == b"\0\0\0" for i in range(0, len(pixels), 3))
    assert black < width * height * .02, f"Black material corruption: {black}/{width*height}; see {case}"


try:
    subprocess.run(["defaults", "import", domain, "-"], input=plistlib.dumps(prefs), check=True)
    pad()
    env = dict(os.environ, LP32_BACKGROUND_TEST="1", LP32_MUTE_AUDIO="1", LP32_TFU_PREFERENCES_ID=domain,
               LP32_TFU_USER_DATA_ROOT=str(case / "user-data"),
               LP32_TFU_TEST_PAD_FILE=str(case / "pad"), LP32_TRACE_TFU_CONTROLLER="1",
               LP32_TRACE_MOVIES="1",
               LP32_AGL_CAPTURE_FRAME=str(case / "frame.ppm"), LP32_AGL_CAPTURE_EVERY="120")
    for key in ("LP32_HEADLESS", "LP32_BUILD_UI_ONLY", "LP32_GUEST_RUNTIME_DIR",
                "LP32_TRACE_GUEST_ADDRESS", "LP32_CARBON_TEST_KEYS_FILE"):
        env.pop(key, None)
    with (case / "run.log").open("w") as log:
        process = subprocess.Popen(["arch", "-x86_64", str(loader),
                                    str(BUNDLE / "Contents/SharedSupport/TFU.image")],
                                   cwd=NATIVE, env=env, stdout=log, stderr=subprocess.STDOUT)
        print(f"TFU test pid={process.pid}; artifacts: {case}", flush=True)
        deadline = time.monotonic() + 60
        while "movie stop name=Aspyr.mov" not in (case / "run.log").read_text():
            assert time.monotonic() < deadline, "Startup movie did not finish"
            wait(.5)
        wait(3)
        # A fresh profile may also show the title's initial button prompt.
        # Stop advancing as soon as the movie opens: another A skips it.
        for _ in range(4):
            if "movie start name=FMV-CineBlock_01-" in (case / "run.log").read_text():
                break
            pulse(0x1000)
            wait(3)
        deadline = time.monotonic() + 30
        while "movie start name=FMV-CineBlock_01-" not in (case / "run.log").read_text():
            assert time.monotonic() < deadline, "New Game did not start its opening movie"
            wait(.5)
        wait(10)
        assert not health_bar(frame()), "Opening story movie was skipped"
        capture = wait_health(True, 270)
        (case / "kashyyyk.ppm").write_bytes(capture[0])
        check_materials(capture)
        wait(20)  # Let the in-engine arrival sequence finish.
        if os.environ.get("LP32_TRACE_GL_RENDER"):
            import signal
            process.send_signal(signal.SIGQUIT)
            wait(3)
        pad(ly=1)
        wait(3)
        pad()
        wait(2)
        pad(rx=.7)
        wait(2)
        pad()
        wait(3)
        capture = wait_health(True)
        (case / "after-sticks.ppm").write_bytes(capture[0])
        check_materials(capture)
        pulse(0x10)  # Options / Start must pause the actual game.
        (case / "paused.ppm").write_bytes(wait_health(False, 15)[0])
        pulse(0x2000)  # Circle / B returns to gameplay.
        (case / "resumed.ppm").write_bytes(wait_health(True, 15)[0])
        (case / "pad").write_text("")  # Disconnect and reconnect a neutral pad.
        wait(2)
        pad()
        wait(2)
        pulse(0x10)
        wait_health(False, 15)
        pulse(0x2000)
        wait_health(True, 15)
        wait(5)
        text = (case / "run.log").read_text(errors="replace")
        assert "TFU Xbox gamepad state polling active" in text
        assert "FMV-CineBlock_01-" in text
        import re
        playback = re.search(r"movie stop name=FMV-CineBlock_01-.*? time=([\d.]+).*?frames=(\d+)", text)
        assert playback and float(playback[1]) > 180 and int(playback[2]) > 1000, "Opening story movie did not play through"
        assert "trapped import" not in text and "compat32: signal" not in text
        print(f"PASS packaged TFU: fresh Kashyyyk gameplay, controller start/pause/resume/reconnect; artifacts: {case}", flush=True)
finally:
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    subprocess.run(["defaults", "delete", domain], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert plistlib.loads(subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])) == plistlib.loads(original), "Normal preferences changed during test"
