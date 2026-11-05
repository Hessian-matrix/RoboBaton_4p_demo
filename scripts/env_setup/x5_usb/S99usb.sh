#!/bin/sh
# USB networking starts independently of the sensor application's resource owner.
case "${1:-start}" in
    start)
        mount -o remount,rw / || exit 1
        /usr/bin/switch_usb_gadget boot
        ;;
    stop)
        # Writable MSD must be ejected by the computer before shutdown.
        /usr/bin/switch_usb_gadget stop
        ;;
    *) echo "Usage: $0 {start|stop}" >&2; exit 1 ;;
esac
