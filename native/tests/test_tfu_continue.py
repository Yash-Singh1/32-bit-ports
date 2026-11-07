#!/usr/bin/env python3
"""Replay Continue from a copy of the user's checkpoint, with isolated input.

Does not verify a specific level: inspect the captured framebuffer to identify
the checkpoint. LP32_CONTINUE_SECONDS controls observation time (default 60).
"""
import datetime
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import tempfile
import time
import uuid

NATIVE = Path(__file__).resolve().parents[1]
BUNDLE = Path(os.environ.get("LP32_CONTINUE_BUNDLE", NATIVE / "build/TFU-Compat.app"))
OUTPUT = NATIVE / "build/test-tfu-continue"


def has_health_bar(path):
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    assert magic == b"P6" and maximum == b"255" and len(pixels) == width * height * 3
    green = 0
    for y in range(int(height * .07), int(height * .10)):
        for x in range(int(width * .08), int(width * .235)):
            offset = (y * width + x) * 3
            r, g, b = pixels[offset:offset + 3]
            green += g > 150 and g > r * 1.4 and g > b * 1.4
    return green > 200


def check_logo_playback(log_text):
    for name, minimum_time, minimum_frames in (
            ("FMV-GoldGuy-WMV9_HD_STEREO.mov", 16.9, 450), ("Aspyr.mov", 9.8, 250)):
        match = re.search(r"movie stop name=" + re.escape(name) +
                          r" time=([\d.]+).*? frames=(\d+)", log_text)
        assert match and float(match[1]) >= minimum_time and int(match[2]) >= minimum_frames, \
            f"Incomplete movie playback: {name}"


def check_standing_blade_texture(log_text, pid):
    marker = re.search(r"GLSL uniform program=\d+ name=falloffDistance", log_text)
    assert marker, "No standing-blade shader captured"
    block = log_text[marker.start():].split("compat32: glDraw", 1)[0]
    texture = re.search(r"texture-state swap=\d+ unit=0 tex2d=(\d+) size2d=(\d+)x(\d+)", block)
    assert texture, "No standing-blade base texture captured"
    directory = Path(os.environ.get("LP32_DUMP_GL_DATA",
        Path.home() / f"Library/Logs/TFUCompat/gl-programs/data-{pid}-1"))
    pixels = (directory / f"texture-rgba8-{texture[1]}.bin").read_bytes()
    assert len(pixels) == int(texture[2]) * int(texture[3]) * 4
    assert any(pixels[i] for i in range(len(pixels)) if i % 4 != 3), "Standing-blade texture has no RGB content"


def main():
    OUTPUT.mkdir(exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="checkpoint-", dir=OUTPUT))
    source = Path(os.environ.get("LP32_CONTINUE_SAVE",
        Path.home() / "Documents/Aspyr/Star Wars The Force Unleashed/SWTFU.BIN"))
    original_save = source.read_bytes()
    original_prefs = subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])
    prefs = plistlib.loads(original_prefs)
    assert prefs.get("AspyrEulaAccepted"), "Requires an already accepted game license"
    prefs.update(RegistrationRequestDate=datetime.datetime(3999, 9, 4),
                 PrefsDialogAlways=bool(os.environ.get("LP32_CONTINUE_SETTINGS")), GameDisplayMode=0,
                 GameDisplayWidth=1280, GameDisplayHeight=800,
                 GameDisplayDepth=32, GameDisplayFrequency=0,
                 IsKBAndMouse="FALSE", LowDetail="0")
    guide = BUNDLE / "Contents/Resources/GameGuideInfo.plist"
    if guide.exists() and not os.environ.get("LP32_CONTINUE_GUIDE"):
        # Equivalent to selecting “Don't show again”, in this test's private
        # preference domain only. LP32_CONTINUE_GUIDE exercises the launcher UI.
        guide_key = plistlib.loads(guide.read_bytes()).get("PreferenceDoNotShowKey")
        if guide_key:
            prefs[guide_key] = True
    if os.environ.get("LP32_CONTINUE_FULLSCREEN"):
        prefs.update(GameDisplayMode=1, GameDisplayWidth=1920, GameDisplayHeight=1200)
    domain = "com.aspyr.SWTFU.compat-continue-" + uuid.uuid4().hex
    target = case / "user-data/Documents/Aspyr/Star Wars The Force Unleashed/SWTFU.BIN"
    target.parent.mkdir(parents=True)
    target.write_bytes(original_save)
    # Steam 1.3 stores progress in Application Support, unlike retail 1.2.
    steam_target = case / "user-data/Application Support/Star Wars The Force Unleashed/SWTFU.BIN"
    steam_target.parent.mkdir(parents=True)
    steam_target.write_bytes(original_save)
    (case / "save.sha256").write_text(hashlib.sha256(original_save).hexdigest() + "\n")
    loader = os.environ.get("LP32_TEST_LOADER", str(NATIVE / "build/game_loader"))
    # Per-import profiling adds clock reads to every guest/host transition.
    # Measure frame times by default; LP32_FRAME_STATS=1 still opts into it.
    env = dict(os.environ, LP32_BACKGROUND_TEST="1", LP32_MUTE_AUDIO="1",
               LP32_TFU_PREFERENCES_ID=domain, LP32_TFU_USER_DATA_ROOT=str(case / "user-data"),
               LP32_TFU_TEST_PAD_FILE=str(case / "pad"), LP32_TRACE_MOVIES="1",
               LP32_DIAGNOSTIC_LOG=str(case / "diagnostic.log"), LP32_PORTAL_FRAME_STATS="1",
               LP32_SLOW_IMPORT_MS="100",
               LP32_TRACE_BUFFER_CACHE="1", LP32_AGL_CAPTURE_FRAME=str(case / "frame.ppm"),
               LP32_AGL_CAPTURE_EVERY="120")
    if os.environ.get("LP32_CONTINUE_FOREGROUND"):
        env.pop("LP32_BACKGROUND_TEST", None)
    if os.environ.get("LP32_CONTINUE_SETTINGS"):
        env["LP32_CARBON_TEST_COMMANDS"] = "ok  "
    for key in ("LP32_HEADLESS", "LP32_BUILD_UI_ONLY", "LP32_GUEST_RUNTIME_DIR",
                "LP32_TRACE_GUEST_ADDRESS", "LP32_CARBON_TEST_KEYS_FILE"):
        env.pop(key, None)
    process = None
    sampler = None

    def pad(buttons=0):
        temp = case / "pad.tmp"
        temp.write_text(f"0 0 0 0 0 0 {buttons} -1\n")
        temp.replace(case / "pad")

    def wait(seconds):
        nonlocal sampler
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            assert process.poll() is None, f"Exited {process.returncode}; see {case}"
            if os.environ.get("LP32_CONTINUE_SAMPLE") and sampler is None and \
                    "movie dispose name=FMV-LoadScreen.mov" in (case / "run.log").read_text():
                sampler = subprocess.Popen(["/usr/bin/sample", str(process.pid), "5", "1",
                    "-file", str(case / "first-frame-sample.txt")],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(min(.25, max(0, deadline - time.monotonic())))

    def pulse(button):
        pad(button)
        wait(.2)
        pad()
        wait(2)

    try:
        subprocess.run(["defaults", "import", domain, "-"], input=plistlib.dumps(prefs), check=True)
        pad()
        with (case / "run.log").open("w") as log:
            process = subprocess.Popen(["arch", "-x86_64", loader,
                str(BUNDLE / "Contents/SharedSupport/TFU.image")],
                cwd=NATIVE, env=env, stdout=log, stderr=subprocess.STDOUT)
            print(f"TFU Continue pid={process.pid}; artifacts: {case}", flush=True)
            deadline = time.monotonic() + float(os.environ.get("LP32_CONTINUE_STARTUP_SECONDS", "75"))
            ready = "movie intro omitted name=Aspyr.mov" if os.environ.get("LP32_CONTINUE_SKIP_INTROS") else "movie stop name=Aspyr.mov"
            while ready not in (case / "run.log").read_text():
                assert time.monotonic() < deadline, "Startup movie did not finish"
                wait(.5)
            wait(3)
            if os.environ.get("LP32_CONTINUE_LAUNCH_ONLY"):
                check_logo_playback((case / "run.log").read_text())
                assert (case / "frame.ppm").exists(), "No launch framebuffer captured"
                print(f"PASS launcher and complete intro playback; artifacts: {case}", flush=True)
                return
            pulse(0x1000)  # Dismiss title prompt.
            wait(3)
            if (case / "frame.ppm").exists():
                shutil.copyfile(case / "frame.ppm", case / "menu.ppm")
            pulse(0x1000)  # Continue is selected with an existing checkpoint.
            if os.environ.get("LP32_CONTINUE_SABER_TRACE"):
                wait(15)
                pulse(0x4000)  # Draw the blade, then let the attack finish.
                wait(4)
                if os.environ.get("LP32_CONTINUE_CAMERA_TURN_SECONDS"):
                    temp = case / "pad.tmp"
                    temp.write_text("0 0 1 0 0 0 0 -1\n")
                    temp.replace(case / "pad")
                    wait(float(os.environ["LP32_CONTINUE_CAMERA_TURN_SECONDS"]))
                    pad()
                    wait(3)
                assert "enabled SIGQUIT-triggered GL tracing for context presentation" in (case / "run.log").read_text()
                process.send_signal(signal.SIGQUIT)
                wait(5)
                shutil.copyfile(case / "frame.ppm", case / "standing-saber.ppm")
            if os.environ.get("LP32_CONTINUE_GRIP"):
                wait(15)
                pulse(0x4000)  # Enter training with the saber already drawn.
                pulse(0x10)
                pulse(2)
                pulse(2)
                pulse(0x1000)
                wait(3)  # Let the training-menu transition finish before navigating.
                for _ in range(4):
                    pulse(2)
                pulse(0x1000)
                pulse(0x1000)  # Confirm replay, affecting only the save copy.
                wait(15)
                shutil.copyfile(case / "frame.ppm", case / "grip-intro.ppm")
                assert has_health_bar(case / "grip-intro.ppm"), "Grip replay did not load; inspect the menu selection"
                pulse(0x1000)
                wait(5)
                shutil.copyfile(case / "frame.ppm", case / "grip-entry.ppm")
                if os.environ.get("LP32_CONTINUE_GRIP_ENTRY_TRACE"):
                    assert "enabled SIGQUIT-triggered GL tracing for context presentation" in (case / "run.log").read_text()
                    process.send_signal(signal.SIGQUIT)
                    wait(5)
                temp = case / "pad.tmp"
                temp.write_text("0 0 0 0 0 1 0 -1\n")
                temp.replace(case / "pad")
                wait(6)
                pad()
                wait(5)
                shutil.copyfile(case / "frame.ppm", case / "grip-before-swing.ppm")
                pulse(0x4000)
                wait(5)
                shutil.copyfile(case / "frame.ppm", case / "grip.ppm")
            wait(float(os.environ.get("LP32_CONTINUE_SECONDS", "60")))
            assert (case / "frame.ppm").exists(), "No framebuffer captured"
            shutil.copyfile(case / "frame.ppm", case / "checkpoint.ppm")
            assert has_health_bar(case / "checkpoint.ppm"), "Continue did not reach a rendered gameplay HUD"
            log_text = (case / "run.log").read_text(errors="replace")
            if not os.environ.get("LP32_CONTINUE_SKIP_INTROS"):
                check_logo_playback(log_text)
            if os.environ.get("LP32_CONTINUE_EXPECT_BLADE"):
                assert os.environ.get("LP32_CONTINUE_SABER_TRACE") or os.environ.get("LP32_CONTINUE_GRIP_ENTRY_TRACE"), "Blade check requires a saber or Grip entry trace"
                check_standing_blade_texture(log_text, process.pid)
            assert "trapped import" not in log_text and "compat32: signal" not in log_text
            # Steam 1.3 can load directly into a mission cinematic without
            # playing retail's FMV-LoadScreen.mov. The captured gameplay HUD
            # above is the distribution-independent completion condition.
            print(f"PASS Continue observation (inspect checkpoint.ppm for level); artifacts: {case}", flush=True)
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if sampler:
            try:
                sampler.wait(timeout=10)
            except subprocess.TimeoutExpired:
                sampler.kill()
                sampler.wait()
        subprocess.run(["defaults", "delete", domain], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert source.read_bytes() == original_save, "Original checkpoint changed during isolated test"


if __name__ == "__main__":
    main()
