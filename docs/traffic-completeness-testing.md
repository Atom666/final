# Exact Traffic Completeness Test

This test uses three independent observations:

- `mirror-test-sender` on VM-A creates a known sequence of UDP datagrams.
- `mirror-test-sink` on VM-B verifies what the workload destination receives.
- `mirror-test-verifier` on VM-NAD verifies frames delivered after mirror deduplication.

Each packet contains a run ID, sequence number and payload hash. A successful run proves that the
NAD handoff received every generated packet exactly once; a packet count alone cannot prove this.

## Build And Copy

Build on the oldest target distribution, for example Debian 12:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cmake --parallel "$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

Copy `mirror-test-sender` to VM-A, `mirror-test-sink` to VM-B and
`mirror-test-verifier` to VM-NAD. All three require `libxxhash.so.0`; only verifier requires root.

## Run

Choose a new positive run ID for every test. The example sends 100,000 packets with a 1200-byte UDP
payload at 10 Mbit/s to UDP/55001.

Start VM-B first:

```sh
mirror-test-sink --run-id 1001 --expected 100000 --port 55001 --timeout 180
```

Start VM-NAD second:

```sh
sudo mirror-test-verifier --interface nad-mirror --run-id 1001 \
  --expected 100000 --port 55001 --timeout 180
```

Start VM-A last:

```sh
mirror-test-sender --destination VM_B_IP --run-id 1001 --count 100000 \
  --size 1200 --rate-mbps 10 --port 55001
```

Sender, sink and verifier must all print `result=PASS`. In particular, verifier must report:

```text
expected=100000 received=100000 unique=100000 missing=0 duplicates=0 corrupted=0 out_of_range=0 result=PASS
```

## Control Run

Stop the receiver, set `enabled = false` in `[dedup]`, restart it, and repeat with a new run ID.
The destination sink should still pass. The NAD verifier should report approximately one duplicate
for each unique packet and fail, proving that it can detect the two agent observations.

Re-enable dedup before subsequent tests. Start at 10 Mbit/s, then repeat with 50, 100 and 250 Mbit/s.
Do not increase load until sender, sink and verifier all pass at the current rate. The tool reports
UDP payload throughput; Ethernet and mirror transport consume additional bandwidth.
