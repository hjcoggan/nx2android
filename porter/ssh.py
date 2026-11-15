"""A Linux device over SSH, standing in for Adb where the port runs the game's own code on the device (pipeline.device_oodle_job): the Linux host
(dm/linux/build.py) on an arm64 Linux handheld instead of the Android host over adb. Only what that step uses: shell, push_file, push_tree and pull."""
import subprocess
from pathlib import Path


class SshError(Exception):
    pass


class SshDevice:
    def __init__(self, target, log=print):
        self.target, self.log = target, log
        self.base = ['ssh', '-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=30', target]

    def pick(self):   # the device answers (as Adb.pick checks one is connected)
        self.shell('true', timeout=20)
        return self.target

    def shell(self, script, timeout=None, check=True):
        try: r = subprocess.run(self.base + [script], capture_output=True, text=True, errors='replace', timeout=timeout)
        except subprocess.TimeoutExpired as e: raise SshError(f'{self.target}: timed out: {script[:80]}') from e
        if check and r.returncode not in (0,) and not r.stdout:
            raise SshError(f'{self.target}: {script[:80]}: exit {r.returncode}: {r.stderr.strip()[-300:]}')
        return r.stdout + r.stderr

    def push_file(self, local, dst, **_):
        with open(local, 'rb') as f:
            r = subprocess.run(self.base + [f'cat > {dst}'], stdin=f, capture_output=True)
        if r.returncode: raise SshError(f'{self.target}: push {local} → {dst}: {r.stderr.decode(errors="replace").strip()[-300:]}')

    def push_tree(self, local, remote, **_):   # local's contents into remote (made), as one tar stream
        tar = subprocess.Popen(['tar', '-cf', '-', '-C', str(local), '.'], stdout=subprocess.PIPE, env={'COPYFILE_DISABLE': '1'})
        r = subprocess.run(self.base + [f'mkdir -p {remote} && tar -xf - -C {remote}'], stdin=tar.stdout, capture_output=True)
        tar.stdout.close(); tar.wait()
        if r.returncode or tar.returncode: raise SshError(f'{self.target}: push {local} → {remote}: {r.stderr.decode(errors="replace").strip()[-300:]}')

    def run(self, verb, *a, timeout=None, **_):   # Adb.run('pull', remote, local)
        if verb != 'pull': raise SshError(f'ssh device: {verb} is not supported')
        remote, local = a
        with open(local, 'wb') as f:
            r = subprocess.run(self.base + [f'cat {remote}'], stdout=f, stderr=subprocess.PIPE, timeout=timeout)
        if r.returncode: raise SshError(f'{self.target}: pull {remote}: {r.stderr.decode(errors="replace").strip()[-300:]}')
        return ''


def linux_host(dm: Path, title_id: str) -> Path:
    """The Linux host built for the game (dm/linux/build.py --game <title ID>), which runs the decode job"""
    return dm / 'linux/out' / title_id / 'dmhost'
