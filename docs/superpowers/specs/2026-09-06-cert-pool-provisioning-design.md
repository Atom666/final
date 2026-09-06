# Certificate Pool Provisioning (students pool) — Design

Date: 2026-09-06
Status: Approved for implementation planning

## Problem

`scripts/nad-provision.sh` (from `docs/superpowers/specs/2026-08-30-agent-deployment-automation-design.md`)
was built around a growing, undifferentiated list of agents: `--count N` /
`--add K` provision agents one at a time as new VMs come online, and
`agents.tsv` is append-only.

The actual deployment model turned out different:

- `nad` is a single, static, long-lived host (`pt-nad-rt.edtechlab.local`,
  `10.255.254.70`). Its CA and server certificate are generated exactly once
  and never regenerated.
- Client certificates are generated **once, up front, in bulk** (100 for
  students, matching headroom for future course runs — a pilot only needs
  30), then the whole batch is exported as a single archive and copied to
  wherever the agents actually run (LXD hosts for students; Windows lab
  machines, a separate pool, later). There is no scenario where agents are
  added to the pool incrementally over the system's lifetime.
- Certificates get reused across course runs: a slot handed to a student in
  one course is freed after the course ends and handed to a different
  student in the next one. The operator needs to see, at a glance, which of
  the pre-generated certificates are currently in use.
- The two populations (student containers vs. lab Windows machines) are
  operationally different enough that mixing them in one flat list makes
  bulk transfer ("copy everything needed for the LXD host in one shot")
  awkward. They need folder-level separation. This design covers the
  `students` pool only; `labs` (Windows) is a separate follow-up blocked on
  merging Windows agent support into this repository (see "Non-goals").

This design replaces the `--count`/`--add`/flat-`agents.tsv` mechanism with
a fixed-size, reusable **pool** model, and fixes the TLS server name, which
is still the placeholder used during initial development.

## Goal

- `nad-provision.sh init` — one-time: CA, server certificate (correct
  hostname), receiver config/units. Idempotent, safe to re-run.
- `nad-provision.sh seed --pool students --count 100` — one-time per pool:
  issues exactly `N` client certificates, each permanent for the life of
  the state directory, each with its own self-contained bootstrap script.
- `nad-provision.sh occupy` / `release` — manual, independent bookkeeping of
  which slots are currently handed out, so slots can be identified for
  reuse.
- `nad-provision.sh status` — see the free/occupied table for a pool.
- `nad-provision.sh export` — package bootstrap scripts for transfer,
  independent of occupied/free state.

## Non-goals

- Growing a pool after it's seeded (`--add` is removed, not replaced). If a
  pool ever needs to be larger, that is a deliberate one-off decision
  (new `--state-dir` or a manual follow-up), not a first-class flag.
- The `labs` (Windows) pool. Blocked on merging Windows agent support
  (`capture_npcap.c`, `platform.c`, `CMakeLists.txt`, `examples/agent-windows.conf`,
  `deploy-windows.ps1`) from the operator's local `Project5`/`winagent`
  copies into this repository — that code does not exist here yet. Once
  merged, `labs` follows the same pool mechanics with a PowerShell
  bootstrap template instead of the shell one.
- Recording *who* holds a slot (student name, course run). The registry
  tracks only `free`/`occupied` per slot, by the operator's own choice.
- Transporting the exported archive (scp/USB/whatever the operator already
  uses — unchanged from the existing design).
- Certificate revocation.
- Automatically reconciling `occupy`/`release` state with what's actually
  running anywhere — this is a manual ledger the operator updates when
  convenient, not a live sync with LXD or the receiver's connection list.

## Components

### State layout

```text
~/mirror-certs/
  nad-host                  # unchanged: the --nad-host value from init
  ca.key  ca.crt            # unchanged: one CA for the life of the state dir
  server.key  server.crt    # unchanged
  students/
    students.tsv            # slot_index \t uuid \t status \t issued_at
    student<N>_<uuid>.key
    student<N>_<uuid>.crt
    student<N>_<uuid>.sh    # self-contained bootstrap, same shape as today's
                             # agent-<N>-bootstrap.sh (certs embedded as base64)
```

`chmod 700` on `students/`, `chmod 600` on `.key` files — unchanged from the
current design. `students.tsv` replaces `agents.tsv`; it is no longer
append-only, since `status` is mutated in place by `occupy`/`release`.

CN of every client certificate stays the bare UUID string
(`CN=<uuid>`) — the receiver's mTLS check
(`src/receiver/client.c:certificate_matches_uuid`) does a literal
case-insensitive compare between the certificate's CN and the UUID string
sent in the agent's registration message. `student<N>_` is a filename/ledger
convenience only; it never appears inside the certificate.

### `nad-provision.sh init --nad-host HOST [--state-dir DIR]`

Same as the current `ensure_ca` / `ensure_server_cert` /
`install_receiver_files` / `enable_receiver_services` / `check_nad_host_stable`
steps, unchanged except:

- `--nad-host` keeps its current meaning and type (the IP address agents
  connect to, e.g. `10.255.254.70` — unchanged from today). What changes is
  the certificate's CN/SAN DNS name: instead of the literal string
  `nad-mirror.internal`, the script uses a new constant,
  `TLS_SERVER_NAME="pt-nad-rt.edtechlab.local"`, defined once near the top
  of `nad-provision.sh` (this is a single static NAD instance — per the
  original design's non-goals, one `--nad-host` per state dir — so a fixed
  constant, not a flag, is enough). `-subj "/CN=$TLS_SERVER_NAME"
  -addext "subjectAltName=DNS:$TLS_SERVER_NAME,IP:$NAD_HOST"`. The same
  constant replaces the hardcoded `TLS_SERVER_NAME=nad-mirror.internal`
  line in `write_bootstrap_script`, matching what `examples/agent.conf`
  already has (`tls_server_name = pt-nad-rt.edtechlab.local`).
- No longer calls `provision_agents` — pool seeding is a separate command.

Idempotent: re-running with the same `--nad-host` is a no-op past the first
run (matches current `ensure_ca`/`ensure_server_cert` behavior).

### `nad-provision.sh seed --pool <name> --count N [--state-dir DIR]`

Requires `init` to have already run (CA/server cert present); otherwise
errors pointing at `init`.

If `<state-dir>/<pool>/<pool>.tsv` already exists:
- Same row count as `--count`: no-op, print "pool already seeded".
- Different row count: error — growing/shrinking a seeded pool isn't
  supported (see Non-goals); the message points at using a fresh
  `--state-dir` instead.

Otherwise, for `idx` in `1..N`: generate UUID, key, CSR, sign with the CA
(same `openssl` sequence as the current `issue_agent`), render
`<pool><idx>_<uuid>.sh` from the existing bootstrap template with the
embedded certs, and append `idx \t uuid \t free \t issued_at` to the pool's
tsv. All rows start `free`.

### `nad-provision.sh occupy --pool <name> --slots SPEC` / `release --pool <name> --slots SPEC`

`SPEC` is a comma-separated list of indices and/or ranges (`1-10`, `1,3,5-9`).
Each slot is processed independently:
- `occupy` on a `free` slot → sets `occupied`, prints `OK`.
- `occupy` on an already-`occupied` slot → prints an error for that slot,
  does not change it.
- `release` mirrors this for `occupied` → `free`.

The tsv is rewritten atomically (temp file + rename, same pattern as
`lxd_lab/credentials.py:save_credentials`) after processing the whole
`SPEC`. Exit code is non-zero if any individual slot was skipped, even
though the valid ones in the same invocation still get applied.

### `nad-provision.sh status --pool <name>`

Prints the pool's tsv as a table (slot, uuid, status, issued_at) plus a
`free: X, occupied: Y` summary line.

### `nad-provision.sh export --pool <name> [--slots SPEC] --out FILE`

Bundles the bootstrap scripts (`.sh` files only — certs are already
embedded in them; raw `.key`/`.crt` never leave `~/mirror-certs`, matching
the existing design's "this directory ... is never transferred as a whole"
rule) for the given slots — all slots in the pool if `--slots` is omitted —
into a `tar.gz` at `FILE`. Does not read or modify `status`; exporting an
already-occupied or already-exported-before slot is fine and has no side
effects. Errors if any requested slot index doesn't exist in the pool.

## Error handling

- All subcommands keep the existing script's `set -eu` style and one-line
  diagnosis on error.
- `seed`/`occupy`/`release`/`status`/`export` all require `--pool`; unknown
  pool name (no matching directory under the state dir) is an error, not a
  silently-created new pool — pools are created only by `seed`.
- `occupy`/`release`/`export` on a slot index outside `1..N` for that pool:
  error naming the invalid index.

## Migration

Nothing to migrate. `~/mirror-certs` currently holds pre-automation manual
test certificates (`agent-a`, `agent-b`, `server`, `ca`, no `agents.tsv`),
and the flat `agents.tsv`/`agents/` scheme from the original design was
never used against real infrastructure. The operator renames the existing
`~/mirror-certs` aside and runs `init` fresh with the corrected hostname.

## Testing

Extends the existing `tests/deploy/test_*.sh` shell-assertion harness
(`test_certs.sh`, `test_openssl_helpers.sh`, `test_registry.sh`,
`test_issue_agent.sh`, `test_provision_agents.sh`, `test_render_template.sh`,
`test_bootstrap_template.sh`, `test_parse_args.sh`) with pool-scoped
equivalents, plus new coverage for `occupy`/`release` (double-occupy,
double-release, range parsing) and `export` (default = whole pool, explicit
`--slots`, missing slot index). Worked out in detail during implementation
planning, not here.
