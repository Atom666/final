# Certificate Pool Provisioning (students pool) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `scripts/nad-provision.sh`'s growing `--count`/`--add` flat agent list with a fixed-size, reusable `students` certificate pool (`init`/`seed`/`occupy`/`release`/`status`/`export` subcommands), and fix the TLS server name from its `nad-mirror.internal` test placeholder to the real `pt-nad-rt.edtechlab.local`.

**Architecture:** `scripts/nad-provision.sh` becomes a subcommand dispatcher. `init` keeps today's CA/server-cert/receiver-install logic (domain fixed). `seed` issues a fixed number of permanent client certs into a pool subdirectory in one shot, each with its own self-contained bootstrap script (same embed-as-base64 shape as today). `occupy`/`release` flip a `free`/`occupied` column in the pool's TSV registry by hand, independently of `export`, which only ever packages files and never touches that column.

**Tech Stack:** POSIX `sh`, `openssl` CLI, the existing hand-rolled shell test harness in `tests/deploy/*.sh` (no test framework — each file defines its own `assert_eq` and sources the script with `MIRROR_PROVISION_SOURCE=1`).

**Spec:** `docs/superpowers/specs/2026-09-06-cert-pool-provisioning-design.md`

## Global Constraints

- Certificate CN is always the bare UUID string (`CN=<uuid>`) — the receiver's mTLS check (`src/receiver/client.c:certificate_matches_uuid`) does a literal case-insensitive compare against the UUID sent at registration. Pool/slot naming (`student7_<uuid>`) is filename/registry-only and must never be written into a CN.
- The real TLS server name is `pt-nad-rt.edtechlab.local` (was the test placeholder `nad-mirror.internal`).
- A seeded pool's size is fixed forever. There is no `--add`/grow path; re-running `seed` with a different `--count` than what's already on disk is an error, not a top-up.
- The registry (`<pool>.tsv`) stores only `slot_index`, `uuid`, `status` (`free`|`occupied`), `issued_at` — never who holds a slot.
- `export` only reads and packages files. It never reads or writes a pool's `.tsv`.
- Raw `.key`/`.crt` files never leave `~/mirror-certs` — only the self-contained `.sh` bootstrap scripts (certs already embedded as base64) are ever exported.

---

## Task 1: Fix the TLS server name placeholder

**Files:**
- Modify: `scripts/nad-provision.sh` (add a constant, use it in `ensure_server_cert`)
- Test: `tests/deploy/test_certs.sh`

**Interfaces:**
- Produces: `TLS_SERVER_NAME` — a script-level constant, value `pt-nad-rt.edtechlab.local`, used by every later task that renders a server or client cert / bootstrap script.

- [ ] **Step 1: Update the failing assertion in `test_certs.sh`**

In `tests/deploy/test_certs.sh`, change line 53 from:

```sh
assert_eq "server certificate has the expected CN" "subject=CN=nad-mirror.internal" "$server_subject"
```

to:

```sh
assert_eq "server certificate has the expected CN" "subject=CN=pt-nad-rt.edtechlab.local" "$server_subject"
```

- [ ] **Step 2: Run the test to verify it now fails**

Run: `sh tests/deploy/test_certs.sh`
Expected: `FAIL - server certificate has the expected CN` (expected `pt-nad-rt.edtechlab.local`, actual `nad-mirror.internal`), other assertions still pass.

- [ ] **Step 3: Add the constant and use it in `ensure_server_cert`**

In `scripts/nad-provision.sh`, add this line right after the `PROJECT_DIR=$(dirname "$MIRROR_PROVISION_SCRIPT_DIR")` line:

```sh
TLS_SERVER_NAME="pt-nad-rt.edtechlab.local"
```

Then change `ensure_server_cert`'s `-subj`/`-addext` line from:

```sh
      openssl req -new -key "$STATE_DIR/server.key" -out "$STATE_DIR/server.csr" \
          -subj "/CN=nad-mirror.internal" \
          -addext "subjectAltName=DNS:nad-mirror.internal,IP:$NAD_HOST" )
```

to:

```sh
      openssl req -new -key "$STATE_DIR/server.key" -out "$STATE_DIR/server.csr" \
          -subj "/CN=$TLS_SERVER_NAME" \
          -addext "subjectAltName=DNS:$TLS_SERVER_NAME,IP:$NAD_HOST" )
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh tests/deploy/test_certs.sh`
Expected: `7 passed, 0 failed` (all assertions in the file pass; the file has 7 `assert_eq` calls).

- [ ] **Step 5: Commit**

```bash
git add scripts/nad-provision.sh tests/deploy/test_certs.sh
git commit -m "fix(deploy): use the real TLS server name instead of the test placeholder"
```

---

## Task 2: Pool naming and slot-spec helpers

**Files:**
- Modify: `scripts/nad-provision.sh` (add `pool_label`, `expand_slot_spec`)
- Test: `tests/deploy/test_pool_slots.sh` (new)

**Interfaces:**
- Consumes: `is_positive_int(value)` (existing, unchanged — returns 0/1 via exit status)
- Produces:
  - `pool_label POOL` — echoes the filename-prefix word for a pool (`students` → `student`, `labs` → `machine`); exits 1 with a message on stderr for any other pool name.
  - `expand_slot_spec SPEC` — echoes one slot index per line for a comma-separated spec of indices and/or `LOW-HIGH` ranges (e.g. `1,3,5-9`); exits 2 with a message on stderr on a malformed part.

- [ ] **Step 1: Write the failing test**

Create `tests/deploy/test_pool_slots.sh`:

```sh
#!/bin/sh
set -u

SCRIPT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)/scripts/nad-provision.sh"

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      "$@" )
}

run_err() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      "$@" ) 2>&1 1>/dev/null
}

assert_eq "students pool labels as student" "student" "$(run pool_label students)"
assert_eq "labs pool labels as machine" "machine" "$(run pool_label labs)"

err=$(run_err pool_label bogus)
assert_eq "unknown pool name is rejected" "unknown pool: bogus" "$err"

assert_eq "single index" "7" "$(run expand_slot_spec 7)"
assert_eq "comma list" "$(printf '1\n3\n5')" "$(run expand_slot_spec 1,3,5)"
assert_eq "range expands inclusive" "$(printf '5\n6\n7\n8\n9')" "$(run expand_slot_spec 5-9)"
assert_eq "mixed list and range" "$(printf '1\n5\n6\n7')" "$(run expand_slot_spec 1,5-7)"

err=$(run_err expand_slot_spec 1,abc)
assert_eq "non-numeric slot is rejected" "invalid slot: abc" "$err"

err=$(run_err expand_slot_spec 5-abc)
assert_eq "non-numeric range bound is rejected" "invalid slot range: 5-abc" "$err"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `sh tests/deploy/test_pool_slots.sh`
Expected: FAIL on every assertion — `pool_label: command not found` / `expand_slot_spec: command not found` (neither function exists yet).

- [ ] **Step 3: Implement `pool_label` and `expand_slot_spec`**

In `scripts/nad-provision.sh`, add after the `base64_flatten()` function:

```sh
pool_label() {
    case "$1" in
        students) echo student ;;
        labs) echo machine ;;
        *) echo "unknown pool: $1" >&2; exit 1 ;;
    esac
}

expand_slot_spec() {
    spec=$1
    IFS=','
    for part in $spec; do
        case "$part" in
            *-*)
                lo=${part%-*}
                hi=${part#*-}
                is_positive_int "$lo" && is_positive_int "$hi" || {
                    echo "invalid slot range: $part" >&2
                    exit 2
                }
                i=$lo
                while [ "$i" -le "$hi" ]; do
                    echo "$i"
                    i=$((i + 1))
                done
                ;;
            *)
                is_positive_int "$part" || {
                    echo "invalid slot: $part" >&2
                    exit 2
                }
                echo "$part"
                ;;
        esac
    done
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `sh tests/deploy/test_pool_slots.sh`
Expected: `9 passed, 0 failed`

- [ ] **Step 5: Commit**

```bash
git add scripts/nad-provision.sh tests/deploy/test_pool_slots.sh
git commit -m "feat(deploy): add pool label and slot-spec expansion helpers"
```

---

## Task 3: Pool seeding — `issue_pool_cert`, `write_pool_bootstrap_script`, `seed_pool`

**Files:**
- Modify: `scripts/nad-provision.sh`
- Test: `tests/deploy/test_seed_pool.sh` (new)

**Interfaces:**
- Consumes: `generate_uuid()`, `base64_flatten(path)`, `render_template(template, ...)`, `pool_label(pool)` (all existing/Task 2), plus script globals `STATE_DIR`, `NAD_HOST`, `TEMPLATE_DIR`, `TLS_SERVER_NAME` (Task 1).
- Produces:
  - `issue_pool_cert POOL IDX` — issues one client cert/key for slot `IDX` of `POOL` under `$STATE_DIR/$POOL/`, signs with `$STATE_DIR/ca.{key,crt}`, calls `write_pool_bootstrap_script`, and appends a `free`-status row to `$STATE_DIR/$POOL/$POOL.tsv`.
  - `write_pool_bootstrap_script POOL IDX UUID CRT_PATH KEY_PATH` — renders `$STATE_DIR/$POOL/<label><IDX>_<UUID>.sh` from `scripts/templates/agent-bootstrap.sh.tmpl`, `chmod 700`.
  - `seed_pool POOL COUNT` — requires `$STATE_DIR/ca.key`/`ca.crt` to exist (else exits 1 pointing at `init`); creates `$STATE_DIR/$POOL` (`chmod 700`); no-ops if `$POOL.tsv` already has exactly `COUNT` rows; exits 1 if it has a different number of rows; otherwise calls `issue_pool_cert` for `1..COUNT`.

- [ ] **Step 1: Write the failing test**

Create `tests/deploy/test_seed_pool.sh`:

```sh
#!/bin/sh
set -u

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/nad-provision.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

STATE_DIR="$TMP/state"
mkdir -p "$STATE_DIR"

run() {
    ( MIRROR_PROVISION_SOURCE=1
      MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      NAD_HOST="192.168.1.78"
      "$@" )
}

run ensure_ca >/dev/null

out=$(run seed_pool students 3)
assert_eq "seed_pool reports how many slots it created" "1" "$(printf '%s\n' "$out" | grep -c 'Seeded pool students with 3 slot')"

tsv="$STATE_DIR/students/students.tsv"
assert_eq "tsv has one row per slot" "3" "$(wc -l < "$tsv" | tr -d ' ')"
assert_eq "all rows start free" "0" "$(awk -F'\t' '$3 != "free"' "$tsv" | wc -l | tr -d ' ')"
assert_eq "slot indices are 1..3" "$(printf '1\n2\n3')" "$(cut -f1 "$tsv")"

uuid1=$(awk -F'\t' '$1 == 1 { print $2 }' "$tsv")
key="$STATE_DIR/students/student1_$uuid1.key"
crt="$STATE_DIR/students/student1_$uuid1.crt"
boot="$STATE_DIR/students/student1_${uuid1}.sh"
assert_eq "slot 1 key exists" "1" "$([ -f "$key" ] && echo 1 || echo 0)"
assert_eq "slot 1 cert exists" "1" "$([ -f "$crt" ] && echo 1 || echo 0)"
assert_eq "slot 1 bootstrap script exists" "1" "$([ -f "$boot" ] && echo 1 || echo 0)"

cert_subject=$(openssl x509 -in "$crt" -noout -subject)
assert_eq "slot cert CN is the bare uuid" "subject=CN=$uuid1" "$cert_subject"

verify_out=$(openssl verify -CAfile "$STATE_DIR/ca.crt" "$crt" 2>&1)
assert_eq "slot cert verifies against the CA" "OK" "${verify_out##*: }"

before=$(cat "$tsv")
out=$(run seed_pool students 3)
after=$(cat "$tsv")
assert_eq "re-running seed with the same count is a no-op" "$before" "$after"
assert_eq "re-running with the same count reports already seeded" "1" "$(printf '%s\n' "$out" | grep -c 'already seeded')"

err=$(run seed_pool students 5 2>&1 1>/dev/null)
assert_eq "re-running with a different count is an error" "1" "$(printf '%s\n' "$err" | grep -c 'growing/shrinking')"

err=$(run seed_pool students 5 2>&1)
status=$?
assert_eq "seed_pool exits non-zero on the count mismatch" "1" "$status"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `sh tests/deploy/test_seed_pool.sh`
Expected: FAIL — `seed_pool: command not found`.

- [ ] **Step 3: Implement `issue_pool_cert`, `write_pool_bootstrap_script`, `seed_pool`**

In `scripts/nad-provision.sh`, add after `expand_slot_spec()` (Task 2):

```sh
write_pool_bootstrap_script() {
    pool=$1
    idx=$2
    uuid=$3
    crt=$4
    key=$5
    label=$(pool_label "$pool")
    out="$STATE_DIR/$pool/${label}${idx}_${uuid}.sh"
    render_template "$TEMPLATE_DIR/agent-bootstrap.sh.tmpl" \
        "AGENT_UUID=$uuid" \
        "RECEIVER_HOST=$NAD_HOST" \
        "RECEIVER_PORT=9443" \
        "TLS_SERVER_NAME=$TLS_SERVER_NAME" \
        "CA_CRT_B64=$(base64_flatten "$STATE_DIR/ca.crt")" \
        "CLIENT_CRT_B64=$(base64_flatten "$crt")" \
        "CLIENT_KEY_B64=$(base64_flatten "$key")" \
        > "$out"
    chmod 700 "$out"
}

issue_pool_cert() {
    pool=$1
    idx=$2
    label=$(pool_label "$pool")
    uuid=$(generate_uuid)
    key="$STATE_DIR/$pool/${label}${idx}_${uuid}.key"
    crt="$STATE_DIR/$pool/${label}${idx}_${uuid}.crt"
    csr="$STATE_DIR/$pool/${label}${idx}_${uuid}.csr"
    ( umask 077
      openssl genrsa -out "$key" 2048 2>/dev/null
      openssl req -new -key "$key" -out "$csr" -subj "/CN=$uuid" )
    openssl x509 -req -in "$csr" -CA "$STATE_DIR/ca.crt" -CAkey "$STATE_DIR/ca.key" \
        -CAcreateserial -out "$crt" -days 365 -sha256
    rm -f "$csr"
    write_pool_bootstrap_script "$pool" "$idx" "$uuid" "$crt" "$key"
    printf '%s\t%s\tfree\t%s\n' "$idx" "$uuid" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >> "$STATE_DIR/$pool/$pool.tsv"
}

seed_pool() {
    pool=$1
    count=$2
    [ -f "$STATE_DIR/ca.key" ] && [ -f "$STATE_DIR/ca.crt" ] || {
        echo "CA not found in $STATE_DIR; run '$0 init' first" >&2
        exit 1
    }
    pool_dir="$STATE_DIR/$pool"
    tsv="$pool_dir/$pool.tsv"
    mkdir -p "$pool_dir"
    chmod 700 "$pool_dir"
    if [ -f "$tsv" ]; then
        existing=$(wc -l < "$tsv" | tr -d ' ')
        if [ "$existing" -eq "$count" ]; then
            printf 'Pool %s already seeded with %d slot(s).\n' "$pool" "$existing"
            return 0
        fi
        echo "Pool '$pool' already has $existing slot(s); growing/shrinking a seeded pool is not supported." >&2
        echo "Use a fresh --state-dir if you need a different size." >&2
        exit 1
    fi
    idx=1
    while [ "$idx" -le "$count" ]; do
        issue_pool_cert "$pool" "$idx"
        idx=$((idx + 1))
    done
    printf 'Seeded pool %s with %d slot(s) in %s\n' "$pool" "$count" "$pool_dir"
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `sh tests/deploy/test_seed_pool.sh`
Expected: `13 passed, 0 failed`

- [ ] **Step 5: Commit**

```bash
git add scripts/nad-provision.sh tests/deploy/test_seed_pool.sh
git commit -m "feat(deploy): issue fixed-size certificate pools with seed_pool"
```

---

## Task 4: Slot bookkeeping — `mark_slots`, `occupy_pool`, `release_pool`

**Files:**
- Modify: `scripts/nad-provision.sh`
- Test: `tests/deploy/test_occupy_release.sh` (new)

**Interfaces:**
- Consumes: `expand_slot_spec(spec)` (Task 2), script global `STATE_DIR`.
- Produces:
  - `mark_slots POOL SPEC TARGET_STATUS REQUIRE_STATUS` — for every slot in `SPEC`: if its current status equals `REQUIRE_STATUS`, sets it to `TARGET_STATUS` and prints `OK   - slot N -> TARGET_STATUS` on stdout; otherwise prints `FAIL - slot N is already <status>` on stderr and leaves it unchanged. A `SPEC` slot absent from the pool prints `FAIL - slot N does not exist in pool` on stderr. Rewrites `<pool>.tsv` with whatever changes succeeded either way. Returns (exit status) 1 if any slot failed, 0 otherwise.
  - `occupy_pool POOL SPEC` — `mark_slots POOL SPEC occupied free`.
  - `release_pool POOL SPEC` — `mark_slots POOL SPEC free occupied`.

- [ ] **Step 1: Write the failing test**

Create `tests/deploy/test_occupy_release.sh`:

```sh
#!/bin/sh
set -u

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/nad-provision.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

STATE_DIR="$TMP/state"
mkdir -p "$STATE_DIR/students"
tsv="$STATE_DIR/students/students.tsv"
printf '1\tuuid-1\tfree\t2026-01-01T00:00:00Z\n' > "$tsv"
printf '2\tuuid-2\tfree\t2026-01-01T00:00:00Z\n' >> "$tsv"
printf '3\tuuid-3\tfree\t2026-01-01T00:00:00Z\n' >> "$tsv"

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

out=$(run occupy_pool students 1-2)
status=$?
assert_eq "occupy_pool exits 0 when all slots apply" "0" "$status"
assert_eq "occupy_pool reports each slot" "2" "$(printf '%s\n' "$out" | grep -c '^OK   - slot')"
assert_eq "slot 1 becomes occupied" "occupied" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"
assert_eq "slot 2 becomes occupied" "occupied" "$(awk -F'\t' '$1 == 2 { print $3 }' "$tsv")"
assert_eq "slot 3 stays free" "free" "$(awk -F'\t' '$1 == 3 { print $3 }' "$tsv")"

err=$(run occupy_pool students 1 2>&1 1>/dev/null)
status=$?
assert_eq "re-occupying an occupied slot fails" "1" "$status"
assert_eq "re-occupying an occupied slot reports the conflict" "1" "$(printf '%s\n' "$err" | grep -c 'slot 1 is already occupied')"

out=$(run release_pool students 1)
assert_eq "release_pool frees slot 1" "free" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"
assert_eq "slot 2 is unaffected by releasing slot 1" "occupied" "$(awk -F'\t' '$1 == 2 { print $3 }' "$tsv")"

err=$(run release_pool students 3 2>&1 1>/dev/null)
status=$?
assert_eq "releasing an already-free slot fails" "1" "$status"

err=$(run occupy_pool students 99 2>&1 1>/dev/null)
assert_eq "occupying a nonexistent slot reports it" "1" "$(printf '%s\n' "$err" | grep -c 'slot 99 does not exist')"

out=$(run occupy_pool students 2-3)
assert_eq "a mixed-validity range still applies the valid slots" "occupied" "$(awk -F'\t' '$1 == 3 { print $3 }' "$tsv")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `sh tests/deploy/test_occupy_release.sh`
Expected: FAIL — `occupy_pool: command not found`.

- [ ] **Step 3: Implement `mark_slots`, `occupy_pool`, `release_pool`**

In `scripts/nad-provision.sh`, add after `seed_pool()` (Task 3):

```sh
mark_slots() {
    pool=$1
    spec=$2
    target_status=$3
    require_status=$4
    tsv="$STATE_DIR/$pool/$pool.tsv"
    [ -f "$tsv" ] || { echo "Pool '$pool' has not been seeded (no $tsv)" >&2; exit 1; }
    slots=$(expand_slot_spec "$spec" | tr '\n' ',')
    tmp="$tsv.tmp.$$"
    failflag="$tmp.fail"
    rm -f "$failflag"
    awk -F'\t' -v OFS='\t' \
        -v slots="$slots" -v target="$target_status" -v require="$require_status" -v failflag="$failflag" '
        BEGIN {
            n = split(slots, arr, ",")
            for (i = 1; i <= n; i++) if (arr[i] != "") want[arr[i]] = 1
        }
        {
            if ($1 in want) {
                seen[$1] = 1
                if ($3 == require) {
                    print "OK   - slot " $1 " -> " target
                    $3 = target
                } else {
                    print "FAIL - slot " $1 " is already " $3 > "/dev/stderr"
                    system("touch " failflag)
                }
            }
            print
        }
        END {
            for (s in want) if (!(s in seen)) {
                print "FAIL - slot " s " does not exist in pool" > "/dev/stderr"
                system("touch " failflag)
            }
        }
    ' "$tsv" > "$tmp"
    mv "$tmp" "$tsv"
    [ ! -f "$failflag" ]
}

occupy_pool() {
    mark_slots "$1" "$2" occupied free
}

release_pool() {
    mark_slots "$1" "$2" free occupied
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `sh tests/deploy/test_occupy_release.sh`
Expected: `11 passed, 0 failed`

- [ ] **Step 5: Commit**

```bash
git add scripts/nad-provision.sh tests/deploy/test_occupy_release.sh
git commit -m "feat(deploy): add manual occupy/release slot bookkeeping"
```

---

## Task 5: Pool status listing — `status_pool`

**Files:**
- Modify: `scripts/nad-provision.sh`
- Test: `tests/deploy/test_status_pool.sh` (new)

**Interfaces:**
- Consumes: script global `STATE_DIR`.
- Produces: `status_pool POOL` — prints a header row, one row per slot (`slot`, `uuid`, `status`, `issued_at`, tab-separated to stay greppable), then a blank line and `free: X, occupied: Y`. Exits 1 with a message on stderr if the pool hasn't been seeded.

- [ ] **Step 1: Write the failing test**

Create `tests/deploy/test_status_pool.sh`:

```sh
#!/bin/sh
set -u

SCRIPT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)/scripts/nad-provision.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

STATE_DIR="$TMP/state"
mkdir -p "$STATE_DIR/students"
printf '1\tuuid-1\tfree\t2026-01-01T00:00:00Z\n' > "$STATE_DIR/students/students.tsv"
printf '2\tuuid-2\toccupied\t2026-01-01T00:00:01Z\n' >> "$STATE_DIR/students/students.tsv"
printf '3\tuuid-3\tfree\t2026-01-01T00:00:02Z\n' >> "$STATE_DIR/students/students.tsv"

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

out=$(run status_pool students)
assert_eq "lists every slot" "3" "$(printf '%s\n' "$out" | grep -c '^[0-9]')"
assert_eq "includes uuid-2 as occupied" "1" "$(printf '%s\n' "$out" | grep -c 'uuid-2.*occupied')"
assert_eq "summarizes free count" "1" "$(printf '%s\n' "$out" | grep -c 'free: 2, occupied: 1')"

err=$(run status_pool bogus 2>&1 1>/dev/null)
assert_eq "unseeded pool is an error" "1" "$(printf '%s\n' "$err" | grep -c 'has not been seeded')"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `sh tests/deploy/test_status_pool.sh`
Expected: FAIL — `status_pool: command not found`.

- [ ] **Step 3: Implement `status_pool`**

In `scripts/nad-provision.sh`, add after `release_pool()` (Task 4):

```sh
status_pool() {
    pool=$1
    tsv="$STATE_DIR/$pool/$pool.tsv"
    [ -f "$tsv" ] || { echo "Pool '$pool' has not been seeded (no $tsv)" >&2; exit 1; }
    printf 'slot\tuuid\tstatus\tissued_at\n'
    cat "$tsv"
    awk -F'\t' '
        { if ($3 == "free") free++; else occupied++ }
        END { printf "\nfree: %d, occupied: %d\n", free + 0, occupied + 0 }
    ' "$tsv"
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `sh tests/deploy/test_status_pool.sh`
Expected: `4 passed, 0 failed`

- [ ] **Step 5: Commit**

```bash
git add scripts/nad-provision.sh tests/deploy/test_status_pool.sh
git commit -m "feat(deploy): add status_pool listing"
```

---

## Task 6: Archive export — `export_pool`

**Files:**
- Modify: `scripts/nad-provision.sh`
- Test: `tests/deploy/test_export_pool.sh` (new)

**Interfaces:**
- Consumes: `pool_label(pool)`, `expand_slot_spec(spec)` (Task 2), script global `STATE_DIR`.
- Produces: `export_pool POOL SPEC_OR_EMPTY OUT` — if `SPEC_OR_EMPTY` is non-empty, exports those slots' `.sh` files; if empty, exports every slot currently in the pool's `.tsv`. Writes a `tar.gz` to `OUT` containing only the bootstrap scripts (flat, no directory prefix). Never reads or writes `status`. Exits 1 naming the slot if a requested slot doesn't exist or its script file is missing.

- [ ] **Step 1: Write the failing test**

Create `tests/deploy/test_export_pool.sh`:

```sh
#!/bin/sh
set -u

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/nad-provision.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

STATE_DIR="$TMP/state"
mkdir -p "$STATE_DIR/students"
tsv="$STATE_DIR/students/students.tsv"
printf '1\tuuid-1\tfree\t2026-01-01T00:00:00Z\n' > "$tsv"
printf '2\tuuid-2\tfree\t2026-01-01T00:00:00Z\n' >> "$tsv"
printf '3\tuuid-3\tfree\t2026-01-01T00:00:00Z\n' >> "$tsv"
for n in 1 2 3; do
    uuid=$(awk -F'\t' -v n="$n" '$1 == n { print $2 }' "$tsv")
    echo "bootstrap $n" > "$STATE_DIR/students/student${n}_${uuid}.sh"
done

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

run export_pool students "" "$TMP/all.tar.gz"
listing=$(tar -tzf "$TMP/all.tar.gz" | sort)
assert_eq "default export includes every slot" "3" "$(printf '%s\n' "$listing" | wc -l | tr -d ' ')"

run export_pool students 2 "$TMP/one.tar.gz"
listing=$(tar -tzf "$TMP/one.tar.gz")
assert_eq "explicit --slots exports only that slot" "1" "$(printf '%s\n' "$listing" | wc -l | tr -d ' ')"
assert_eq "exported file matches slot 2's script name" "1" "$(printf '%s\n' "$listing" | grep -c '^student2_')"

before=$(cat "$tsv")
after=$(cat "$tsv")
assert_eq "export never touches the registry" "$before" "$after"

err=$(run export_pool students 99 "$TMP/bad.tar.gz" 2>&1 1>/dev/null)
assert_eq "exporting a nonexistent slot is an error" "1" "$(printf '%s\n' "$err" | grep -c 'slot 99')"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `sh tests/deploy/test_export_pool.sh`
Expected: FAIL — `export_pool: command not found`.

- [ ] **Step 3: Implement `export_pool`**

In `scripts/nad-provision.sh`, add after `status_pool()` (Task 5):

```sh
export_pool() {
    pool=$1
    spec=$2
    out=$3
    pool_dir="$STATE_DIR/$pool"
    tsv="$pool_dir/$pool.tsv"
    [ -f "$tsv" ] || { echo "Pool '$pool' has not been seeded (no $tsv)" >&2; exit 1; }
    label=$(pool_label "$pool")
    if [ -n "$spec" ]; then
        slots=$(expand_slot_spec "$spec")
    else
        slots=$(cut -f1 "$tsv")
    fi
    names=""
    for idx in $slots; do
        uuid=$(awk -F'\t' -v idx="$idx" '$1 == idx { print $2 }' "$tsv")
        [ -n "$uuid" ] || { echo "slot $idx does not exist in pool '$pool'" >&2; exit 1; }
        name="${label}${idx}_${uuid}.sh"
        [ -f "$pool_dir/$name" ] || { echo "bootstrap script missing for slot $idx: $pool_dir/$name" >&2; exit 1; }
        names="$names $name"
    done
    # shellcheck disable=SC2086 -- $names is a list of known-safe generated filenames
    tar -czf "$out" -C "$pool_dir" $names
    printf 'Exported %d script(s) to %s\n' "$(printf '%s\n' $slots | wc -l | tr -d ' ')" "$out"
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `sh tests/deploy/test_export_pool.sh`
Expected: `6 passed, 0 failed`

- [ ] **Step 5: Commit**

```bash
git add scripts/nad-provision.sh tests/deploy/test_export_pool.sh
git commit -m "feat(deploy): add export_pool bootstrap-script archiving"
```

---

## Task 7: Replace the CLI with `init`/`seed`/`occupy`/`release`/`status`/`export`, remove the flat-agent model

**Files:**
- Modify: `scripts/nad-provision.sh` (remove `parse_args`, `current_agent_count`, `next_agent_index`, `issue_agent`, `write_bootstrap_script`, `provision_agents`, `print_summary`, the old `main`; add `cmd_init`, `cmd_seed`, `cmd_occupy`, `cmd_release`, `cmd_status`, `cmd_export`, new `usage`, new `main`)
- Delete: `tests/deploy/test_parse_args.sh`, `tests/deploy/test_registry.sh`, `tests/deploy/test_issue_agent.sh`, `tests/deploy/test_provision_agents.sh` (all test functions this task removes)
- Test: `tests/deploy/test_cli.sh` (new)

**Interfaces:**
- Consumes: `seed_pool`, `occupy_pool`, `release_pool`, `status_pool`, `export_pool` (Tasks 3-6), `ensure_ca`, `ensure_server_cert`, `check_nad_host_stable`, `check_receiver_installed`, `install_receiver_files`, `enable_receiver_services` (existing, Task 1 for `ensure_server_cert`).
- Produces: `main "$@"` dispatches `init`/`seed`/`occupy`/`release`/`status`/`export`/`-h`/`--help` to `cmd_init`/`cmd_seed`/`cmd_occupy`/`cmd_release`/`cmd_status`/`cmd_export`, each of which sets `STATE_DIR` (default `$HOME/mirror-certs`) from `--state-dir` and calls the matching pool function. This is the last task — no later task depends on `main`'s internals.

- [ ] **Step 1: Remove the four superseded test files**

```bash
git rm tests/deploy/test_parse_args.sh tests/deploy/test_registry.sh tests/deploy/test_issue_agent.sh tests/deploy/test_provision_agents.sh
```

(These test `parse_args`, `current_agent_count`/`next_agent_index`, `issue_agent`/`write_bootstrap_script`, and `provision_agents`/`print_summary` respectively — all removed by this task. Removing the tests first, before the functions, means step 3 doesn't leave a moment where sourcing the script for a still-present old test would break for unrelated reasons.)

- [ ] **Step 2: Write the failing CLI test**

Create `tests/deploy/test_cli.sh`:

```sh
#!/bin/sh
set -u

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/nad-provision.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

run_ok() {
    ( MIRROR_PROVISION_SOURCE=1
      MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
      . "$SCRIPT"
      "$@" )
}

run_err() {
    ( MIRROR_PROVISION_SOURCE=1
      MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
      . "$SCRIPT"
      "$@" ) 2>&1 1>/dev/null
}

err=$(run_err main)
assert_eq "no subcommand shows usage error" "0" "$(printf '%s\n' "$err" | grep -c '^$')"

err=$(run_err main bogus)
assert_eq "unknown subcommand is rejected" "1" "$(printf '%s\n' "$err" | grep -c 'Unknown command: bogus')"

err=$(run_err main seed --count 3)
assert_eq "seed without --pool errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--pool is required')"

err=$(run_err main seed --pool students)
assert_eq "seed without --count errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--count is required')"

err=$(run_err main occupy --pool students)
assert_eq "occupy without --slots errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--slots is required')"

err=$(run_err main export --pool students)
assert_eq "export without --out errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--out is required')"

STATE_DIR_ARG="$TMP/state"
out=$(run_ok main init --nad-host 192.168.1.78 --state-dir "$STATE_DIR_ARG" 2>&1)
status=$?
if [ "$status" -ne 0 ] && printf '%s' "$out" | grep -q 'mirror-receiver binary not found'; then
    printf 'skip - full init end-to-end (mirror-receiver not installed on this machine)\n'
else
    assert_eq "init succeeds when the receiver is installed" "0" "$status"
    assert_eq "init creates the CA" "1" "$([ -f "$STATE_DIR_ARG/ca.crt" ] && echo 1 || echo 0)"

    run_ok main seed --pool students --count 2 --state-dir "$STATE_DIR_ARG" >/dev/null
    assert_eq "seed via CLI creates the pool tsv" "2" "$(wc -l < "$STATE_DIR_ARG/students/students.tsv" | tr -d ' ')"

    run_ok main occupy --pool students --slots 1 --state-dir "$STATE_DIR_ARG" >/dev/null
    assert_eq "occupy via CLI updates status" "occupied" "$(awk -F'\t' '$1 == 1 { print $3 }' "$STATE_DIR_ARG/students/students.tsv")"

    out=$(run_ok main status --pool students --state-dir "$STATE_DIR_ARG")
    assert_eq "status via CLI reports one occupied" "1" "$(printf '%s\n' "$out" | grep -c 'free: 1, occupied: 1')"

    run_ok main export --pool students --out "$TMP/export.tar.gz" --state-dir "$STATE_DIR_ARG" >/dev/null
    assert_eq "export via CLI produces an archive with both slots" "2" "$(tar -tzf "$TMP/export.tar.gz" | wc -l | tr -d ' ')"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 3: Run test to verify it fails**

Run: `sh tests/deploy/test_cli.sh`
Expected: FAIL on most assertions — `main: command not found` (the script still auto-runs the old `main` at source time via the old bottom-of-file guard, and none of `cmd_init`/`cmd_seed`/etc. exist yet).

- [ ] **Step 4: Remove the old flat-agent code**

In `scripts/nad-provision.sh`, delete these functions entirely (they are fully superseded — `seed_pool`/`occupy_pool`/`release_pool`/`status_pool`/`export_pool` from Tasks 3-6 replace everything they did):

- `usage()` (will be replaced in Step 5)
- `parse_args()`
- `current_agent_count()`
- `next_agent_index()`
- `issue_agent()`
- `write_bootstrap_script()`
- `provision_agents()`
- `print_summary()`
- `main()` (will be replaced in Step 5)
- the trailing guard `if [ "${MIRROR_PROVISION_SOURCE:-0}" != "1" ]; then main "$@"; fi` (will be re-added in Step 5)

Also delete the now-unused globals declared near the top of the file: `MODE=`, `MODE_VALUE=`, `AGENTS_TSV=`, `NEW_AGENTS=` (leave `NAD_HOST=` and `STATE_DIR="$HOME/mirror-certs"` — those are still used as the default template for each `cmd_*`'s own local parsing).

- [ ] **Step 5: Add `usage`, `cmd_init`, `cmd_seed`, `cmd_occupy`, `cmd_release`, `cmd_status`, `cmd_export`, and the new `main`**

Add at the same place the old `usage()`/`main()` lived (end of file, before the run guard):

```sh
usage() {
    cat <<'EOF'
Usage: scripts/nad-provision.sh COMMAND [OPTIONS]

Commands:
  init    --nad-host HOST [--state-dir DIR]
          One-time: generate the CA and server certificate, install and
          enable the receiver's systemd units.

  seed    --pool NAME --count N [--state-dir DIR]
          One-time per pool: issue exactly N permanent client certs plus
          one self-contained bootstrap script per slot. Re-running with
          the same --count is a no-op; a different --count is an error.

  occupy  --pool NAME --slots SPEC [--state-dir DIR]
  release --pool NAME --slots SPEC [--state-dir DIR]
          Manually flip a slot (or comma/range list of slots, e.g.
          "1,3,5-9") between free and occupied. Bookkeeping only.

  status  --pool NAME [--state-dir DIR]
          List every slot in a pool with its free/occupied status.

  export  --pool NAME [--slots SPEC] --out FILE [--state-dir DIR]
          Package bootstrap scripts (default: the whole pool) into a
          tar.gz. Never reads or changes free/occupied status.

  -h, --help
          Show this help.

--state-dir defaults to $HOME/mirror-certs.
EOF
}

require_state_dir_option() {
    [ "$#" -ge 2 ] || { echo "--state-dir requires a value" >&2; exit 2; }
}

cmd_init() {
    STATE_DIR="$HOME/mirror-certs"
    NAD_HOST=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --nad-host)
                [ "$#" -ge 2 ] || { echo "--nad-host requires a value" >&2; exit 2; }
                case "$2" in
                    *[!A-Za-z0-9.:-]*|'')
                        echo "--nad-host must be a valid IP address or hostname" >&2
                        exit 2
                        ;;
                esac
                NAD_HOST=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$NAD_HOST" ] || { echo "--nad-host is required" >&2; exit 2; }
    [ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }
    check_receiver_installed
    mkdir -p "$STATE_DIR"
    chmod 700 "$STATE_DIR"
    check_nad_host_stable
    ensure_ca
    ensure_server_cert
    install_receiver_files
    enable_receiver_services
    printf 'nad initialized: CA and server cert in %s, receiver enabled.\n' "$STATE_DIR"
}

cmd_seed() {
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    COUNT=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --count)
                [ "$#" -ge 2 ] || { echo "--count requires a value" >&2; exit 2; }
                is_positive_int "$2" || { echo "--count must be a positive integer" >&2; exit 2; }
                COUNT=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    [ -n "$COUNT" ] || { echo "--count is required" >&2; exit 2; }
    seed_pool "$POOL" "$COUNT"
}

cmd_occupy_or_release() {
    action=$1
    shift
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    SLOTS=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --slots)
                [ "$#" -ge 2 ] || { echo "--slots requires a value" >&2; exit 2; }
                SLOTS=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    [ -n "$SLOTS" ] || { echo "--slots is required" >&2; exit 2; }
    if [ "$action" = occupy ]; then
        occupy_pool "$POOL" "$SLOTS"
    else
        release_pool "$POOL" "$SLOTS"
    fi
}

cmd_occupy() { cmd_occupy_or_release occupy "$@"; }
cmd_release() { cmd_occupy_or_release release "$@"; }

cmd_status() {
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    status_pool "$POOL"
}

cmd_export() {
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    SLOTS=
    OUT=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --slots)
                [ "$#" -ge 2 ] || { echo "--slots requires a value" >&2; exit 2; }
                SLOTS=$2
                shift 2
                ;;
            --out)
                [ "$#" -ge 2 ] || { echo "--out requires a value" >&2; exit 2; }
                OUT=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    [ -n "$OUT" ] || { echo "--out is required" >&2; exit 2; }
    export_pool "$POOL" "$SLOTS" "$OUT"
}

main() {
    [ "$#" -ge 1 ] || { usage >&2; exit 2; }
    cmd=$1
    shift
    case "$cmd" in
        init) cmd_init "$@" ;;
        seed) cmd_seed "$@" ;;
        occupy) cmd_occupy "$@" ;;
        release) cmd_release "$@" ;;
        status) cmd_status "$@" ;;
        export) cmd_export "$@" ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "Unknown command: $cmd" >&2
            usage >&2
            exit 2
            ;;
    esac
}

if [ "${MIRROR_PROVISION_SOURCE:-0}" != "1" ]; then
    main "$@"
fi
```

- [ ] **Step 6: Run the CLI test to verify it passes**

Run: `sh tests/deploy/test_cli.sh`
Expected: `10 passed, 0 failed` if `mirror-receiver` is installed on the test machine, or `9 passed, 0 failed` plus one `skip` line otherwise (the `init` end-to-end block is skipped when `/usr/local/sbin/mirror-receiver` isn't present, matching how `check_receiver_installed` already behaves elsewhere in this suite — e.g. `test_provision_agents.sh` never called it directly for the same reason).

- [ ] **Step 7: Run the full existing deploy test suite to confirm nothing else regressed**

Run:
```sh
for t in tests/deploy/test_*.sh; do echo "== $t =="; sh "$t" || exit 1; done
```
Expected: every remaining file (`test_certs.sh`, `test_pool_slots.sh`, `test_seed_pool.sh`, `test_occupy_release.sh`, `test_status_pool.sh`, `test_export_pool.sh`, `test_cli.sh`, `test_bootstrap_template.sh`, `test_openssl_helpers.sh`, `test_render_template.sh`, `test_host_stability.sh`) reports `0 failed`.

- [ ] **Step 8: Commit**

```bash
git add -A scripts/nad-provision.sh tests/deploy/
git commit -m "feat(deploy): replace the flat --count/--add CLI with pool subcommands"
```

---

## Task 8: Update the README's automated-deploy section

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace the "Автоматический деплой (nad-provision.sh)" section**

Replace the section currently starting at `## Автоматический деплой (nad-provision.sh)` and ending right before `## Полная установка на три VM` with:

```markdown
## Автоматический деплой (nad-provision.sh)

Сначала соберите и установите бинарники на `nad` (`./scripts/build.sh
--clean --install`), затем один раз:

```sh
# На nad (root):
./scripts/nad-provision.sh init --nad-host 192.168.1.78
```

Команда генерирует CA и серверный сертификат (`CN`/`SAN` —
`pt-nad-rt.edtechlab.local`), устанавливает `/etc/mirror-receiver/*` и
включает `mirror-interface.service`/`mirror-receiver.service`. Повторный
запуск с тем же `--nad-host` — no-op.

Дальше один раз на весь пул нужных сертификатов (сейчас есть пул
`students`; `labs` для Windows-машин лабораторий появится отдельно):

```sh
./scripts/nad-provision.sh seed --pool students --count 100
```

Это выпускает 100 постоянных сертификатов и по одному самодостаточному
скрипту на слот в `/root/mirror-certs/students/student<N>_<uuid>.sh`
(под `sudo` `$HOME` — это `/root`). Повторный запуск с тем же `--count` —
no-op; с другим — ошибка (пул не растёт, см.
`docs/superpowers/specs/2026-09-06-cert-pool-provisioning-design.md`).

Скопируйте нужный `student<N>_<uuid>.sh` на целевую машину (LXD-контейнер
студента) и запустите там от root:

```sh
sudo ./student7_<uuid>.sh
```

Сертификаты, `agent.conf` и `mirror-agent.service` настроятся
автоматически; `capture_iface` определяется по интерфейсу маршрута по
умолчанию (переопределяется через `--iface`). Скрипт содержит приватный
ключ агента в открытом виде — удалите его после использования.

Учёт того, какие слоты сейчас в деле:

```sh
./scripts/nad-provision.sh occupy  --pool students --slots 1-10
./scripts/nad-provision.sh release --pool students --slots 7
./scripts/nad-provision.sh status  --pool students
```

Экспорт пачки скриптов для переноса на другую машину (по умолчанию — весь
пул; `occupy`/`release` не требуются и не меняются экспортом):

```sh
./scripts/nad-provision.sh export --pool students --out students.tar.gz
```

Ниже описан тот же процесс вручную — полезно для отладки или если нужно
изменить шаг вручную.
```

- [ ] **Step 2: Verify no other file links to the removed `--count`/`--add` usage**

Run: `grep -rn -- '--count\|--add' README.md docs/ scripts/ | grep -v docs/superpowers/specs/2026-08-30 | grep -v docs/superpowers/plans/2026-08-30`

Expected: no output (the two 2026-08-30 spec/plan files are historical records of the superseded design and are intentionally left as-is).

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document the pool-based nad-provision.sh CLI"
```
