# VM Testing Guide

This guide describes the basic two-VM test and the full three-VM deduplication test:

- `agent-vm`: runs `mirror-agent` and captures one interface.
- `nad-vm`: runs `mirror-receiver`; NAD or `tcpdump` listens on `nad-mirror`.

The examples assume Ubuntu/Debian. Replace package commands as needed for your distro.

## 1. Network Preconditions

Pick the IP address of `nad-vm` reachable from `agent-vm`:

```sh
ip -4 addr
```

In the examples below:

```text
nad-vm IP: 10.0.0.10
agent capture interface: eth0
receiver port: 9443
```

Allow TCP/9443 from `agent-vm` to `nad-vm` in cloud security groups, host firewall, or both.

Check connectivity from `agent-vm`:

```sh
ping -c 3 10.0.0.10
nc -vz 10.0.0.10 9443
```

The `nc` check will fail until the receiver is running, but it is useful later.

## 2. Install Build Dependencies

On both VMs:

```sh
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev libxxhash-dev iproute2 systemd pkg-config tcpdump netcat-openbsd iperf3
```

Build the project on both VMs:

```sh
./scripts/check-deps.sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cmake --parallel "$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

The dependency checker may warn that you are not root. That is fine for build/tests. Runtime capture and injection need privileges.

## 3. Prepare Receiver VM

Create the local veth pair on `nad-vm`:

```sh
sudo ./scripts/mirror-interface-setup.sh
ip link show mirror-rx
ip link show nad-mirror
```

Expected:

```text
mirror-rx: UP, mtu 9216
nad-mirror: UP, mtu 9216
```

For a first manual run, copy the example config:

```sh
sudo mkdir -p /etc/mirror-receiver
sudo cp examples/receiver.conf /etc/mirror-receiver/receiver.conf
```

## 4. Quick Smoke Test Without mTLS

Use this only to validate basic connectivity and packet flow. This mode disables certificate verification.

On `nad-vm`, edit `/etc/mirror-receiver/receiver.conf`:

```ini
server_cert_file = /etc/mirror-receiver/server.crt
server_key_file = /etc/mirror-receiver/server.key
client_ca_file =
require_client_certificate = false
insecure_tls = true
```

Create a temporary self-signed server certificate:

```sh
sudo openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /etc/mirror-receiver/server.key \
  -out /etc/mirror-receiver/server.crt \
  -days 3 \
  -subj "/CN=nad-mirror.internal"
```

Start the receiver:

```sh
sudo build-cmake/bin/mirror-receiver /etc/mirror-receiver/receiver.conf
```

In a second shell on `nad-vm`, capture what NAD would see:

```sh
sudo tcpdump -eni nad-mirror -vv
```

On `agent-vm`, create config:

```sh
sudo mkdir -p /etc/mirror-agent
sudo cp examples/agent.conf /etc/mirror-agent/agent.conf
```

Edit `/etc/mirror-agent/agent.conf`:

```ini
capture_iface = eth0
receiver_host = 10.0.0.10
receiver_port = 9443
ca_file =
client_cert_file =
client_key_file =
tls_server_name =
tls_verify_peer = false
insecure_tls = true
batch_max_records = 256
batch_max_bytes = 262144
batch_linger_us = 200
transport_write_timeout_sec = 10
```

Start the agent:

```sh
sudo build-cmake/bin/mirror-agent /etc/mirror-agent/agent.conf
```

Generate traffic on `agent-vm`:

```sh
ping -c 5 10.0.0.10
curl -I http://example.com
```

Expected result: `tcpdump` on `nad-vm` `nad-mirror` shows Ethernet frames. There must be no GRE or ERSPAN headers.

The agent sends multiple unchanged length-prefixed records in each reusable TLS
batch. After a load test, inspect the latest agent line:

```sh
sudo journalctl -u mirror-agent.service -g 'connection_state=connected' \
  -n 1 --no-pager -o cat
```

`queue_dropped`, `ring_dropped`, and `send_errors` must be zero. Compare
`ingress_pps` with `egress_pps`, and inspect `queue_peak_packets`,
`queue_residence_p95_us`, `batch_avg_records`, `ssl_write_calls`, `cpu_percent`,
and `rss_bytes`. A continuously growing queue means batching is still slower
than capture; a larger queue is not a sustainable fix.

## 5. mTLS Test

Use this after the smoke test works.

On any secure machine, or on `nad-vm` for a lab, create a CA, server cert, and client cert:

```sh
mkdir -p /tmp/mirror-certs
cd /tmp/mirror-certs

openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days 365 \
  -out ca.crt \
  -subj "/CN=mirror-test-ca"

openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr \
  -subj "/CN=nad-mirror.internal"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 365 -sha256

openssl genrsa -out client.key 2048
AGENT_UUID=11111111-2222-3333-4444-555555555555
openssl req -new -key client.key -out client.csr \
  -subj "/CN=${AGENT_UUID}"
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt -days 365 -sha256
```

Copy certs to `nad-vm`:

```sh
sudo mkdir -p /etc/mirror-receiver
sudo cp ca.crt /etc/mirror-receiver/agents-ca.crt
sudo cp server.crt /etc/mirror-receiver/server.crt
sudo cp server.key /etc/mirror-receiver/server.key
sudo chmod 600 /etc/mirror-receiver/server.key
```

Copy certs to `agent-vm`:

```sh
sudo mkdir -p /etc/mirror-agent
sudo cp ca.crt /etc/mirror-agent/ca.crt
sudo cp client.crt /etc/mirror-agent/client.crt
sudo cp client.key /etc/mirror-agent/client.key
sudo chmod 600 /etc/mirror-agent/client.key
```

On `nad-vm`, set:

```ini
require_client_certificate = true
insecure_tls = false
```

On `agent-vm`, set:

```ini
agent_uuid = 11111111-2222-3333-4444-555555555555
receiver_host = 10.0.0.10
tls_server_name = nad-mirror.internal
tls_verify_peer = true
insecure_tls = false
```

Because the certificate CN is `nad-mirror.internal`, either add DNS or use `/etc/hosts` on `agent-vm`:

```sh
echo "10.0.0.10 nad-mirror.internal" | sudo tee -a /etc/hosts
```

Then set:

```ini
receiver_host = nad-mirror.internal
```

Start receiver and agent as before.

## 6. Systemd Test

Install binaries and units:

```sh
sudo cmake --install build-cmake
```

On `nad-vm`:

```sh
sudo cp examples/receiver.conf /etc/mirror-receiver/receiver.conf
sudo systemctl daemon-reload
sudo systemctl enable --now mirror-interface.service
sudo systemctl enable --now mirror-receiver.service
sudo journalctl -u mirror-receiver.service -f
```

On `agent-vm`:

```sh
sudo cp examples/agent.conf /etc/mirror-agent/agent.conf
sudo systemctl daemon-reload
sudo systemctl enable --now mirror-agent.service
sudo journalctl -u mirror-agent.service -f
```

## 7. Functional Checks

### Receiver is listening

On `nad-vm`:

```sh
ss -ltnp | grep 9443
```

### Agent connects

On `nad-vm`:

```sh
sudo journalctl -u mirror-receiver.service -n 100
```

Look for a successful connection and registration.

### Frames arrive at NAD interface

On `nad-vm`:

```sh
sudo tcpdump -eni nad-mirror -c 20
```

On `agent-vm`, generate traffic:

```sh
ping -c 5 1.1.1.1
curl -I http://example.com
```

Expected: `tcpdump` prints L2 frames on `nad-mirror`.

### No ERSPAN/GRE

On `nad-vm`:

```sh
sudo tcpdump -eni nad-mirror 'gre'
```

Expected: no packets. The PoC injects raw Ethernet frames, not GRE/ERSPAN.

### Local MTU

On `nad-vm`:

```sh
ip link show mirror-rx
ip link show nad-mirror
```

Expected MTU: `9216`.

## 8. Three-VM Cross-Agent Deduplication

Use three VMs on the same reachable network:

```text
agent-a: 10.0.0.11, capture eth0, UUID 11111111-1111-1111-1111-111111111111
agent-b: 10.0.0.12, capture eth0, UUID 22222222-2222-2222-2222-222222222222
nad-vm:  10.0.0.10, receiver TCP/9443 and nad-mirror
```

Run the receiver as described above. Configure and start one agent on each endpoint, using the same receiver address but the distinct UUIDs shown above. Keep both VM clocks synchronized with NTP; otherwise configure their measured nanosecond corrections in `[agent_clock_offsets]` on `nad-vm`.

On `nad-vm`, observe only traffic between the endpoint VMs:

```sh
sudo tcpdump -ni nad-mirror 'host 10.0.0.11 and host 10.0.0.12'
```

Generate traffic from `agent-a`:

```sh
ping -c 10 10.0.0.12
iperf3 -c 10.0.0.12 -t 10
```

Run `iperf3 -s` on `agent-b` before the client command. Each network packet is normally captured twice: TX on one endpoint and RX on the other. With dedup enabled, only one member of each A/B observation pair should appear on `nad-mirror`. The periodic receiver log should show `receiver_cross_agent_duplicates_total` increasing.

Temporarily set `enabled = false` under `[dedup]` and restart only the receiver. Repeating the ping should produce both endpoint observations. Re-enable dedup after this comparison.

Transport idempotency is independent of packet dedup. A repeated protocol record with the same UUID, epoch and sequence increases `receiver_transport_duplicates_total`, does not enter the fingerprint cache, and does not reach `nad-mirror`.

## 9. Failure Tests

### Receiver down

Stop receiver:

```sh
sudo systemctl stop mirror-receiver.service
```

Expected agent behavior:

- capture thread keeps running;
- queue fills up to configured limits;
- mirror copies may be dropped;
- workload traffic is not blocked;
- agent reconnects after receiver returns.

Start receiver again:

```sh
sudo systemctl start mirror-receiver.service
```

### Bad client certificate

Replace the agent cert with one not signed by the CA and restart the agent.

Expected:

- receiver rejects TLS handshake;
- receiver process stays alive;
- other agents are not affected.

### Output MTU too small

On `nad-vm`:

```sh
sudo ip link set mirror-rx mtu 1200
sudo ip link set nad-mirror mtu 1200
```

Restart receiver.

Expected:

- receiver refuses to start because `required_mtu = 9216`;
- no silent truncation.

Restore:

```sh
sudo ip link set mirror-rx mtu 9216
sudo ip link set nad-mirror mtu 9216
```

## 10. Known PoC Limits To Remember

- The self-flow socket filter currently covers IPv4 Ethernet with zero or one VLAN tag; IPv6 filtering is a future extension. The packet protocol and receiver restoration support up to four VLAN metadata tags, while current TPACKET capture produces one metadata tag per packet.
- Agent source identity is not encoded into frames delivered to `nad-mirror`.
- With offloads enabled, AF_PACKET sees Linux stack-visible packets, not guaranteed physical wire-identical frames. With `segmentation_mode=receiver`, each supported oversized TCP/IPv4 or TCP/IPv6 aggregate crosses TLS once and the receiver segments it to the source interface MTU before fingerprint/dedup/reorder. Deploy the new receiver before enabling this mode on agents. `segmentation_mode=agent` is the compatibility fallback.
- FCS, preamble and SFD are not available from Linux capture.
- The PoC currently logs stats but does not expose Prometheus metrics.
- Cross-agent deduplication is enabled by default with a 500 ms window. When two endpoint VMs both run agents, one observation of a packet should reach `nad-mirror`; an intentional repeat produces another A/B pair and therefore another output packet.
- Compare `component=receiver-summary`, `component=receiver-pipeline`, and `component=receiver-agent` lines before and after a ping or iperf run. The pipeline line separates admission reject reasons, stage backlog and PPS, time at capacity, batching efficiency, and residence. `peer_rescued_after_reject` counts a rejected observation recovered by the other agent; with exactly two agents, `rejected_observation_pairs` estimates packets whose two endpoint copies were both rejected. Replaying the same protocol record increments `transport_duplicates` without changing the packet-cache counters.
- IPv4 and IPv6 fragments deliberately bypass cross-agent deduplication. Oversized fragmented, UDP, malformed, or unsupported aggregates are counted in `oversized_dropped`; they are not emitted as misleading complete Ethernet frames.
