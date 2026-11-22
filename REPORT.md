# Virtual Switch Project Report

## 1. Overview
This project is a multithreaded network switch written in C11 for Linux using POSIX threads. It acts as an Ethernet-like switch that connects multiple client processes (hosts) over TCP. The switch dynamically learns MAC addresses, forwards unicast frames to specific ports, floods broadcasts and unknown destinations, enforces VLAN isolation, supports port mirroring, schedules egress traffic using 3-priority Weighted Round Robin (WRR) with anti-starvation aging, bounds transmission with token-bucket rate limiting, handles live admin reconfiguration via a UNIX socket, and shuts down cleanly upon receiving signals.

---

## 2. Thread Architecture

### Thread Hierarchy Diagram

```
                             +-------------------+
                             |    main thread    |
                             +---------+---------+
                                       |
    +-------------+-------------+------+------+-------------+-------------+
    |             |             |             |             |             |
    v             v             v             v             v             v
[signal_thread] [acceptor]   [aging]      [refill]       [admin]       [logger]
                  |
         +--------+--------+
         | (per connection)|
         v                 v
   [admission_0]      [admission_k]
         |
         v (spawns for port i)
   +-----+-----+
   |           |
   v           v
[rx_thread] [tx_thread]
```

### Thread Responsibilities

1. **`main thread`**: Parses command-line configuration, initializes shared memory and synchronization objects, spawns background service threads, waits for the signal thread upon termination, and runs the ordered teardown sequence.
2. **`signal_thread`**: Dedicated thread that calls `sigwait()` to handle `SIGINT`, `SIGTERM` (clean exit), `SIGHUP` (dynamic configuration reload), and `SIGUSR1` (MAC table dump).
3. **`acceptor_thread`**: Listens on the switch TCP port for incoming client sockets. When a host connects, it validates the port counting semaphore (`sem_trywait` / `sem_timedwait`) and dispatches a short-lived admission thread.
4. **`admission_thread`**: Runs per pending connection. Reads the initial handshake/MAC, checks the admission timeout, allocates a free port slot under `slot_lock`, marks the state as active, and spawns the port's RX and TX threads.
5. **`rx_thread` (1 per active port)**: Reads 17-byte frame headers and variable payloads from the host socket. Learns the source MAC in the MAC table, handles VLAN tagging, performs destination lookups, fans out unicast/broadcast frames, and queues mirror copies.
6. **`tx_thread` (1 per active port)**: Dequeues outgoing frames from the port's 3-priority output queue using Weighted Round Robin (WRR) and anti-starvation age promotion. Enforces token-bucket rate limits and writes the frames to the client socket.
7. **`reaper_thread`**: Background thread that joins terminated RX/TX threads when ports disconnect or are shut down, closes socket descriptors, resets queue structures, and returns port tokens to the free counting semaphore.
8. **`aging_thread`**: Wakes periodically (every 1 second) using a condition variable to scan the MAC table buckets and evict dynamic entries whose age exceeds `mac_age_sec`.
9. **`refill_thread`**: Ticks every 100 milliseconds to refill token buckets for all egress ports according to their configured `rate_fps`.
10. **`admin_thread`**: Listens on a UNIX domain socket (`/tmp/vswitch.sock`). Parses commands from `swctl` (`show mac`, `show ports`, `shutdown`, `up`, `vlan`, `mirror`, `clear mac`, `stats`) and executes atomic configuration changes.
11. **`logger_thread`**: Consumes formatted log records from a non-blocking bounded ring buffer and writes them with microsecond timestamps to the log file and stderr.

---

## 3. Synchronization & Locks

### Mutex and Lock Inventory

| Lock | Level | Scope | Data Protected |
|---|---|---|---|
| `cfg_lock` | L1 | Global | Global runtime configuration values (`switch_cfg_t`: rates, VLAN mappings, limits). |
| `slot_lock` | L2 | Global | Port slot allocation table and port state transitions (`PORT_FREE`, `PORT_STARTING`, `PORT_ACTIVE`). |
| `port->lock` | L3 | Per-port | Port-specific descriptors: client socket FDs (`rfd`, `wfd`), thread handles, and running flags. |
| `mac_table.buckets[b].lock` | L4 | Striped (64 buckets) | Individual MAC table hash bucket linked lists, LRU node pointers, and timestamps. |
| `queue->lock` | L5 | Per-port queue | 3-priority FIFO ring buffers, frame pointers, token bucket counters, and queue capacity counts. |
| `shm->mtx` | L6 | Process-shared (SHM) | Shared-memory global packet counters (ingress, egress, drops, mirrored frames). |
| `log_ring.lock` | L7 | Global | Asynchronous logging ring buffer read/write indices and log message slots. |
| `reap_mutex` | Aux | Switch | List of port indices awaiting cleanup by the reaper thread. |
| `aging_lock` | Aux | Switch | MAC aging timer sleep/wake synchronization. |
| `refill_lock` | Aux | Switch | Rate-limit refill timer sleep/wake synchronization. |
| `adm_mutex` | Aux | Switch | Active admission worker count and pending connection drain condvar. |

---

## 4. Lock Order Hierarchy

To mathematically guarantee deadlock freedom across all threads, every thread strictly acquires multiple locks in the following global order:

The lock hierarchy is ordered from the configuration lock, to port-slot allocation, port state, MAC-table buckets, output queues, shared statistics, and finally the logger. Operations that need several locks acquire them in this order. They never acquire a lock from a higher level before finishing work on a lower-level lock, because doing so could create a circular wait between threads.

### Rules Preventing Deadlocks:
1. **Strict Hierarchy**: No thread may acquire a higher-level lock while holding a lower-level lock (e.g., a thread holding `queue->lock` (L5) never attempts to acquire `cfg_lock` (L1) or `slot_lock` (L2)).
2. **Multiple Port Rule**: If an administrative or forwarding operation must touch multiple ports (such as port mirroring), ports are always locked in strictly ascending index order (`port[i].lock` before `port[j].lock` where $i < j$).
3. **Atomic Fast Path**: Fast-path frame forwarding avoids taking `port->lock` entirely by using C11 atomic operations (`atomic_load_explicit`) for port state, VLAN ID, trunk mode, and mirror target.

---

## 5. Real Bug Found and Fixed (From `BUGLOG.md`)

### Bug: Cross-port data race on `frame_buf_t` during broadcast fan-out
* **What Happened**: To achieve zero-copy broadcast forwarding, the switch pushes a single ref-counted `frame_buf_t` into the output queues of all destination ports. Initially, the frame's enqueue timestamp was stored inside the shared `frame_buf_t` structure (`fb->enqueue_time = now`). When a broadcast was fanned out across multiple ports, multiple `tx_thread` workers concurrently updated and checked `fb->enqueue_time` under different per-port queue mutexes.
* **How Found**: During high-concurrency stress testing (`t11_signals_stress.sh`), ThreadSanitizer flagged a data race on `fb->enqueue_time` between `queue.c:94` and `queue.c:72`.
* **How Fixed**: Removed `enqueue_time` from the shared `frame_buf_t` structure entirely. Enqueue timestamps were moved into a slot-local timestamp array (`enqueue_times[QUEUE_PRIOS][capacity]`) inside each port's private `out_queue_t`. Each timestamp is now strictly protected by its own port's `q->lock`, eliminating all cross-thread data races while preserving zero-copy payload sharing.

---

## 6. Completed Items and Grading Summary

### Tier 1: Core Switch (Completed - 50%)
- **Item 1: Wire Protocol**: Implemented robust 17-byte header parsing with `read_full` and `write_full`. Handles partial reads/writes, coalesced frames, and client disconnections (`SIGPIPE` ignored, `EPIPE` handled cleanly).
- **Item 2: No Head-of-Line Blocking**: Each port has its own sender thread and bounded queue. When a queue is full, the frame is dropped immediately without blocking the receiving port.
- **Item 3: Zero-Copy Broadcasts**: Broadcast and flooded frames use atomic reference counting (`atomic_fetch_add` / `atomic_fetch_sub`). The last port to transmit frees the buffer. Verified clean under Valgrind and ThreadSanitizer.
- **Item 4: Concurrent MAC Table**: 64 striped bucket locks, dynamic time-based entry aging, LRU table-wide eviction when full, and per-port security limits that shut down ports exceeding MAC thresholds.
- **Item 5: Port Limit**: Counting semaphore manages available port slots. Extra connections block up to `accept_wait_ms` before receiving `SWITCH FULL\n` and disconnecting.

### Tier 2: Advanced Features (Completed - 35%)
- **Item 6: Quality of Service**: 3-priority queues (High, Normal, Low) served with Weighted Round Robin (5:3:1 ratio) plus age-based anti-starvation promotion.
- **Item 7: Rate Limiting**: Egress token-bucket rate limiter with fractional accumulation for low rates. Sender threads wait using `pthread_cond_timedwait` without busy-waiting.
- **Item 8: VLANs & Trunk Port**: Access ports tag incoming frames and strip tags on delivery. Dedicated trunk port carries 802.1Q tagged frames for all VLANs. Broadcasts remain strictly contained within their VLAN.
- **Item 9: Port Mirroring**: Non-blocking copy of ingress and egress traffic forwarded to a designated sniffer port.
- **Item 10: Live Reconfiguration**: Port shutdown/up, VLAN reassignment, and MAC table clearing while traffic is running. Strict lock order prevents deadlocks.
- **Item 11: Admin Tool (`swctl`)**: UNIX domain socket server and CLI client supporting all inspection and reconfiguration commands. Live stats backed by POSIX shared memory (`/vswitch_stats`).
- **Item 12: POSIX Signal Handling**: Dedicated `sigwait()` thread handles `SIGINT` (graceful shutdown), `SIGHUP` (dynamic config reload), and `SIGUSR1` (MAC table dump).
- **Item 13: Asynchronous Logger**: Non-blocking bounded ring buffer with a dedicated logger thread writing microsecond timestamps to disk.

### Report (Completed - 15%)
- Detailed thread diagram, lock table, lock hierarchy explanation, bug analysis, and feature completion summary.

---

## 7. Assumptions
1. **Switch Full Response**: When all ports are occupied and the admission wait timeout expires, the switch transmits the ASCII string `SWITCH FULL\n` before closing the connection.
2. **Access Port VLAN Assignment**: If an incoming frame on an access port already contains a non-zero VLAN field, the switch overwrites it with the access port's configured VLAN, ensuring untrusted hosts cannot spoof VLAN tags.
3. **No Busy Waiting**: All synchronization uses POSIX condition variables (`pthread_cond_wait`, `pthread_cond_timedwait`), semaphores (`sem_wait`, `sem_timedwait`), or non-blocking `poll()` loops. The `sleep()` call is only used in `host.c` (test simulator) and `swctl.c` (`--watch` display loop).
