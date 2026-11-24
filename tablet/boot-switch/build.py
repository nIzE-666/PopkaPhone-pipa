#!/usr/bin/env python3
"""Build the pipa boot switch APK with JDK and Android SDK tools."""

import argparse
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import zipfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUTPUT = ROOT / "tablet" / "build" / "boot-switch"
KEYS = ROOT / "private" / "pipa-boot-switch"
APK = "PopkaPhone-Boot-Switch.apk"


def run(*args):
    subprocess.run(args, check=True)


def java_tool(name):
    jdk = os.environ.get("JAVA_HOME")
    suffix = ".exe" if sys.platform == "win32" else ""
    return Path(jdk) / "bin" / (name + suffix) if jdk else name


def signing_key():
    KEYS.mkdir(mode=0o700, parents=True, exist_ok=True)
    key = KEYS / "signing.p12"
    password = KEYS / "password"
    if key.exists() and not password.is_file():
        raise ValueError(f"Missing password for the existing key {key}")
    if not password.exists():
        fd = os.open(password, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as file:
            file.write(secrets.token_hex(24) + "\n")
    if not key.exists():
        run(
            java_tool("keytool"), "-genkeypair", "-keystore", key, "-storetype", "PKCS12",
            "-storepass:file", password, "-alias", "pipa-boot-switch", "-keyalg", "RSA",
            "-keysize", "3072", "-validity", "36500", "-dname", "CN=PopkaPhone Boot Switch",
        )
        key.chmod(0o600)
    return key, password


def build(sdk):
    platform = sdk / "platforms" / "android-36" / "android.jar"
    tools = sdk / "build-tools" / "36.0.0"
    suffix = ".exe" if sys.platform == "win32" else ""
    aapt = tools / ("aapt2" + suffix)
    align = tools / ("zipalign" + suffix)
    dexer = tools / "lib" / "d8.jar"
    signer = tools / "lib" / "apksigner.jar"
    for path in (platform, aapt, align, dexer, signer):
        if not path.is_file():
            raise ValueError(f"Missing {path}. Install platforms;android-36 and build-tools;36.0.0.")

    version_code = subprocess.check_output(["git", "rev-list", "--count", "HEAD"], cwd=ROOT, text=True).strip()
    revision = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="build-", dir=OUTPUT) as temporary:
        scratch = Path(temporary)
        assets = scratch / "assets"
        generated = scratch / "generated"
        classes = scratch / "classes"
        dex = scratch / "dex"
        for folder in (assets, generated, classes, dex):
            folder.mkdir()
        helper = ROOT / "device" / "naomi-pipa-slot-switch"
        (assets / helper.name).write_bytes(helper.read_text(encoding="utf-8").encode("utf-8"))

        resources = scratch / "resources.zip"
        base, aligned, signed = (scratch / name for name in ("base.apk", "aligned.apk", APK))
        run(aapt, "compile", "--dir", HERE / "res", "-o", resources)
        run(
            aapt, "link", "-o", base, "-I", platform, "--manifest", HERE / "AndroidManifest.xml",
            "--min-sdk-version", "34", "--target-sdk-version", "36", "--version-code", version_code,
            "--version-name", f"0.1-{revision}", "--java", generated, "-A", assets, resources,
        )
        sources = sorted((HERE / "src").rglob("*.java")) + sorted(generated.rglob("*.java"))
        run(java_tool("javac"), "--release", "11", "-encoding", "UTF-8", "-cp", platform, "-d", classes, *sources)
        run(
            java_tool("java"), "-cp", dexer, "com.android.tools.r8.D8", "--release", "--min-api", "34",
            "--lib", platform, "--output", dex, *sorted(classes.rglob("*.class")),
        )
        with zipfile.ZipFile(base, "a", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(dex / "classes.dex", "classes.dex")
        run(align, "-f", "4", base, aligned)
        key, password = signing_key()
        run(java_tool("java"), "-jar", signer, "sign", "--ks", key, "--ks-pass", f"file:{password}", "--out", signed, aligned)
        run(java_tool("java"), "-jar", signer, "verify", signed)
        signed.replace(OUTPUT / APK)
    print(OUTPUT / APK)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, help="Android SDK path (defaults to ANDROID_HOME or ANDROID_SDK_ROOT)")
    args = parser.parse_args()
    root = args.sdk or os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if not root:
        parser.error("set ANDROID_HOME or pass --sdk")
    try:
        build(Path(root).resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"APK build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
