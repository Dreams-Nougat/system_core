#!/usr/bin/python
"""
Simple conformance test for adb.
"""
import hashlib
import os
import random
import re
import subprocess
import tempfile
import unittest

def call(cmd_str):
    """Run process and return output tuple (stdout, stderr, ret code)"""
    process = subprocess.Popen(cmd_str.split(),
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    stdout, stderr = process.communicate()
    return stdout, stderr, process.returncode


def call_combine(cmd_str):
    """Run process and return output tuple (stdout, stderr, ret code)"""
    process = subprocess.Popen(cmd_str.split(),
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    stdout, _ = process.communicate()
    return stdout, process.returncode


def call_checked(cmd_str):
    """Run process and get stdout+stderr, raise an exception on trouble"""
    return subprocess.check_output(cmd_str.split(), stderr=subprocess.STDOUT)


def call_checked_list(cmd_str):
    return call_checked(cmd_str).split('\n')


def call_checked_list_skip(cmd_str):
    out_list = call_checked_list(cmd_str)
    def is_init_line(line):
        if (len(line) >= 3) and (line[0] == "*") and (line[-2] == "*"):
            return True
        else:
            return False
    return [line for line in out_list if not is_init_line(line)]


def get_device_list(qualifiers=False):
    output = call_checked_list_skip("adb devices")
    dev_list = []
    for line in output[1:]:
        if line.strip() == "":
            continue
        device, _ = line.split()
        dev_list.append(device)
    return dev_list


def get_attached_device_count():
    return len(get_device_list())


def compute_md5(string):
    hsh = hashlib.md5()
    hsh.update(string)
    return hsh.hexdigest()


class TempFile:
    def __init__(self, handle, md5):
        self.handle = handle
        self.md5 = md5
        self.host_full_path = handle.name
        self.base_name = os.path.basename(self.host_full_path)


def make_random_host_dir(in_dir, num_files, rand_size=True):
    files = {}
    min_size = 4 * (1 << 10)
    max_size = 128 * (1 << 10)
    fixed_size = min_size

    for i in range(num_files):
        file_handle = tempfile.NamedTemporaryFile(dir=in_dir)

        if rand_size:
            size = random.randrange(min_size, max_size, 1024)
        else:
            size = fixed_size
        rand_str = os.urandom(size)
        file_handle.write(rand_str)
        file_handle.flush()

        md5 = compute_md5(rand_str)
        files[file_handle.name] = TempFile(file_handle, md5)
    return files


class AdbWrapper(object):

    """ Convenience wrapper object for the adb command """
    def __init__(self, device=None, out_dir=None):
        self.device = device
        self.out_dir = out_dir
        self.adb_cmd = "adb "
        if self.device:
            self.adb_cmd += "-s {} ".format(device)
        if self.out_dir:
            self.adb_cmd += "-p {} ".format(out_dir)

    def shell(self, cmd):
        return call_checked(self.adb_cmd + "shell " + cmd)

    def shell_nocheck(self, cmd):
        return call_combine(self.adb_cmd + "shell " + cmd)

    def push(self, local, remote):
        return call_checked(self.adb_cmd + "push {} {}".format(local, remote))

    def pull(self, remote, local):
        return call_checked(self.adb_cmd + "pull {} {}".format(remote, local))

    def sync(self, directory=""):
        return call_checked(self.adb_cmd + "sync {}".format(directory))

class AdbBasic(unittest.TestCase):

    def test_devices(self):
        """ Get uptime for each device plugged in from /proc/uptime """
        dev_list = get_device_list()
        for device in dev_list:
            out = call_checked("adb -s {} shell cat /proc/uptime".format(device))
            self.assertTrue(len(out.split()) == 2)
            self.assertTrue(float(out.split()[0]) > 0.0)
            self.assertTrue(float(out.split()[1]) > 0.0)

    def test_devices_with_qualifiers(self):
        """ Get uptime for each device plugged in from /proc/uptime """
        dev_list = get_device_list(qualifiers=True)
        for device in dev_list:
            out = call_checked("adb -s {} shell cat /proc/uptime".format(device))
            self.assertTrue(len(out.split()) == 2)
            self.assertTrue(float(out.split()[0]) > 0.0)
            self.assertTrue(float(out.split()[1]) > 0.0)

    def test_help(self):
        """ Make sure we get _something_ out of help """
        out = call_checked("adb help")
        self.assertTrue(len(out) > 0)

    def test_version(self):
        """ Get a version number out of the output of adb """
        out = call_checked("adb version").split()
        version_num = False
        for item in out:
            if re.match(r"[\d+\.]*\d", item):
                version_num = True
        self.assertTrue(version_num)



class AdbFile(unittest.TestCase):

    SCRATCH_DIR = "/data/local/tmp"
    DEVICE_TEMP_FILE = SCRATCH_DIR + "/adb_test_file"
    DEVICE_TEMP_DIR = SCRATCH_DIR + "/adb_test_dir"

    def test_push(self):
        """ Push a file to all attached devices """
        dev_list = get_device_list()
        for device in dev_list:
            self.push_with_device(device)

    def push_with_device(self, device):
        """ Push a randomly generated file to specified device """
        kbytes = 512
        adb = AdbWrapper(device)
        with tempfile.NamedTemporaryFile(mode="w") as tmp:
            rand_str = os.urandom(1024 * kbytes)
            tmp.write(rand_str)
            tmp.flush()

            host_md5 = compute_md5(rand_str)
            adb.shell_nocheck("rm -r {}".format(AdbFile.DEVICE_TEMP_FILE))
            try:
                adb.push(local=tmp.name, remote=AdbFile.DEVICE_TEMP_FILE)
                dev_md5, _ = adb.shell("md5 {}".format(AdbFile.DEVICE_TEMP_FILE)).split()
                self.assertEqual(host_md5, dev_md5)
            finally:
                adb.shell_nocheck("rm {}".format(AdbFile.DEVICE_TEMP_FILE))

    def test_push_directory(self):
        """ Push a directory to all attached devices """
        dev_list = get_device_list()
        for device in dev_list:
            self.push_dir_with_device(device)

    def push_dir_with_device(self, device):
        """ Push a randomly generated directory of files to specified device """
        try:
            temp_files = {}

            # create temporary host directory
            base_dir = tempfile.mkdtemp()

            # create mirror device directory hierarchy within base_dir
            full_dir_path = base_dir + AdbFile.DEVICE_TEMP_DIR
            os.makedirs(full_dir_path)

            # create 32 random files within the host mirror
            temp_files = make_random_host_dir(in_dir=full_dir_path, num_files=32)

            # clean up any trash on the device
            adb = AdbWrapper(device, out_dir=base_dir)
            adb.shell_nocheck("rm -r {}".format(AdbFile.DEVICE_TEMP_DIR))

            # issue the sync
            adb.sync("data")

            # confirm that every file on the device mirrors that on the host
            for host_full_path in temp_files.keys():
                device_full_path = AdbFile.DEVICE_TEMP_DIR + "/" + temp_files[host_full_path].base_name
                dev_md5, _ = adb.shell("md5 {}".format(device_full_path)).split()
                self.assertEqual(temp_files[host_full_path].md5, dev_md5)

        finally:
            if temp_files:
                for tf in temp_files.values():
                    tf.handle.close()
            if base_dir:
                os.removedirs(base_dir + AdbFile.DEVICE_TEMP_DIR)

    def test_pull(self):
        """ Pull a file from all attached devices """
        dev_list = get_device_list()
        for device in dev_list:
            self.pull_with_device(device)

    def pull_with_device(self, device):
        """ Pull a randomly generated file from specified device """
        kbytes = 512
        adb = AdbWrapper(device)
        adb.shell_nocheck("rm -r {}".format(AdbFile.DEVICE_TEMP_FILE))
        try:
            adb.shell("dd if=/dev/urandom of={} bs=1024 count={}".format(AdbFile.DEVICE_TEMP_FILE, kbytes))
            dev_md5 = adb.shell("md5 {}".format(AdbFile.DEVICE_TEMP_FILE)).split()[0]

            with tempfile.NamedTemporaryFile(mode="w") as tmp_write:
                adb.pull(remote=AdbFile.DEVICE_TEMP_FILE, local=tmp_write.name)
                with open(tmp_write.name) as tmp_read:
                    host_contents = tmp_read.read()
                    host_md5 = compute_md5(host_contents)
                self.assertEqual(dev_md5, host_md5)
        finally:
            adb.shell_nocheck("rm {}".format(device, AdbFile.DEVICE_TEMP_FILE))

    def test_sync(self):
        """ Sync a directory with all attached devices """
        dev_list = get_device_list()
        for device in dev_list:
            self.sync_with_device(device)

    def sync_with_device(self, device):
        pass

if __name__ == '__main__':
    random.seed(0)
    dev_count = get_attached_device_count()
    if dev_count:
        suite = unittest.TestLoader().loadTestsFromName(__name__)
        unittest.TextTestRunner(verbosity=3).run(suite)
    else:
        print "Test suite must be run with attached devices"


