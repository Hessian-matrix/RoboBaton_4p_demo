#!/bin/sh
# Root expansion is independent of USB role/function and runs before application startup.
case "${1:-start}" in
    start)
        mount -o remount,rw / || exit 1
        mount -o remount,rw /usr/hobot || exit 1
        medium=$(tr ' ' '\n' < /proc/cmdline | awk -F= '$1 == "hobotboot.medium" {print $2; exit}')
        case "$medium" in nand|NAND|nor|NOR) exit 0 ;; esac
        device=/dev/block/platform/by-name/system
        filesystem=$(blkid -o value -s TYPE "$device") || exit 1
        if [ "$filesystem" = ext4 ]; then
            resize2fs "$device" || exit 1
        else
            echo "Skipping root expansion: $filesystem is not ext4"
        fi
        ;;
    stop) ;;
    *) echo "Usage: $0 {start|stop}" >&2; exit 1 ;;
esac
