#!/bin/sh
# tests/unit/shell/test_interactive.sh — interactive REPL + terminal job control
# Usage: ./test_interactive.sh [path/to/silex]
#
# Drives silex under a pseudo-terminal (pty) to exercise behaviour that only
# exists for an interactive shell with a controlling terminal:
#   - a command runs on its own Enter (not one line behind);
#   - PS2 continuation of a multi-line construct;
#   - Ctrl-Z stops the foreground job (monitor mode), `fg`/`bg` resume it;
#   - Ctrl-C at the prompt cancels the line without killing the shell.
#
# Reads until each expected marker appears (bounded by a timeout) rather than
# sleeping fixed amounts, so it is not timing-sensitive. Needs python3 for
# pty.fork(); if that is missing the suite skips (exit 0) rather than failing.

SILEX="${1:-build/bin/silex}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available (pty harness needs it)"
    exit 0
fi

# Absolute path: the harness execv()s it from the same cwd, but be explicit.
case "$SILEX" in
    /*) ABS_SILEX="$SILEX" ;;
    *)  ABS_SILEX="$(pwd)/$SILEX" ;;
esac

SILEX="$ABS_SILEX" python3 - <<'PY'
import os, pty, sys, time, select, signal

SILEX = os.environ["SILEX"]
PASS = 0
FAIL = 0

def report(name, ok, detail=b""):
    global PASS, FAIL
    if ok:
        print("PASS: " + name); PASS += 1
    else:
        print("FAIL: " + name)
        if detail:
            print("  output: " + repr(detail[-300:]))
        FAIL += 1

def spawn():
    pid, fd = pty.fork()
    if pid == 0:
        # Deterministic prompts; -i forces interactive.
        os.environ["PS1"] = "$ "
        os.environ["PS2"] = "> "
        os.execv(SILEX, [SILEX, "-i"])
        os._exit(127)
    return pid, fd

def kill(pid):
    try:
        os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
    except OSError:
        pass

def read_until(fd, acc, marker, timeout):
    """Drain fd into acc until `marker` (bytes) appears or timeout elapses."""
    end = time.time() + timeout
    while time.time() < end:
        if marker in acc:
            return True
        r, _, _ = select.select([fd], [], [], 0.15)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                break
            if not d:
                break
            acc.extend(d)
    return marker in acc

def settle(fd, acc, t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return
            if not d:
                return
            acc.extend(d)

# --- test 1: a single command runs on its own Enter -------------------------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.4)
os.write(fd, b"echo MARK_ONE\n")
report("command runs on its own Enter", read_until(fd, acc, b"MARK_ONE", 4), acc)
kill(pid)

# --- test 2: two commands, the first not held back a line -------------------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.4)
os.write(fd, b"echo AAA\n")
ok_first = read_until(fd, acc, b"AAA", 4)   # must appear before sending the 2nd
os.write(fd, b"echo BBB\n")
ok_second = read_until(fd, acc, b"BBB", 4)
report("first command not delayed until the next line", ok_first and ok_second, acc)
kill(pid)

# --- test 3: PS2 continuation of a multi-line for loop ----------------------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.4)
os.write(fd, b"for i in a b c\n")
os.write(fd, b"do echo LOOP_$i\n")
os.write(fd, b"done\n")
ok = (read_until(fd, acc, b"LOOP_a", 4) and
      b"LOOP_b" in acc and read_until(fd, acc, b"LOOP_c", 4))
report("multi-line for loop via PS2 continuation", ok, acc)
kill(pid)

# --- test 4: Ctrl-Z stops the foreground job, jobs lists it Stopped ---------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.4)
os.write(fd, b"sleep 5\n")
settle(fd, acc, 0.4)              # let sleep take the foreground
os.write(fd, b"\x1a")            # Ctrl-Z
ok_stop = read_until(fd, acc, b"Stopped", 4)
os.write(fd, b"jobs\n")
ok_jobs = read_until(fd, acc, b"Stopped", 3)
report("Ctrl-Z stops the foreground job", ok_stop and ok_jobs, acc)
kill(pid)

# --- test 5: bg resumes a stopped job (Stopped -> Running) ------------------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.4)
os.write(fd, b"sleep 5\n")
settle(fd, acc, 0.4)
os.write(fd, b"\x1a")
read_until(fd, acc, b"Stopped", 4)
os.write(fd, b"bg\n")
settle(fd, acc, 0.4)
del acc[:]                        # only look at jobs output after bg
os.write(fd, b"jobs\n")
report("bg resumes a stopped job (Running)", read_until(fd, acc, b"Running", 4), acc)
kill(pid)

# --- test 6: fg brings a stopped job back and runs it to completion ---------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.4)
os.write(fd, b"sleep 1\n")
settle(fd, acc, 0.3)
os.write(fd, b"\x1a")
read_until(fd, acc, b"Stopped", 4)
os.write(fd, b"fg\n")
settle(fd, acc, 2.0)             # resumes; sleep finishes; fresh prompt returns
os.write(fd, b"echo AFTER_FG\n")
report("fg resumes a stopped job to completion",
       read_until(fd, acc, b"AFTER_FG", 4), acc)
kill(pid)

# --- test 7: Ctrl-C at the prompt cancels the line, shell survives ----------
pid, fd = spawn(); acc = bytearray()
settle(fd, acc, 0.5)             # ensure the SIGINT handler is installed
os.write(fd, b"\x03")           # Ctrl-C at an empty prompt
settle(fd, acc, 0.3)
os.write(fd, b"echo SURVIVED\n")
report("Ctrl-C at the prompt does not kill the shell",
       read_until(fd, acc, b"SURVIVED", 4), acc)
kill(pid)

print("")
print("interactive tests: %d passed, %d failed" % (PASS, FAIL))
sys.exit(1 if FAIL else 0)
PY