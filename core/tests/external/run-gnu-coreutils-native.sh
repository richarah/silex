#!/bin/sh
# run-gnu-coreutils-native.sh — run the GNU coreutils suite on a NATIVE
# filesystem, then hand off to the normal runner.
#
# Why this exists: the vendored checkout lives under /mnt/c, a WSL2 drvfs
# mount where **chmod is a no-op** -- `chmod 0 f` leaves the file readable.
# Every permission-semantics test in the suite (the rm/cp/mv fail-perm
# family, sort-exit-early, the chmod result checks) therefore fails for
# the filesystem's reasons no matter how correct the applets are. Measured
# in place: 190 pass / 27 fail. Measured on ext4: 243 pass / 0 fail.
#
# The copy is made once and reused; delete $DEST to force a fresh one.
# The suite runs against a SNAPSHOT of the binary because rebuilding the
# tree while it runs makes every tool symlink fail with ETXTBSY.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="${SILEX_NATIVE_DIR:-/tmp/silex-native}"
SNAP="$DEST/silex-snapshot"
SILEX_SRC="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

if [ ! -x "$SILEX_SRC" ]; then
    echo "ERROR: silex binary not found: $SILEX_SRC"
    exit 1
fi
if [ ! -d "$SCRIPT_DIR/repos/coreutils" ]; then
    echo "ERROR: coreutils checkout not found (run fetch-all.sh)"
    exit 1
fi

case $(df -T "$DEST" 2>/dev/null | awk 'NR==2 {print $2}') in
    9p|drvfs|cifs|"")
        echo "WARNING: $DEST is not a native filesystem; results will be"
        echo "         no better than running in place." ;;
esac

mkdir -p "$DEST/repos" || exit 1
[ -d "$DEST/repos/coreutils" ] || {
    echo "Copying coreutils checkout to $DEST (once; a few minutes)..."
    cp -a "$SCRIPT_DIR/repos/coreutils" "$DEST/repos/" || exit 1
}
[ -d "$DEST/repos/gnulib" ] || cp -a "$SCRIPT_DIR/repos/gnulib" "$DEST/repos/" 2>/dev/null
cp "$SCRIPT_DIR/run-gnu-coreutils.sh" "$DEST/" || exit 1
cp "$SILEX_SRC" "$SNAP" || exit 1
chmod +x "$SNAP"

# The copied runner derives SILEX from its own location, which is now wrong;
# point it at the snapshot explicitly. Its stale-config detection notices the
# checkout moved and reconfigures for the new path.
SILEX="$SNAP" exec sh "$DEST/run-gnu-coreutils.sh"
