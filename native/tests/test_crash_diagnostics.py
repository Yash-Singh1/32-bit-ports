"""Fatal diagnostics must survive a libc fault and always terminate promptly."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

loader = str(Path(sys.argv[1]).resolve())
base = {k: v for k, v in os.environ.items() if not k.startswith("LP32_")}
base["LP32_NO_DIAGNOSTIC_LOG"] = "1"
for mode, expected_signal in (("1", signal.SIGSEGV), ("stdio", signal.SIGSEGV),
                              ("stack", signal.SIGILL), ("unmapped", signal.SIGSEGV)):
    env = {**base, "LP32_CRASH_DIAGNOSTIC_SELFTEST": mode,
           "LP32_CRASH_DUMP": "0x1000:8"}
    result = subprocess.run(["arch", "-x86_64", loader], env=env,
                            capture_output=True, timeout=6, text=True)
    assert result.returncode == 128 + expected_signal, (mode, result.returncode, result.stderr)
    assert f"compat32: signal {expected_signal} " in result.stderr, result.stderr
    assert "crash cs=" in result.stderr and "crash region" in result.stderr, result.stderr
    if mode == "stack":
        assert "rsp=0x0000000000000000" in result.stderr, result.stderr
    if mode == "unmapped":
        assert "mem[0x00001000] unreadable" in result.stderr, result.stderr
    print(f"Crash diagnostics PASS ({mode})")

# An unread pipe can block write(2) forever. Even an inherited SIGALRM mask
# must not defeat the fatal handler's two-second deadline.
read_fd, write_fd = os.pipe()
try:
    os.set_blocking(write_fd, False)
    try:
        while True:
            os.write(write_fd, b"x" * 4096)
    except BlockingIOError:
        pass
    os.set_blocking(write_fd, True)
    previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGALRM})
    try:
        process = subprocess.Popen(["arch", "-x86_64", loader],
            env={**base, "LP32_CRASH_DIAGNOSTIC_SELFTEST": "1"},
            stdout=subprocess.DEVNULL, stderr=write_fd)
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
    try:
        assert process.wait(timeout=6) == -signal.SIGALRM
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
finally:
    os.close(read_fd)
    os.close(write_fd)
print("Crash diagnostics PASS (blocked output with SIGALRM initially masked)")

# A failure before profile selection still needs a log on Finder launches.
with tempfile.TemporaryDirectory(prefix="lp32-startup-") as temporary:
    home = Path(temporary)
    (home / "Library/Logs").mkdir(parents=True)
    image = home / "invalid.image"
    image.write_bytes(b"invalid Mach-O")
    env = {**base, "HOME": str(home)}
    env.pop("LP32_NO_DIAGNOSTIC_LOG")
    result = subprocess.run(["arch", "-x86_64", loader, str(image)],
        env=env, capture_output=True, timeout=6, text=True)
    assert result.returncode == 1, result.stderr
    log = (home / "Library/Logs/LP32Compat/startup.log").read_text()
    assert "diagnostic session pid=" in log and str(image) in log, log
print("Crash diagnostics PASS (startup log before image loading)")
