"""Scratch space on tmpfs, for every test and harness that makes a directory (stdlib only).

A throwaway cluster or a temporary directory on a disk wears it all day, so scratch is memory: `scratch(prefix)` makes a directory under /dev/shm (SIFT_SCRATCH names another tmpfs
directory) and refuses a root that is not tmpfs unless SIFT_ALLOW_DISK=1 is set, which a default run never does. Importing this module points `tempfile` at tmpfs when its default
directory is a disk, so a plain `tempfile.mkdtemp()` of a test file that imports it is on tmpfs too (scripts/check-scratch.sh requires the import).
"""
import os
import tempfile

SHM = '/dev/shm'


def fs_type(path):
    """the file system type of the longest mount point that holds PATH ('' when /proc/mounts cannot say)"""
    best = ('', '')
    try:
        with open('/proc/mounts') as f:
            for line in f:
                _, mnt, typ = line.split()[:3]
                mnt = mnt.replace('\\040', ' ')
                if (path == mnt or path.startswith(mnt.rstrip('/') + '/')) and len(mnt) >= len(best[0]):
                    best = (mnt, typ)
    except OSError:
        pass
    return best[1]


def allow_disk():
    return os.environ.get('SIFT_ALLOW_DISK') == '1'


def require_tmpfs(path):
    """PATH itself when it is on tmpfs (or SIFT_ALLOW_DISK=1), else a RuntimeError that names the file system"""
    fs = fs_type(os.path.realpath(path))
    if fs != 'tmpfs' and not allow_disk():
        raise RuntimeError('%s is on %s, not tmpfs: scratch space wears a disk (SIFT_SCRATCH names a tmpfs directory; SIFT_ALLOW_DISK=1 allows a disk for one deliberate run)' % (path, fs or 'an unknown file system'))
    return path


def root():
    return os.environ.get('SIFT_SCRATCH') or SHM


def scratch(prefix='sift.'):
    """a new directory on tmpfs; the caller removes it"""
    return tempfile.mkdtemp(prefix=prefix, dir=require_tmpfs(root()))


def use():
    """point tempfile at tmpfs when its default is not, where there is a tmpfs to use (Linux); a disk default stays only with SIFT_ALLOW_DISK=1 or no tmpfs"""
    if fs_type(os.path.realpath(tempfile.gettempdir())) != 'tmpfs' and not allow_disk() and fs_type(root()) == 'tmpfs':
        tempfile.tempdir = root()


use()
