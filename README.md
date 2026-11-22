# Virtual Switch

A multithreaded software network switch implementation in C (POSIX threads, Linux C11).
Implements Tier 1 (Core items 1 to 5) and Tier 2 (Advanced items 6 to 13).

## Architecture & Layout

- `common.h/.c`: Wire I/O helpers (`read_full`, `write_full`), ref-counted frame buffers, shared-memory statistics, configuration loader, utility helpers.
- `logger.c`: Non-blocking ring-buffer logger thread writing formatted timestamps to `log_file`.
- `queue.h/.c`: 3-priority output queue with Weighted Round-Robin (WRR), anti-starvation age promotion, and token-bucket rate limiting.
- `mac_table.h/.c`: Striped hash table with per-bucket mutexes, LRU eviction, dynamic aging, per-port security limits, table dump, and clear.
- `port.h/.c`: Port lifecycle management, non-blocking RX/TX threads, background reaper thread, token-refill timer thread, VLAN filtering, port mirroring, and forwarding logic.
- `switch.h`: Port and switch control structures, port states, and lock order definitions.
- `switch.c`: Main entry point, connection acceptor thread, bounded worker admission threads, MAC aging thread, signal handler thread, and ordered shutdown.
- `admin.c`: UNIX domain socket administrative server supporting runtime management and introspection.
- `swctl.c`: CLI administrative utility for querying statistics and managing live switch configuration.
- `host.c`: Test host utility with configurable traffic generation, trickle, fragmentation, flood, and sniffer modes.

## Building & Testing

```bash
make        # Builds switch, host, and swctl (-std=c11 -Wall -Wextra -Werror -g -O0)
make clean  # Removes all binaries and build directories
make test   # Executes full automated test suite (tests/t1..t11)
make tsan   # Builds ThreadSanitizer binaries (switch_tsan, host_tsan, swctl_tsan)
make asan   # Builds AddressSanitizer/UBSan binaries (switch_asan, host_asan, swctl_asan)
```

## Running

### Switch
```bash
./switch <tcp_port> <num_ports> [key=value ...] [config=<path>]
```
Example:
```bash
./switch 9000 4 mac_age_sec=300 rate_fps=10 wrr=5:3:1
```

### Administrative Control (`swctl`)
```bash
./swctl [--sock <path>] [--shm <name>] <command> [args]
```
Supported commands:
- `show mac`: Dump learned MAC addresses, associated ports, and VLANs.
- `show ports`: Display current state, VLAN, mirror target, and trunk status for each port.
- `shutdown <port>`: Administratively disable a port (`PORT_ADMIN_DOWN`).
- `up <port>`: Re-enable an administratively or security-disabled port (`PORT_FREE`).
- `vlan <port> <vlan>`: Set access VLAN (1-4094) for a port and flush its learned MAC entries.
- `mirror <port> <sniffer_port>`: Mirror ingress and egress traffic of a port to a sniffer port (-1 to disable).
- `clear mac`: Flush all dynamic entries in the switch MAC address table.
- `stats`: Print per-port ingress, egress, drop, and mirror packet counters from shared memory.
- `stats --watch`: Continuously display and update live statistics every second.

### Host Tool
```bash
./host <switch_ip> <tcp_port> <mac> [options]
```
Host commands (via stdin):
- `send <dst_mac> <prio> <vlan> <text>`: Send unicast frame (priority: 0=High, 1=Normal, 2=Low).
- `bcast <prio> <vlan> <text>`: Send broadcast frame (`ff:ff:ff:ff:ff:ff`).
- `spoof <src_mac> <dst_mac> <prio> <vlan> <text>`: Send frame with custom source MAC.
- `quit`: Cleanly disconnect and terminate.

Host command-line options:
- `--sniffer`: Promiscuous receive mode displaying captured frames prefixed with `SNIFF`.
- `--flood <n>`: Transmit `n` frames back-to-back as fast as possible.
- `--trickle`: Transmit frame 1 byte at a time.
- `--die-mid-frame`: Transmit half a frame then terminate abruptly.
- `--no-read`: Stay connected without draining socket receive buffer.
- `--slow-read <ms>`: Delay reading frames by specified milliseconds.
- `--coalesce`: Send 2 frames coalesced into a single `write()` call.

## Configuration File Format

Configuration files can be loaded via `config=<path>` on the command line or reloaded live via `SIGHUP`.
- Format: `key=value`, one setting per line.
- Comments: Lines starting with `#` or text after `#` are ignored.
- Blank lines and whitespace around keys and values are ignored.

Supported configuration keys:
| Key | Default | Reloaded on SIGHUP | Description |
|---|---|---|---|
| `queue_capacity` | 64 | No | Maximum frames in per-port output queue (>= 1) |
| `accept_wait_ms` | 3000 | No | Admission queue timeout for pending connections (ms) |
| `mac_age_sec` | 300 | Yes | Dynamic MAC entry lifetime in seconds (>= 1) |
| `mac_max_entries` | 1024 | Yes | Maximum total MAC entries in switch table |
| `mac_max_per_port` | 8 | Yes | Port security limit (maximum MACs per port) |
| `rate_fps` | 0 | Yes | Egress rate limit in frames per second (0 = unlimited) |
| `wrr` | `5:3:1` | Yes | Weighted Round Robin weights for High:Normal:Low priorities |
| `prio_age_ms` | 200 | Yes | Anti-starvation promotion threshold (ms) |
| `vlan.<port>` | 1 | Yes | Access VLAN assignment for specified port index (1-4094) |
| `trunk_port` | -1 | Yes | Designate port as 802.1Q VLAN trunk (-1 = none) |
| `log_file` | `switch.log` | No | File path for asynchronous timestamped logging |
| `dump_file` | `mac_dump.txt` | No | Target file path for SIGUSR1 MAC table dump |
| `sock_path` | `/tmp/vswitch.sock` | No | UNIX domain socket path for admin server |
| `shm_name` | `/vswitch_stats` | No | POSIX shared memory object name for statistics |

## POSIX Signals

The switch blocks signals in all worker threads and dedicates a dedicated signal thread to `sigwait()`:
- `SIGINT` / `SIGTERM`: Initiates orderly graceful shutdown: stops acceptor, joins admissions, closes and reaps all ports, stops timer and admin servers, flushes logger, unlinks shared memory and UNIX socket, and releases all memory.
- `SIGHUP`: Dynamically reloads and applies configuration file updates (rate limits, WRR weights, MAC limits, VLANs, trunk port).
- `SIGUSR1`: Atomically dumps the entire MAC forwarding table to `dump_file`.
