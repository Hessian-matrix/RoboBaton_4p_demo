#!/bin/sh
# BusyBox getty -n -l calls this with "--"; never forward caller login options.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

console_login=false
case "$(tty 2>/dev/null)" in
    /dev/ttyS0) console_login=true ;;
    /dev/console)
        # A redirected/multiple kernel console must not silently gain root access.
        [ "$(cat /sys/class/tty/console/active 2>/dev/null)" != ttyS0 ] || console_login=true
        ;;
esac

if [ "$(id -u)" -eq 0 ] && [ "$console_login" = true ] &&
    [ "$(/usr/bin/uart-autologin status)" = enabled ]; then
    exec /bin/login -f root
fi
exec /bin/login
