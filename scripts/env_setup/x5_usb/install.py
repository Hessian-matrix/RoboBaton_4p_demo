#!/usr/bin/env python3
"""Install the USB package into the revised carrier's offline SDK rootfs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


OBSOLETE = (
    "usr/bin/x5-usb-rndis", "etc/init.d/usb-gadget.sh", "etc/init.d/usb3.0-gadget.sh",
    "etc/init.d/uvc-gadget.sh", "etc/init.d/S92adb", "etc/udev/rules.d/99-usb-gadget.rules",
    "usr/bin/uvc-uac2-ecm.sh", "usr/bin/uvc-uac2-rndis.sh",
)


def install(root, package):
    root = root.resolve(strict=True)
    if root == Path("/") or not (root / "etc/init.d").is_dir():
        raise ValueError("expected an offline SDK rootfs, never /")
    for relative in ("usr/bin/python3", "usr/bin/adbd", "usr/sbin/dnsmasq", "root/demo/lib/libsc132.so.3"):
        # Absolute rootfs links must be resolved inside the target, not on the build host.
        path = root / relative
        for _ in range(10):
            if not path.is_symlink():
                break
            link = Path(os.readlink(path))
            path = root / str(link).lstrip("/") if link.is_absolute() else path.parent / link
        if not path.is_file():
            raise ValueError("missing USB runtime dependency: " + relative)
    backend = package / "x5-uvc-mosaic"
    header = backend.read_bytes()[:20]
    if header[:4] != b"\x7fELF" or header[18:20] != b"\xb7\x00":
        raise ValueError("UVC backend must be an AArch64 ELF")
    default = root / "etc/4p-usb-default.json"
    if default.is_symlink():
        raise ValueError("USB boot default must not be a symlink")
    if default.exists() and json.loads(default.read_text()).get("mode") not in ("host", "rndis", "ecm", "ncm"):
        raise ValueError("invalid existing USB boot default")
    profiles = root / "etc/init.d/.usb"
    if profiles.is_symlink():
        raise ValueError("SDK USB profiles must not be a symlink")
    for source in ("switch_usb_gadget", "S98rootfs.sh", "S99usb.sh", "x5-uvc-mosaic"):
        if not (package / source).is_file():
            raise ValueError("missing USB package file: " + source)
    for relative in (*OBSOLETE, "usr/bin/switch_usb_gadget", "usr/bin/x5-uvc-mosaic",
                     "etc/init.d/S98rootfs.sh", "etc/init.d/S99usb.sh", "usr/share/4p-usb/manifest.json", "etc/4p-usb-default.json"):
        target = root / relative
        if relative not in OBSOLETE and target.is_symlink():
            raise ValueError("symlink in offline destination: " + str(target))
        for parent in target.parents:
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError("symlink in offline destination: " + str(parent))
    for relative in OBSOLETE:
        (root / relative).unlink(missing_ok=True)
    if profiles.is_dir():
        shutil.rmtree(profiles)
    inventory = {}
    for source_name, destination in (("switch_usb_gadget", "usr/bin/switch_usb_gadget"),
                                     ("S98rootfs.sh", "etc/init.d/S98rootfs.sh"),
                                     ("S99usb.sh", "etc/init.d/S99usb.sh"),
                                     ("x5-uvc-mosaic", "usr/bin/x5-uvc-mosaic")):
        source, target = package / source_name, root / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        target.unlink(missing_ok=True)
        shutil.copyfile(source, target)
        target.chmod(0o755)
        inventory[destination] = hashlib.sha256(target.read_bytes()).hexdigest()
    if not default.exists():
        default.write_text('{"mode":"ncm"}\n')
    producer = root / "root/demo/lib/libsc132.so.3"
    manifest = dict(inventory=inventory,
                    sc132_sha256=hashlib.sha256(producer.read_bytes()).hexdigest(),
                    uvc=dict(format="MJPEG", width=1280, height=960, fps=30,
                             layout=[["CAM1", "CAM2"], ["CAM3", "CAM4"]]),
                    board="USB3-default revised carrier", default_mode=json.loads(default.read_text())["mode"])
    target = root / "usr/share/4p-usb/manifest.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(manifest, indent=2) + "\n")
    print("Installed USB3 Host/Device manager and real SC132 mosaic UVC backend into", root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--package", required=True, type=Path,
                        help="staged directory containing x5-uvc-mosaic and the three runtime scripts")
    args = parser.parse_args()
    try:
        install(args.rootfs, args.package.resolve(strict=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit("USB image integration: " + str(error))
