#!/usr/bin/env python3
"""Persist NTP-client boot configuration for the X5 Buildroot SysV image."""
import argparse
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile


DEFAULT_SERVERS = [f'{index}.pool.ntp.org' for index in range(4)]


def checked_path(root, relative):
    path = root / relative
    for entry in (path, *path.parents):
        if entry == root:
            break
        if entry.is_symlink():
            raise ValueError(f'symlink in destination path: {entry}')
        if entry.exists() and entry != path and not entry.is_dir():
            raise ValueError(f'non-directory destination parent: {entry}')
    if path.exists() and not path.is_file():
        raise ValueError(f'destination must be a regular file: {path}')
    return path


def replace_file(path, content, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f'.{path.name}.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(content)
            os.fchmod(stream.fileno(), mode)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def configure(root, servers, execute=False):
    if root.is_symlink():
        raise ValueError('--root must not be a symlink')
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError('--root must be an existing directory')
    for server in servers:
        if (len(server) > 253 or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:-]*', server)
                or server.startswith('127.127.')):
            raise ValueError(f'invalid network NTP server: {server!r}')
    if not servers:
        raise ValueError('at least one NTP server is required')
    if execute and root == Path('/') and os.geteuid() != 0:
        raise ValueError('live configuration requires root')

    init = checked_path(root, 'etc/init.d/S49ntp')
    init_text = init.read_text()
    # This entry must consume the same defaults file and argument variable as Buildroot NTP.
    if not all(token in init_text for token in
               ('DAEMON="ntpd"', 'NTPD_ARGS=', '"/etc/default/$DAEMON"', '$NTPD_ARGS')):
        raise ValueError('unsupported S49ntp: expected Buildroot ntpd with /etc/default/ntpd and NTPD_ARGS')
    phc = checked_path(root, 'etc/init.d/S66phc2sys')
    config = ('# X5 network-time client; no battery-backed RTC is assumed.\n'
              '# ntpd retries unreachable sources; DNS and an NTP route must become available.\n'
              'driftfile /var/run/ntp.drift\n'
              'restrict default kod limited nomodify nopeer noquery notrap\n'
              'restrict 127.0.0.1\n'
              'restrict ::1\n' +
              ''.join(f'server {server} iburst\n' for server in dict.fromkeys(servers)))
    desired = {
        'etc/ntp.conf': (config.encode(), 0o644),
        'etc/default/ntpd': (b'NTPD_ARGS="-g -c /etc/ntp.conf"\n', 0o644),
        'etc/init.d/S49ntp': (init.read_bytes(), stat.S_IMODE(init.stat().st_mode) | 0o111),
    }
    changes = []
    for relative, (content, mode) in desired.items():
        path = checked_path(root, relative)
        if (not path.exists() or path.read_bytes() != content
                or stat.S_IMODE(path.stat().st_mode) != mode):
            changes.append((path, content, mode))
    for path, _, mode in changes:
        print(f'{"INSTALL" if execute else "PLAN"} {path} mode={mode:04o}')
    if phc.exists():
        if 'DAEMON="phc2sys"' not in phc.read_text():
            raise ValueError('unsupported S66phc2sys: inspect its ownership before disabling it')
        print(f'{"DISABLE" if execute else "PLAN DISABLE"} {phc} (move outside the S* boot scan)')
    if not execute:
        print('Preview only. Use --execute to persist this NTP-owner policy.')
        return
    if not changes and not phc.exists():
        print('Already configured; no files changed.')
        return

    backup = Path(tempfile.mkdtemp(prefix='x5-ntp-backup-', dir=root / 'etc'))
    originals = {}
    for path, _, _ in changes:
        if path.exists():
            saved = backup / path.relative_to(root)
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, saved)
            originals[path] = saved
        else:
            originals[path] = None
    (backup / 'created-files.txt').write_text(''.join(
        f'{path.relative_to(root)}\n' for path, saved in originals.items() if saved is None))
    moved_phc = False
    try:
        for path, content, mode in changes:
            replace_file(path, content, mode)
        if phc.exists():
            phc.rename(backup / 'S66phc2sys')
            moved_phc = True
    except OSError:
        for path, saved in originals.items():
            if saved is None:
                path.unlink(missing_ok=True)
            else:
                replace_file(path, saved.read_bytes(), stat.S_IMODE(saved.stat().st_mode))
        if moved_phc:
            (backup / 'S66phc2sys').rename(phc)
        raise
    print(f'Original files retained at {backup}')
    print('No running service or system clock changed. This policy takes effect at the next boot.')
    print('Do not run phc2sys/chronyd/another CLOCK_REALTIME owner alongside ntpd.')
    print('Network readiness is not time readiness: verify ntpq -pn before timestamp-sensitive capture.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/'),
                        help='existing offline rootfs directory, or / for the live filesystem')
    parser.add_argument('--server', action='append', dest='servers',
                        help='network NTP host/IP; repeat for multiple sources (default: 0..3.pool.ntp.org)')
    parser.add_argument('--execute', action='store_true', help='write configuration; default is preview only')
    args = parser.parse_args()
    try:
        configure(args.root, args.servers if args.servers is not None else DEFAULT_SERVERS, args.execute)
    except (OSError, ValueError) as error:
        parser.exit(1, f'X5 NTP boot configuration: {error}\n')


if __name__ == '__main__':
    main()
