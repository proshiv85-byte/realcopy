#!/bin/bash
# End-to-end tests for the realcopy engine, driven through the CLI.
# Usage: tests/run.sh ./realcopy
set -u

RC=$(realpath "${1:-./realcopy}")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/realcopy-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

pass=0 fail=0 skip=0
ok()   { echo "PASS  $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL  $1"; fail=$((fail + 1)); }
skipped() { echo "SKIP  $1"; skip=$((skip + 1)); }
check() { local name=$1; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
reset() { rm -rf src dst; mkdir -p src dst; }

# --- single file
reset
head -c 10M /dev/urandom > src/a.bin
touch -d '2020-01-02 03:04:05' src/a.bin
"$RC" -q src/a.bin dst
check "single file copied exactly" cmp -s src/a.bin dst/a.bin
check "modification time kept" test "$(stat -c %Y src/a.bin)" = "$(stat -c %Y dst/a.bin)"

# --- folder tree
reset
mkdir -p src/tree/sub/deeper src/tree/empty
head -c 5M /dev/urandom > src/tree/big.bin
echo hello > src/tree/sub/hello.txt
: > src/tree/sub/deeper/zero-bytes
ln -s hello.txt src/tree/sub/link
out=$("$RC" src/tree dst 2>&1)
rm src/tree/sub/link
check "folder tree copied exactly" diff -r src/tree dst/tree
check "empty folder copied" test -d dst/tree/empty
check "symlink left out and reported" grep -q "Left out 1 symlink" <<<"$out"

# --- multiple sources
reset
echo one > src/one; echo two > src/two; mkdir src/d; echo three > src/d/three
"$RC" -q src/one src/two src/d dst
check "multiple sources copied" diff -r src dst

# --- existing target: default refuses and leaves it alone
reset
echo new > src/f; echo old > dst/f
"$RC" -q src/f dst 2>/dev/null; rc=$?
check "existing file refused by default (exit 1)" test $rc -eq 1
check "existing file untouched" grep -qx old dst/f

# --- --skip-existing
reset
echo new > src/f; echo other > src/g; echo old > dst/f
"$RC" -q -n src/f src/g dst
check "--skip-existing keeps existing file" grep -qx old dst/f
check "--skip-existing copies the rest" grep -qx other dst/g

# --- --overwrite, including a larger file replaced by a smaller one
reset
echo short > src/f; head -c 1M /dev/urandom > dst/f
"$RC" -q -f src/f dst
check "--overwrite replaces and truncates" cmp -s src/f dst/f

# --- merging into an existing folder
reset
mkdir -p src/m dst/m; echo a > src/m/a; echo keep > dst/m/keep
"$RC" -q -f src/m dst
check "merge into existing folder" test -f dst/m/a -a -f dst/m/keep

# --- same file must never be truncated
reset
head -c 1M /dev/urandom > src/s; cp src/s ref
"$RC" -q -f src/s src 2>/dev/null; rc=$?
check "same file refused" test $rc -eq 1
check "same file left intact" cmp -s src/s ref

# --- folder into itself
reset
mkdir -p src/loop/inner; echo x > src/loop/x
"$RC" -q src/loop src/loop/inner 2>/dev/null; rc=$?
check "folder into itself refused" test $rc -eq 1
check "nothing created inside itself" test ! -e src/loop/inner/loop

# --- duplicate names
reset
mkdir -p src/p1 src/p2; echo 1 > src/p1/same; echo 2 > src/p2/same
"$RC" -q src/p1/same src/p2/same dst 2>/dev/null; rc=$?
check "duplicate source names refused" test $rc -eq 1
check "nothing copied on duplicate names" test ! -e dst/same

# --- verify
reset
head -c 20M /dev/urandom > src/v
out=$("$RC" -c src/v dst 2>&1)
check "--verify copies and reports verified" grep -q "Verified" <<<"$out"
check "--verify result is exact" cmp -s src/v dst/v

# --- missing source / bad target
reset
"$RC" -q src/nope dst 2>/dev/null
check "missing source fails" test $? -eq 1
echo x > src/x
"$RC" -q src/x dst/not-there 2>/dev/null
check "missing target folder fails" test $? -eq 1

# --- cancel mid-copy removes the partial file
reset
truncate -s 2G src/huge
"$RC" -q src/huge dst & pid=$!
sleep 0.3
kill -INT $pid 2>/dev/null
wait $pid; rc=$?
if [ $rc -eq 0 ]; then
    skipped "cancel (copy finished before it could be cancelled)"
else
    check "cancel exits with 130" test $rc -eq 130
    check "cancel removes the partial file" test ! -e dst/huge
fi

# --- a small filesystem (needs unprivileged user namespaces)
reset
if unshare -rm true 2>/dev/null; then
    head -c 20M /dev/urandom > src/big
    mkdir small
    out=$(unshare -rm sh -c "mount -t tmpfs -o size=8M tmpfs small && '$RC' src/big small; echo \"rc=\$?\"; ls -A small" 2>&1)
    check "not enough space detected before writing" grep -q "not enough space" <<<"$out"
    check "nothing left on the full drive" test "$(tail -n1 <<<"$out")" = "rc=1"
else
    skipped "small filesystem tests (unshare not permitted)"
fi

echo
echo "$pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
