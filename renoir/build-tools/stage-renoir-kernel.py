#!/usr/bin/env python3
"""Stage matched 6.18 kernel and DT artifacts for LineageOS."""

import collections
import pathlib
import shutil
import subprocess
import sys
import tempfile


def index_modules(path):
    modules = collections.defaultdict(list)
    for module in sorted(path.rglob("*.ko")):
        modules[module.name].append(module)
    return modules


def main():
    root, work = map(pathlib.Path, sys.argv[1:])
    kernel = root / "redwood-6.18-kernel"
    out = work / "out"
    dt = work / "dt"
    destination = root / "device/xiaomi/renoir-kernel-6.18"
    release = (out / "include/config/kernel.release").read_text().strip()
    if not release.startswith("6.18.") or not release.endswith("-Tempest"):
        raise SystemExit(f"Expected a 6.18 Tempest build, got {release}")
    modules = index_modules(out)
    modules.update(index_modules(work / "vb"))
    names = (kernel / "renoir/modules.load").read_text().splitlines()
    names = [line for line in names if line and not line.startswith("#")]
    missing = [name for name in names if name not in modules]
    ambiguous = [name for name in names if len(modules.get(name, [])) > 1]
    if missing or ambiguous:
        raise SystemExit(f"Missing modules {missing}\nAmbiguous modules {ambiguous}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".renoir-stage-", dir=destination.parent) as tmp:
        stage = pathlib.Path(tmp)
        ramdisk = stage / "vendor_ramdisk"
        ramdisk.mkdir()
        for name in names:
            source = modules[name][0]
            vermagic = subprocess.check_output(
                ["modinfo", "-F", "vermagic", source], text=True
            ).split()
            if not vermagic or vermagic[0] != release:
                raise SystemExit(f"Unexpected vermagic for {source}: {vermagic}")
            subprocess.run(
                ["llvm-strip", "--strip-debug", "-o", ramdisk / name, source],
                check=True,
            )
        (ramdisk / "modules.load").write_text("\n".join(names) + "\n")
        shutil.copyfile(out / "arch/arm64/boot/Image", stage / "Image")
        shutil.copyfile(dt / "dtbo.img", stage / "dtbo.img")
        (stage / "dtb").mkdir()
        shutil.copyfile(dt / "shima.dtb", stage / "dtb/shima.dtb")

        target = stage / "lib/modules" / release
        target.parent.mkdir(parents=True)
        target.symlink_to(ramdisk, target_is_directory=True)
        for name in ("modules.builtin", "modules.builtin.modinfo"):
            shutil.copyfile(out / name, ramdisk / name)
        (ramdisk / "modules.order").write_text("\n".join(names) + "\n")
        depmod = subprocess.run(
            ["depmod", "-e", "-F", out / "System.map", "-b", stage, release],
            check=True,
            capture_output=True,
            text=True,
        )
        if "unknown symbol" in depmod.stderr or "disagrees about version" in depmod.stderr:
            raise SystemExit(depmod.stderr)
        dependencies = ramdisk / "modules.dep"
        indexed = {line.split(":", 1)[0] for line in dependencies.read_text().splitlines()}
        if indexed != set(names):
            raise SystemExit("depmod did not index the complete module set")

        destination.mkdir(exist_ok=True)
        for name in ["dtb", "vendor_ramdisk"]:
            previous = destination / name
            if previous.is_dir():
                shutil.rmtree(previous)
            shutil.move(stage / name, previous)
        for name in ["Image", "dtbo.img"]:
            (stage / name).replace(destination / name)
        print(f"Staged {release} and {len(names)} modules in {destination}")


if __name__ == "__main__":
    main()
