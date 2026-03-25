# ft_ping

A reimplementation of the `ping` command from **inetutils 2.0**, written in C using raw ICMP sockets.

---

## Overview

`ft_ping` sends ICMP Echo Request packets to a network destination, waits for ICMP Echo Reply responses, measures the Round-Trip Time (RTT) for each packet, and displays statistics at the end of the session.

The program requires **root privileges** because it uses a raw socket (`SOCK_RAW`) to construct ICMP headers directly — a capability the kernel restricts to superusers.

---

## Usage

```bash
sudo ./ft_ping [options] destination
```

`destination` can be either a hostname (`google.com`) or a direct IP address (`8.8.8.8`).

---

## Options

| Option | Description |
|--------|-------------|
| `-v` | Verbose output — displays ICMP error messages normally ignored (Destination Unreachable, late replies, unknown types) |
| `-q` | Quiet output — displays only the header and final statistics |
| `-c N` | Stop after sending N packets |
| `--ttl N` | Set the Time-To-Live of outgoing packets (1–255). Packets destroyed in transit trigger a "Time to live exceeded" message |
| `-i N` | Wait N seconds between each packet (default: 1.0, accepts decimals e.g. `0.5`) |
| `-w N` | Stop after N seconds regardless of how many packets have been sent |
| `-?` | Display help and exit |

---

## Examples

```bash
# Basic ping
sudo ./ft_ping google.com

# Stop after 5 packets
sudo ./ft_ping -c 5 google.com

# Ping every 500ms
sudo ./ft_ping -i 0.5 google.com

# Stop after 10 seconds
sudo ./ft_ping -w 10 google.com

# Show TTL exceeded (packet destroyed by a router after 3 hops)
sudo ./ft_ping --ttl 3 google.com

# Quiet mode — statistics only
sudo ./ft_ping -q -c 10 google.com

# Verbose mode — show ICMP error messages
sudo ./ft_ping -v google.com

# Combine options
sudo ./ft_ping -v -c 5 -i 0.5 --ttl 10 google.com
```

---

## Output format

```
PING google.com (142.251.39.110): 56 data bytes
64 bytes from 142.251.39.110: icmp_seq=0 ttl=117 time=12.453 ms
64 bytes from 142.251.39.110: icmp_seq=1 ttl=117 time=11.821 ms
64 bytes from 142.251.39.110: icmp_seq=2 ttl=117 time=13.102 ms
^C--- google.com ping statistics ---
3 packets transmitted, 3 packets received, 0% packet loss
round-trip min/avg/max/stddev = 11.821/12.459/13.102/0.527 ms
```

With `--ttl` set low enough to expire in transit:

```
PING google.com (142.251.39.110): 56 data bytes
64 bytes from 10.60.0.1: Time to live exceeded
64 bytes from 10.60.0.1: Time to live exceeded
^C--- google.com ping statistics ---
2 packets transmitted, 0 packets received, 100% packet loss
```

---

## Return value

| Value | Meaning |
|-------|---------|
| `0` | At least one packet was received — destination is reachable |
| `1` | No packet was received — destination is unreachable |

This makes `ft_ping` usable in shell scripts:

```bash
if sudo ./ft_ping -c 1 -q google.com; then
    echo "Network OK"
else
    echo "No connection"
fi
```

---

## Implementation

### Key concepts

**Raw socket** — `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` gives direct access to the network layer. The program constructs the ICMP header manually; the kernel adds the IP header automatically.

**Embedded timestamp** — the send time (`gettimeofday`) is stored inside the ICMP packet data. When the Echo Reply arrives, the timestamp is extracted and subtracted from the receive time to compute the RTT — no state is needed on the sender side.

**Absolute deadline** — each iteration uses a fixed deadline (`iter_start + interval`). This deadline is shared between `ping_loop`, `receive_ping`, and `wait_for_packet`. Regardless of how many stray packets arrive, the full send + receive + sleep cycle always takes exactly `interval` seconds.

**RFC 1071 checksum** — computed at send time only. On reception, the kernel already verifies the checksum before delivering the packet to `recvfrom` — any corrupted packet is silently discarded before the program ever sees it.

**Online statistics** — only two accumulators are maintained (`total_time` and `sum_sq`). Mean and standard deviation are computed in O(1) at the end using the König-Huygens identity: `mdev = sqrt(E[X²] - E[X]²)`.

### Function overview

| Function | Role |
|----------|------|
| `main` | Parse options, check root, resolve DNS, open socket, launch loop |
| `ping_loop` | Main loop — cadences sends, manages deadlines and sleep |
| `send_ping` | Build and send one ICMP Echo Request |
| `create_icmp_packet` | Fill the ICMP header, embed timestamp, compute checksum |
| `receive_ping` | Wait for and dispatch incoming packets |
| `wait_for_packet` | Block on `select()` until a packet arrives or deadline expires |
| `parse_packet` | Filter, validate, compute RTT, update statistics, display result |
| `calculate_checksum` | RFC 1071 Internet checksum |
| `resolve_hostname` | DNS resolution — supports both hostnames and raw IPs |
| `time_diff` | Subtract two `struct timeval` values, return milliseconds as `double` |
| `print_statistics` | Display final summary after session ends |
| `sig_handler` | Handle SIGINT (Ctrl+C) — set `ping_loop` flag to 0 |

---

## Build

```bash
make
```

Requires `gcc` and standard POSIX headers. Tested on Linux (Ubuntu 22.04).

---

## Memory

Verified with Valgrind — zero leaks, zero errors:

```
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts
```

No dynamic allocation is used. All buffers are stack-allocated.
