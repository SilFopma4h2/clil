#!/usr/bin/env bash
# Small end-to-end tasks against a real model, for before/after numbers.
# usage: tests/bench/run.sh [clil options]   e.g. tests/bench/run.sh -m qwen3.6-35b-a3b
set -u
CLIL=${CLIL:-$(cd "$(dirname "$0")/../.." && pwd)/clil}
OPTS=("$@")
pass=0 total=0 secs=0 calls=0

# task NAME SETUP PROMPT CHECK: SETUP and CHECK run with sh in a fresh directory
task() {
    local d t0 t n ok
    d=$(mktemp -d)
    (cd "$d" && sh -c "$2")
    t0=$(date +%s)
    (cd "$d" && "$CLIL" -y --max-steps 15 "${OPTS[@]}" "$3" </dev/null >out.log 2>err.log)
    t=$(($(date +%s) - t0))
    n=$(grep -c '●' "$d/err.log")
    if (cd "$d" && sh -c "$4") >/dev/null 2>&1; then ok=pass pass=$((pass + 1)); else ok=FAIL; fi
    total=$((total + 1)) secs=$((secs + t)) calls=$((calls + n))
    printf '%-8s %-4s %4ss %3s tool calls   %s\n' "$1" "$ok" "$t" "$n" "$d"
}

task create 'true' \
    'Create hello.txt containing exactly the line: hello world' \
    'test "$(cat hello.txt)" = "hello world"'

task rename 'printf "int add(int a, int b) { return a + b; }\nint main(void) { return add(1, 2) - 3; }\n" > calc.c' \
    'In calc.c, rename the function add to sum everywhere.' \
    'grep -q "sum(1, 2)" calc.c && grep -q "int sum(" calc.c && ! grep -q "add(" calc.c'

task insert 'printf "int add(int a, int b) { return a + b; }\n\nint main(void) { return 0; }\n" > calc.c' \
    'Add a C function sub(int a, int b) that returns a - b to calc.c, right after add.' \
    'cc -c calc.c -o /dev/null && grep -q "a - b" calc.c'

task fix 'printf "#include <stdio.h>\nint max(int a, int b) { return a < b ? a : b; }\nint main(void) { printf(\"%%d\\\\n\", max(3, 7)); return 0; }\n" > max.c' \
    'max.c prints the wrong number. Fix the bug in max() so it returns the larger value. Do not change main.' \
    'cc max.c -o m && test "$(./m)" = 7'

task find 'mkdir -p src/net src/ui && echo "#define PORT 8080" > src/net/conf.h && echo "#define COLOR 1" > src/ui/theme.h' \
    'Which file defines PORT? Write only its path (relative to here) into answer.txt.' \
    'grep -q "src/net/conf.h" answer.txt'

printf '\n%d/%d passed · %ss · %d tool calls\n' "$pass" "$total" "$secs" "$calls"
[ "$pass" -eq "$total" ]
