"""Exercise the actual GL import bridge with a synthetic image and no game assets."""
import os
from pathlib import Path
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="lp32-asphalt-scene-") as directory:
    image = Path(directory) / "Asphalt6.image"
    subprocess.run([
        "xcrun", "clang", "-target", "i386-apple-macos10.6", "-nostdlib",
        "-Wl,-e,_start,-no_pie", str(native / "tests/fixtures/asphalt6_float.S"),
        "-o", str(image),
    ], check=True)
    for flags in ({"LP32_GL_BACKEND": "apple"}, {"LP32_GL_BACKEND": "metal", "LP32_GLMETAL_PATH": str(native / "glmetal/build/libGLMetal.dylib")}):
        environment = dict(os.environ, LP32_GAME="asphalt6", LP32_ASPHALT6_SCENE_SELFTEST="1",
                           LP32_ASPHALT6_SCENE_SCALE="3", LP32_ASPHALT6_MSAA="8",
                           LP32_NO_DIAGNOSTIC_LOG="1")
        environment.update(flags)
        result = subprocess.run(["arch", "-x86_64", str(native / "build/game_loader"), str(image)],
                                env=environment, capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        print(result.stderr, end="")
        assert result.returncode == 0
        if flags["LP32_GL_BACKEND"] == "metal":
            assert "OpenGL through GLMetal" in result.stderr
            assert "falling back" not in result.stderr
        assert "Asphalt scene import bridge fast: PASS" in result.stderr
        assert "Asphalt scene import bridge chained: PASS" in result.stderr
