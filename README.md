# Virtual Switch (Phase 1)

A multithreaded network switch implementation in C (POSIX threads, Linux).

## Phase 1 Layout

The Phase 1 implementation keeps the Lab frame and MAC-table files intact and
groups the smaller modules into a few files:

- `common.h/.c`: I/O helpers, frame buffers, atomic stats, and configuration
- `queue.h/.c`: bounded output queue
- `switch.h`: shared switch and port structures
- `port.h/.c`: port lifecycle, receiver/sender threads, reaper, and forwarding
- `switch.c`: main, admission, acceptor, signal, and aging threads

## Building

```bash
make        # Builds switch and host with standard flags (-std=c11 -Wall -Wextra -Werror -g -O0)
make clean  # Removes all binaries and object files
make tsan   # Builds ThreadSanitizer binaries (switch_tsan, host_tsan)
make asan   # Builds AddressSanitizer binaries (switch_asan, host_asan)
```

## Running

### Switch
```bash
./switch <tcp_port> <num_ports> [key=value ...]
```
Example:
```bash
./switch 9000 4 mac_age_sec=300 queue_capacity=64
```

### Host
```bash
./host <switch_ip> <tcp_port> <mac> [options]
```
Example:
```bash
./host 127.0.0.1 9000 aa:bb:cc:dd:ee:01
```

Host commands (via stdin):
- `send <dst_mac> <prio> <vlan> <text>`: Send unicast frame
- `bcast <prio> <vlan> <text>`: Send broadcast frame
- `spoof <src_mac> <dst_mac> <prio> <vlan> <text>`: Send frame with specified source MAC
- `quit`: Disconnect and exit

Host options:
- `--trickle`: Transmit frame 1 byte at a time
- `--die-mid-frame`: Transmit half a frame then terminate
- `--no-read`: Stay connected without reading from socket
- `--slow-read <ms>`: Delay reading frames
- `--coalesce`: Send 2 frames together in one write

## Configuration Keys

Pass as `key=value` command-line arguments:
- `queue_capacity` (default: 64): Max frames in per-port output queue
- `accept_wait_ms` (default: 3000): Milliseconds an extra host waits for a free port before timeout
- `mac_age_sec` (default: 300): MAC entry lifetime in seconds
- `mac_max_entries` (default: 1024): Maximum total MAC entries in switch table (LRU eviction)
- `mac_max_per_port` (default: 8): Port security limit (maximum distinct MACs allowed per port)

## Design Notes & Assumptions

1. **Access Port Leniency**: On access ports, the switch ignores the host's `vlan` field and clamps `priority > 2` to `PRIO_NORMAL` (1). Access ports do not reject frames via `frame_header_validate`.
2. **"SWITCH FULL" Response**: When all switch ports are occupied, an incoming connection waits up to `accept_wait_ms` milliseconds via `sem_timedwait`. If no port becomes free before the deadline expires, the switch replies with the ASCII line `SWITCH FULL\n` and closes the socket.
3. **Semaphore Invariant**: The counting semaphore `free_ports` strictly equals the number of ports in state `PORT_FREE`. Ports in `PORT_SECURITY_DOWN` or `PORT_ADMIN_DOWN` do not post to the semaphore.
4. **Reference Counting**: Broadcast and flooded frames allocate a single `frame_buf_t` with reference counting (`frame_buf_retain` / `frame_buf_release`) across destinations with atomic memory ordering.
