"""Per-thread CPU busy share from a macOS `sample` report.

busy = thread samples - samples whose leaf frame is a blocking call.
usage: thread_busy.py sample.txt [min_busy_percent]
"""
import re
import sys

WAIT = re.compile(r"__psynch_cvwait|__semwait_signal|__workq_kernreturn|mach_msg2_trap|"
                  r"semaphore_\w*wait\w*_trap|kevent|__ulock_wait|__psynch_mutexwait|"
                  r"swtch_pri|__select|__sigwait|__wait4")
THREAD = re.compile(r"^\s+(\d+) Thread_\d+:?\s*(.*)")
FRAME = re.compile(r"^([\s+!:|]*)(\d+) (.*)")


def parse(lines):
    threads = []
    frames = []
    for line in lines:
        m = THREAD.match(line)
        if m:
            frames = []
            threads.append({"name": m.group(2)[:45] or "(unnamed)", "total": int(m.group(1)),
                            "frames": frames})
            continue
        if not threads or line.startswith(("Total number", "Sort by top", "Binary Images")):
            if line.startswith(("Total number", "Sort by top")):
                break
            continue
        m = FRAME.match(line)
        if m:
            frames.append((len(m.group(1)), int(m.group(2)), m.group(3)))
    return threads


def wait_samples(frames):
    waiting = 0
    for i, (depth, count, sym) in enumerate(frames):
        is_leaf = i + 1 == len(frames) or frames[i + 1][0] <= depth
        if is_leaf and WAIT.search(sym):
            waiting += count
    return waiting


def main():
    min_busy = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    rows = []
    threads = parse(open(sys.argv[1]).read().split("\n"))
    longest = max(t["total"] for t in threads)
    for t in threads:
        if t["total"] < longest // 2:  # skip short-lived workers
            continue
        busy = 100.0 * (t["total"] - wait_samples(t["frames"])) / max(t["total"], 1)
        rows.append((busy, t["name"]))
    for busy, name in sorted(rows, reverse=True):
        if busy >= min_busy:
            print(f"{busy:5.1f}% busy  {name}")


if __name__ == "__main__":
    main()
