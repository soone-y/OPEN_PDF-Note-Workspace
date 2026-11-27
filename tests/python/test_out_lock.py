"""Behavior of scripts/build/out_lock.ps1, the build / GUI-test reader-writer lock."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts/build/out_lock.ps1"


def shells():
    found = [shell for shell in ("powershell.exe", "pwsh") if shutil.which(shell)]
    return found


class OutLockTests(unittest.TestCase):
    def setUp(self):
        self.out = Path(tempfile.mkdtemp(prefix="out_lock_"))
        self.addCleanup(shutil.rmtree, self.out, ignore_errors=True)

    def hold(self, shell, mode, label, seconds=120):
        ready = self.out / f"ready_{label}.txt"
        script = (f". '{HELPER}'; $l = Enter-OutLock -OutRoot '{self.out}' -Mode {mode} -Label '{label}'; "
                  f"Set-Content -LiteralPath '{ready}' -Value ok; Start-Sleep -Seconds {seconds}; Exit-OutLock $l")
        process = subprocess.Popen([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.time() + 30
        while not ready.exists():
            if process.poll() is not None:
                self.fail("lock holder exited early: " + process.stderr.read().decode(errors="replace"))
            self.assertLess(time.time(), deadline, "lock holder did not start")
            time.sleep(0.1)
        return process

    def try_lock(self, shell, mode, timeout):
        script = (f"$ErrorActionPreference = 'Stop'; . '{HELPER}'; $l = Enter-OutLock -OutRoot '{self.out}' -Mode {mode} -Label 'probe' "
                  f"-TimeoutSeconds {timeout}; Exit-OutLock $l; 'acquired'")
        return subprocess.run([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                              capture_output=True, timeout=60)

    def test_shared_holders_coexist_and_block_exclusive(self):
        for shell in shells():
            with self.subTest(shell=shell):
                holder = self.hold(shell, "Shared", "test-a")
                shared = self.try_lock(shell, "Shared", 2)
                self.assertIn(b"acquired", shared.stdout, shared.stderr.decode(errors="replace"))
                exclusive = self.try_lock(shell, "Exclusive", 2)
                output = (exclusive.stdout + exclusive.stderr).decode(errors="replace")
                self.assertNotEqual(exclusive.returncode, 0, output)
                self.assertIn("Timed out", output)
                self.assertIn("shared test-a", output, "the wait must name the holder")
                holder.kill()
                holder.wait(10)
                # The OS releases the lock when the holder dies; its stale note is ignored.
                after = self.try_lock(shell, "Exclusive", 10)
                self.assertIn(b"acquired", after.stdout, after.stderr.decode(errors="replace"))

    def test_exclusive_holder_blocks_shared(self):
        shell = shells()[0]
        holder = self.hold(shell, "Exclusive", "build-x", seconds=4)
        waited = self.try_lock(shell, "Shared", 30)
        output = (waited.stdout + waited.stderr).decode(errors="replace")
        self.assertIn("Waiting for out/build.lock", output)
        self.assertIn("exclusive build-x", output)
        self.assertIn("acquired", output, "a waiting test must continue once the build ends")
        holder.wait(30)

    def test_build_and_gui_entry_use_the_shared_helper(self):
        for rel in ("scripts/build/build_workspace.ps1", "scripts/build/build_readonly_viewer.ps1"):
            text = (ROOT / rel).read_text(encoding="utf-8-sig")
            self.assertIn("Enter-OutLock -OutRoot $outRoot -Mode Exclusive", text)
            self.assertIn("Exit-OutLock $globalLockStream", text)
            self.assertIn("Write-OutBuildSource", text)
            self.assertNotIn("Another build is currently running", text)
        gui = (ROOT / "tests/scripts/app_test_desktop.ps1").read_text(encoding="utf-8-sig")
        entry = gui.split("Enter-OutLock", 1)
        self.assertIn("-Mode Shared", entry[1].split("\n", 1)[0])
        # The lock is taken only on the relaunch path, after the early returns.
        self.assertLess(gui.index("if ($context -eq 'interactive') { return $false }"), gui.index("Enter-OutLock"))
        self.assertIn("Exit-OutLock $outLock", gui.split("} finally {", 1)[1])


if __name__ == "__main__":
    unittest.main()
