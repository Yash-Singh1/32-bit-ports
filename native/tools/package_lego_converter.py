#!/usr/bin/env python3
"""Build LEGOConverter with a native runtime, Pirates unpacker, and compiler.

No user game files enter the converter. Only developer builds need Python,
Xcode/CLT, and first-build network access for pinned Unicorn wheels.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
UNICORN_VERSION = "2.1.4"
TOOLCHAIN_LIBRARIES = ["libtapi.dylib", "libcodedirectory.dylib", "libLTO.dylib", "libswiftDemangle.dylib"]


def run(*command):
    subprocess.run([str(part) for part in command], check=True)


def output(*command):
    return subprocess.check_output([str(part) for part in command], text=True).strip()


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, destination, symlinks=True)
    else:
        shutil.copy2(source, destination)


def sdk_path(requested):
    if requested:
        path = Path(requested).resolve()
        if not path.is_dir():
            raise SystemExit(f"SDK does not exist: {path}")
        return path
    for name in ["MacOSX14.4.sdk", "MacOSX13.3.sdk", "MacOSX15.4.sdk"]:
        path = Path("/Library/Developer/CommandLineTools/SDKs") / name
        if path.is_dir():
            return path.resolve()
    raise SystemExit("Provide --sdk /path/to/MacOSX14.4.sdk to package a compatible SDK.")


def prepare_unicorn(build, clang, sdk):
    cache = build / "converter-deps"
    cache.mkdir(exist_ok=True)
    metadata = json.load(urllib.request.urlopen(f"https://pypi.org/pypi/unicorn/{UNICORN_VERSION}/json", timeout=30))
    result = {}
    for arch, tag in [("arm64", "macosx_11_0_arm64"), ("x86_64", "macosx_10_9_x86_64")]:
        release = next(item for item in metadata["urls"] if tag in item["filename"])
        wheel = cache / release["filename"]
        if not wheel.is_file() or hashlib.sha256(wheel.read_bytes()).hexdigest() != release["digests"]["sha256"]:
            print(f"Downloading Unicorn {UNICORN_VERSION} for {arch}", flush=True)
            with urllib.request.urlopen(release["url"], timeout=90) as response, wheel.open("wb") as stream:
                shutil.copyfileobj(response, stream)
            if hashlib.sha256(wheel.read_bytes()).hexdigest() != release["digests"]["sha256"]:
                raise SystemExit(f"Checksum mismatch: {wheel.name}")
        extracted = cache / ("unicorn-" + arch)
        if not extracted.exists():
            with zipfile.ZipFile(wheel) as archive:
                archive.extractall(extracted)
        library = extracted / "unicorn/lib/libunicorn.2.dylib"
        helper = cache / ("UnpackPirates-" + arch)
        run(clang, "-arch", arch, "-isysroot", sdk, "-mmacosx-version-min=11.0", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I" + str(extracted / "unicorn/include"), ROOT / "converter/UnpackPirates.c", library,
            "-Wl,-rpath,@executable_path/../../Frameworks", "-o", helper)
        launcher = cache / ("RunTool-" + arch)
        run(clang, "-arch", arch, "-isysroot", sdk, "-mmacosx-version-min=11.0", "-O2", "-Wall", "-Wextra", "-Werror",
            ROOT / "converter/RunTool.c", "-o", launcher)
        result[arch] = (helper, launcher, library, extracted)
    return result


def package_toolchain(source, sdk, resources):
    print("Bundling the compiler, linker, SDK, and runtime source", flush=True)
    toolchain = resources / "Toolchain"
    for name in ["clang", "ld"]:
        copy(source / "bin" / name, toolchain / "bin" / name)
    for name in TOOLCHAIN_LIBRARIES:
        copy(source / "lib" / name, toolchain / "lib" / name)
    resource = Path(output(source / "bin/clang", "-print-resource-dir"))
    version = resource.name
    copy(resource / "include", toolchain / "lib/clang" / version / "include")
    copy(resource / "lib/darwin/libclang_rt.osx.a", toolchain / "lib/clang" / version / "lib/darwin/libclang_rt.osx.a")
    copy(sdk, toolchain / "SDK")
    copy(ROOT / "src", toolchain / "Sources")
    lines = (ROOT / "Makefile").read_text().splitlines()
    prerequisites = " ".join(line for line in lines if line.startswith("$(BUILD_DIR)/game_loader:"))
    sources = sorted({item.removeprefix("src/") for item in re.findall(r"src/[\w.]+", prerequisites)
                      if Path(item).suffix in (".c", ".m", ".S")})
    frameworks = ["CoreText", "Security", "ForceFeedback", "OpenAL", "CFNetwork", "SystemConfiguration",
                  "AVFoundation", "CoreMedia", "CoreVideo", "Cocoa", "Carbon", "OpenGL", "AudioToolbox",
                  "AudioUnit", "CoreAudio", "GameController", "IOKit"]
    commands = output("otool", "-l", source / "bin/clang")
    versions = [int(value.split(".")[0]) for value in re.findall(r"minos (\d+(?:\.\d+)*)", commands)]
    minimum = max(versions or [14])
    if minimum > 14:
        raise SystemExit("Choose a --toolchain whose compiler supports macOS 14 or earlier.")
    manifest = {"sources": sources, "frameworks": frameworks, "minimumCompilerOS": minimum,
                "resourceVersion": version, "compiler": output(source / "bin/clang", "--version"),
                "sdk": sdk.name}
    (toolchain / "Build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (toolchain / "NOTICE.txt").write_text(
        "The compiler, linker, dependent libraries, resource headers, and SDK were copied from the developer's locally installed Apple Command Line Tools.\n"
        + manifest["compiler"] + "\nSDK: " + sdk.name + "\n"
        "Normal conversions use the bundled game runtime and do not launch the compiler.\n")
    for file in [toolchain / "bin/clang", toolchain / "bin/ld"] + [toolchain / "lib" / name for name in TOOLCHAIN_LIBRARIES]:
        run("codesign", "--force", "--sign", "-", file)


def package(build, sdk, toolchain_source):
    build.mkdir(parents=True, exist_ok=True)
    clang = Path(output("xcrun", "--find", "clang"))
    unicorn = prepare_unicorn(build, clang, sdk)
    print("Assembling LEGO-Converter.app", flush=True)
    app = build / "LEGO-Converter.app"
    with tempfile.TemporaryDirectory(prefix=".lego-converter-package-", dir=build) as temporary:
        staged = Path(temporary) / app.name
        contents = staged / "Contents"
        resources = contents / "Resources"
        (contents / "MacOS").mkdir(parents=True)
        resources.mkdir()
        copy(ROOT / "converter/Info.plist", contents / "Info.plist")
        run("lipo", "-create", build / "lego-converter-arm64", build / "lego-converter-x86_64", "-output", contents / "MacOS/LEGOConverter")
        copy(build / "game_loader", resources / "Runtime/game_loader")
        for name in ["Info.plist", "Info-CloneWars.plist", "Info-Saga.plist", "Info-Marvel.plist", "Info-Batman3.plist", "Info-Movie.plist", "Info-Hobbit.plist"]:
            copy(ROOT / name, resources / "Templates" / name)
        (resources / "Tools").mkdir()
        (contents / "Frameworks").mkdir()
        for name, index in [("UnpackPirates", 0), ("RunTool", 1)]:
            run("lipo", "-create", unicorn["arm64"][index], unicorn["x86_64"][index], "-output", resources / "Tools" / name)
        run("lipo", "-create", unicorn["arm64"][2], unicorn["x86_64"][2], "-output", contents / "Frameworks/libunicorn.2.dylib")
        for arch, paths in unicorn.items():
            for license in paths[3].rglob("*"):
                if license.is_file() and ("LICENSE" in license.name or "COPYING" in license.name):
                    copy(license, resources / "Licenses/Unicorn" / arch / license.name)
        package_toolchain(toolchain_source, sdk, resources)
        for file in [contents / "Frameworks/libunicorn.2.dylib", resources / "Tools/RunTool",
                     resources / "Tools/UnpackPirates", resources / "Runtime/game_loader", contents / "MacOS/LEGOConverter"]:
            run("codesign", "--force", "--sign", "-", file)
        run("codesign", "--force", "--sign", "-", staged)
        run("codesign", "--verify", "--deep", "--strict", staged)
        previous = Path(temporary) / "previous.app"
        if app.exists():
            app.rename(previous)
        try:
            staged.rename(app)
        except BaseException:
            if previous.exists(): previous.rename(app)
            raise
    print(app, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--toolchain", type=Path, default=Path("/Library/Developer/CommandLineTools/usr"))
    args = parser.parse_args()
    package(args.build.resolve(), sdk_path(args.sdk), args.toolchain.resolve())
