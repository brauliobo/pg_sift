"""A throwaway PostgreSQL cluster for a test class, on tmpfs, stopped and removed even when a test or its setup fails.

    class Live(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            cls.dir, cls.data, cls.port = cluster.start(cls, 'sift-io-selftest.', '-c shared_buffers=32MB')

The directory is made by steps/tmpfs.py (a disk is refused unless SIFT_ALLOW_DISK=1). The stop is registered before the cluster starts, so a start that fails half way is stopped too,
and it ends the postmaster by its data directory (`pg_ctl -D DATA -m immediate stop`), never by a name.
"""
import os
import shutil
import socket
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import tmpfs  # noqa: E402


def which(name):
    return shutil.which(name) or shutil.which(name, path=subprocess.run(['pg_config', '--bindir'], capture_output=True, text=True).stdout.strip() or None)


def stop(path):
    data = os.path.join(path, 'data')
    if os.path.exists(os.path.join(data, 'postmaster.pid')):
        subprocess.run([which('pg_ctl'), '-D', data, '-m', 'immediate', 'stop'], capture_output=True)
    shutil.rmtree(path, ignore_errors=True)


def start(cls, prefix, options='', root=None):
    """(directory, data directory, port) of a cluster started in a new directory on tmpfs; the sockets are in the directory; trust authentication, fsync off, no TCP listener.
    ROOT names another directory (a deliberate disk run: SIFT_ALLOW_DISK=1)"""
    try:
        path = tempfile_dir(prefix, root)
    except RuntimeError as e:
        raise unittest.SkipTest(str(e))
    cls.addClassCleanup(stop, path)
    with socket.socket() as sk:
        sk.bind(('127.0.0.1', 0))
        port = sk.getsockname()[1]
    data = os.path.join(path, 'data')
    subprocess.run([which('initdb'), '-D', data, '-U', 'postgres', '-A', 'trust', '--locale=C'], check=True, capture_output=True)
    subprocess.run([which('pg_ctl'), '-D', data, '-w', '-l', os.path.join(path, 'log'), '-o', "-p %d -k %s -c listen_addresses='' -c fsync=off %s" % (port, path, options), 'start'],
                   check=True, capture_output=True)
    return path, data, port


def tempfile_dir(prefix, root):
    import tempfile
    if root is None:
        return tmpfs.scratch(prefix)
    return tempfile.mkdtemp(prefix=prefix, dir=tmpfs.require_tmpfs(root))
