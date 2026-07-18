Import("env")
import os
import shutil
import subprocess
import sys

# Nach jedem Build wird hier ein einziges, zusammengeführtes Flash-Image in
# docs/ abgelegt — das ist die Datei, die der Browser-basierte Web-Installer
# (docs/index.html + ESP Web Tools) über manifest-{ili9341,st7789}.json an
# das Gerät schreibt. Ohne diesen Merge bräuchte der Web-Installer mehrere
# Dateien mit unterschiedlichen Offsets; ein einziges Image ist robuster und
# einfacher. Pro PlatformIO-Env (Display-Controller-Variante) entsteht eine
# eigene *-merged.bin, damit beide Varianten nebeneinander in docs/ liegen.
# Benannt nach dem Display-Controller, nicht nach der USB-Portanzahl — die
# ist kein zuverlässiges äußeres Merkmal für den verbauten Controller.
def post_firmware(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    pioenv      = env.subst("$PIOENV")

    variant = "st7789" if "st7789" in pioenv else "ili9341"

    docs_dir = os.path.join(project_dir, "docs")
    os.makedirs(docs_dir, exist_ok=True)

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, "firmware.bin")

    merged = os.path.join(docs_dir, f"CYD-WiFi-Meter-{variant}-merged.bin")

    boot_app0 = os.path.join(
        env.subst("$PROJECT_PACKAGES_DIR"),
        "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin"
    )

    if not all(os.path.exists(f) for f in [bootloader, partitions, firmware]):
        print("merge_bin: binaries not ready - skipping merge")
        return

    esptool_path = os.path.join(
        env.subst("$PROJECT_PACKAGES_DIR"),
        "tool-esptoolpy", "esptool.py"
    )

    python = sys.executable

    # Offsets/Flags müssen zum tatsächlichen Build passen (siehe
    # platformio.ini): klassischer ESP32 (nicht S3!) lädt den Bootloader bei
    # 0x1000 statt 0x0000, Partitionstabelle bei 0x8000, boot_app0 (OTA-
    # Auswahl-Stub) bei 0xe000 (Start der "otadata"-Partition in
    # default.csv), die App selbst bei 0x10000.
    cmd = [
        python, esptool_path,
        "--chip", "esp32",
        "merge_bin",
        "-o", merged,
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "4MB",
        "0x1000",  bootloader,
        "0x8000",  partitions,
        "0xe000",  boot_app0,
        "0x10000", firmware,
    ]

    print(f"Merging -> {merged}")
    try:
        subprocess.run(cmd, check=True)
        print(f"merge_bin OK: {os.path.getsize(merged):,} bytes")
    except Exception as e:
        print(f"merge_bin failed: {e}")


env.AddPostAction("$BUILD_DIR/firmware.bin", post_firmware)
