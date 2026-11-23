# The quiet-host guard: before every timed block scripts/quiet.sh judges the host by its pressure stall information.
# A block timed on a host that did not quiet down is marked noisy and cannot decide a verdict.

QUIET_SCRIPT = ROOT/'scripts/quiet.sh'
QUIET_MAX_PERCENT = 10
QUIET_TIMEOUT_SECONDS = 120
# after one block timed noisy the host is only checked once more, briefly, before each later block: a busy host must not eat the budget block by block
NOISY_RECHECK_SECONDS = 6
QUIET_OK, QUIET_TIMEOUT, QUIET_NO_PSI = 0, 1, 3


def quiet_script(*args):
    return subprocess.run([str(QUIET_SCRIPT),*map(str,args)],capture_output=True,text=True)


class QuietGuard:
    def __init__(self, enabled, maximum=QUIET_MAX_PERCENT, timeout=QUIET_TIMEOUT_SECONDS, script=quiet_script):
        self.enabled,self.maximum,self.timeout,self.script = enabled,maximum,timeout,script
        self.blocks = self.noisy_blocks = 0
        self.waited_seconds = 0.0
        self.recheck = False
        self.last = None

    def settle(self, deadline):
        """wait for a quiet host within the budget; True when the next block may be timed"""
        if not self.enabled:
            return True
        limit = NOISY_RECHECK_SECONDS if self.recheck else self.timeout
        start = time.monotonic()
        result = self.script('wait','--max',self.maximum,'--timeout',max(0,math.floor(min(limit,deadline-start))))
        self.waited_seconds += time.monotonic()-start
        self.blocks += 1
        quiet = result.returncode==QUIET_OK
        self.noisy_blocks += not quiet
        self.recheck = not quiet
        self.last = result.stderr.strip().splitlines()[-1] if result.stderr.strip() else ''
        return quiet

    def summary(self):
        pressure = self.script('report').stdout.strip() if self.enabled else ''
        return dict(enabled=self.enabled,max_percent=self.maximum,timeout_seconds=self.timeout,blocks=self.blocks,
                    noisy_blocks=self.noisy_blocks,waited_seconds=self.waited_seconds,pressure=pressure,last=self.last)


QUIET = QuietGuard(False)
