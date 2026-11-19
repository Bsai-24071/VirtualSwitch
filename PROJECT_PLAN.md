# Virtual Switch: Agent Plan (3 Phases)

Read this whole file first. Then do **only the phase you are told to do**.
Do not start the next phase. When a phase is done, stop and give a short summary.

| Phase | Covers | Spec weight |
|---|---|---|
| Phase 1 | Tier 1: items 1 to 5 (core) | 50% |
| Phase 2 | Tier 2: items 6 to 13 (advanced) | 35% |
| Phase 3 | Tier 3 bonus + Report + README + final checks | 15% report + up to 15% bonus |

---

## 0. Global Rules (apply to every phase)

1. Language: C11, Linux, POSIX threads. Build flags must stay exactly: `gcc -std=c11 -Wall -Wextra -Werror -g -O0`. You may add `-pthread` and link flags, and extra sanitizer flags for the `tsan`/`asan` targets only.
2. Every `.c` file that needs POSIX starts with `#define _POSIX_C_SOURCE 200809L`, like the lab files. Use only POSIX APIs (no `eventfd`, no `pipe2`). Use `pipe()` + `fcntl()`.
3. Check the return value of every system call and library call that can fail (`pthread_*` too). Style: copy the lab files (`rc = pthread_mutex_lock(...)`, `strerror(rc)`).
4. **No busy waiting.** No `while (!flag);`. No `sleep()`/`usleep()` as synchronisation in the switch or swctl. Periodic threads (aging, timer) wait with `pthread_cond_timedwait` on a stop condvar, so shutdown can wake them. `sleep` is allowed only inside `host.c` and test scripts.
5. **No comments in code unless really required** (for example a lock-order warning or a non-obvious reason). No banner comments, no commented-out code.
6. Ignore `SIGPIPE`. Never assume one `read()` is one frame.
7. Valgrind and ThreadSanitizer must be clean at the end of every phase.
8. `make` builds everything. `make clean` removes the programs and objects. Add `make tsan` and `make asan` (separate build dir, separate binaries).
9. Keep a file `BUGLOG.md`. Every time you hit a real bug (race, leak, deadlock, wrong output), add 3 lines: what happened, how you found it, how you fixed it. The report in Phase 3 needs one real bug. Do not invent one.
10. Keep README.md short and update it at the end of each phase.
11. Do not add features that are not in the project spec.

---

## 1. The Lab09 Files (already given)

| File | Verdict | What to do |
|---|---|---|
| `frame.h`, `frame.c` | Part of the project (the fixed frame format) | **Do not change.** Use as is. |
| `mac_table.h`, `mac_table.c` | Part of item 4 (striped hash table, moved detection). Missing: aging, size limit with LRU, port security, per-port removal, dump, clear | **Keep the existing function names, signatures, return codes and bucket design.** Extend only (details in Phase 1 and 2). |
| `Makefile` | Builds only 2 objects | Replace with a full Makefile. Keep the same `CC` and `CFLAGS` lines. |

Notes about the existing code:
- `mac_table_lookup` returns `-1` for an unknown MAC. Ports are numbered from 0.
- `mac_table_insert` prints `MAC moved` with `fprintf`. Keep it in Phase 1. In Phase 2 the only allowed edit of old behaviour is to remove this `fprintf` and log from the caller through the logger.
- The table is keyed by MAC only (no VLAN in the key). Keep it that way.

---

## 2. Final File Layout

`(L)` = from lab, `(E)` = extended, `(N)` = new in that phase, `(M)` = modified in that phase.

| File | Purpose | Phase 1 | Phase 2 | Phase 3 |
|---|---|---|---|---|
| `frame.h/.c` | frame header | (L) keep | keep | keep |
| `mac_table.h/.c` | MAC table | (E) | (M) | keep |
| `Makefile` | build | (N) | (M) | (M) |
| `common.h/.c` | I/O helpers, frame buffers, stats, config | (N) | (M) shared memory/config reload | |
| `queue.h/.c` | bounded per-port queue | (N) one FIFO | (M) 3 queues, WRR, tokens | |
| `switch.h` | `switch_t`, `port_t` structs and states | (N) | (M) | (M) |
| `port.h/.c` | port lifecycle, rx/tx threads, reaper | (N) | (M) | (M) |
| `port.c` | forwarding, learn, lookup, unicast, flood | (N) | (M) VLAN, mirror | |
| `switch.c` | main, acceptor, admission, signal and aging threads | (N) | (M) | (M) |
| `host.c` | test host | (N) | (M) more modes | |
| `ratelimit.h/.c` | token refill timer thread | | (N) | |
| `logger.h/.c` | log queue + logger thread | | (N) | |
| `admin.h/.c` | UNIX socket admin server | | (N) | |
| `swctl.c` | admin tool | | (N) | |
| `supervisor.c` | restart on crash | | | (N) |
| `twoswitch.c` | launcher: 2 switches + 2 pipes | | | (N) |
| `mac_shadow.h/.c` | MAC table copy in shared memory | | | (N) |
| `bench.c`, `bench_tables.h/.c` | benchmark | | | (N) |
| `tests/*.sh` | test scripts | (N) 7 | (N) 6 | (N) 2 |
| `README.md`, `BUGLOG.md` | docs | (N) | (M) | (M) |
| `REPORT.md` | report | | | (N) |

---

## 3. Shared Design (follow this from Phase 1 so later phases need no rewrite)

### 3.1 Threads
| Thread | Count | Job |
|---|---|---|
| main | 1 | init, start threads, wait for signal thread, ordered shutdown |
| signal thread | 1 | `sigwait` only. All other threads block signals (set mask in `main` before creating any thread) |
| acceptor | 1 | `poll` on listen socket + stop pipe, `accept` |
| admission | 1 per new connection, short-lived | `sem_timedwait` for a free port, then start the port or send "switch full" |
| receiver | 1 per port | read frames, learn MAC, forward |
| sender | 1 per port | pop from the port queue, write to the host |
| reaper | 1 | joins and cleans ports that ended (a thread never joins itself) |
| aging | 1 | removes old MAC entries |
| timer | 1 (Phase 2) | refills token buckets |
| admin | 1 (Phase 2) | UNIX socket commands |
| logger | 1 (Phase 2) | writes the log file |

### 3.2 Port states and the semaphore
States: `FREE`, `STARTING`, `ACTIVE`, `CLOSING`, `ADMIN_DOWN`, `SECURITY_DOWN`.
Invariant: **the value of the free-ports semaphore equals the number of ports in `FREE`.**
- Admission thread: `sem_timedwait` (absolute `CLOCK_REALTIME` deadline) then pick a `FREE` slot under `slot_lock`.
- Host disconnects: reaper cleans up, state `FREE`, `sem_post`.
- `ADMIN_DOWN` / `SECURITY_DOWN`: no `sem_post` until `up` is used.
- "Switch full" reply: send the ASCII line `SWITCH FULL\n`, then close. Write this in the README.

### 3.3 Port objects live for the whole run
`port_t ports[num_ports]` is allocated once at start. Each port's queue is reused (reset on reuse). Teardown frees only per-connection things (fd, queued frames, MAC entries of that port). This avoids use-after-free when another thread pushes into a port that is closing: the queue just has a `closed` flag and refuses.

### 3.4 Waking blocked threads (no shutdown tricks)
Each port has a wake pipe `wake[2]` (non-blocking). The receiver and sender never block in plain `read`/`write`. They `poll` on `{socket fd, wake[0]}` (`POLLIN` for rx, `POLLOUT` for tx). To stop a port: set state, close queue (broadcast its condvars), write 1 byte to `wake[1]`. This works for sockets and for pipes (needed in Phase 3).
`port_t` has separate `rfd` and `wfd` (equal for TCP sockets). Use `write()`, not `send()`, so pipes work too.

### 3.5 Wire handling rules
- Receiver: `read_full` the 17-byte header, deserialize, check `length <= FRAME_MAX_PAYLOAD` (if not, close the port: the stream cannot be re-synced), `read_full` the payload.
- EOF or error in the middle of a frame: drop the partial frame, close the port.
- Be lenient on access ports: ignore the host's `vlan` field, clamp `priority > 2` to normal. Do not call `frame_header_validate` on access ports. (Assumption to write in README.)
- Never learn a broadcast source MAC.
- A zero-length payload is valid.
- `frame_buf_t` holds the **wire-ready bytes** (serialized header + payload in one buffer), so the sender does one `write_full`.

### 3.6 Frame buffer reference counts
`frame_buf_new(hdr, payload)` returns refcount 1. `frame_buf_retain` before each push to a queue. `frame_buf_release` frees when the count reaches 0 (`atomic_fetch_sub` with `memory_order_acq_rel`).
Queue push contract: caller retains first; if push fails (full or closed), the **caller** releases. The receiver releases its own reference after the fan-out.

### 3.7 Lock order (authoritative; copy into the report)
Always take locks in this order. Never go backwards.

| Level | Lock | Protects |
|---|---|---|
| L1 | `sw->cfg_lock` | the live config |
| L2 | `sw->slot_lock` | port state changes and slot allocation |
| L3 | `port->lock` | one port's connection data. If several, take **in ascending port index** |
| L4 | MAC bucket lock | one bucket. Never hold two at once |
| L5 | `queue->lock` | one queue (and its token bucket). Never hold two at once; mirror pushes happen one after another |
| L6 | stats mutex (shared memory) | counters |
| L7 | log queue lock | log queue |

Extra rules:
- L4 to L7 are leaf locks: no other lock is taken while holding them.
- Never hold any lock during blocking I/O (`read`, `write`, `poll`, `accept`).
- The fast path (forwarding) reads `vlan`, `state`, `trunk`, `mirror_to` through atomics, not through `port->lock`.

### 3.8 Config keys (`key=value`)
Phase 1: extra command line args of the form `key=value` override defaults (used by tests). Phase 2: `config=<path>` loads a file with the same keys. File format: one `key=value` per line, `#` starts a comment, blank lines ignored.

| Key | Default | Reload on SIGHUP |
|---|---|---|
| `queue_capacity` | 64 | no |
| `accept_wait_ms` | 3000 | no |
| `mac_age_sec` | 300 | yes |
| `mac_max_entries` | 1024 | yes |
| `mac_max_per_port` | 8 | yes |
| `rate_fps` (0 = off) | 0 | yes |
| `wrr` | `5:3:1` | yes |
| `prio_age_ms` | 200 | yes |
| `vlan.<port>` | 1 | yes |
| `trunk_port` (-1 = none) | -1 | yes |
| `log_file` | `switch.log` | no |
| `dump_file` | `mac_dump.txt` | no |
| `sock_path` | `/tmp/vswitch.sock` | no |
| `shm_name` | `/vswitch_stats` | no |

### 3.9 Clean shutdown order (SIGINT)
signal thread sets stop flag and wakes the acceptor, then: acceptor joined, admission threads finished, all ports closed and joined by the reaper, aging/timer/admin joined, logger flushed and joined last, then destroy table, queues, semaphore, shm (`shm_unlink`), and free everything.

---

## PHASE 1: Tier 1 Core (items 1 to 5)

**Goal:** a working switch with learning, forwarding, flooding, broadcast, no head-of-line blocking, ref-counted frames, a full MAC table and a port limit. Must pass valgrind and TSan.

### 1A. Files to create or change (about 21 files)
- Keep: `frame.h`, `frame.c`
- Extend: `mac_table.h`, `mac_table.c`, replace `Makefile`
- New (9 source/header files): `common.h/.c`, `queue.h/.c`, `switch.h`, `port.h/.c`, `switch.c`, `host.c`
- New docs: `README.md`, `BUGLOG.md`
- New tests: 7 scripts in `tests/`

### 1B. Build order (do in this order, compile after each step)
0. **Check the Lab09 files first (before writing anything new).**
   - Read `frame.h`, `frame.c`, `mac_table.h`, `mac_table.c`, `Makefile`. All of them are part of the project, so none is removed.
   - Build them with the lab Makefile and confirm zero warnings.
   - Write a tiny temporary test (delete it afterwards) that calls `frame_header_serialize`/`deserialize` and `mac_table_insert`/`lookup` from two threads, to confirm the lab code works before building on it.
   - Reuse them everywhere: use `frame_header_t`, `frame_header_serialize`, `frame_header_deserialize` and `frame_mac_is_broadcast` for all frame work, and `mac_table_*` for all MAC work. Do not write a second copy of any of this.
   - If you find a real bug in a lab file, do not silently rewrite it. Tell the user what the bug is and the smallest fix, then record it in `BUGLOG.md`.
1. **Makefile.** Targets: `all` (switch, host), `clean`, `tsan`, `asan`. Links `-pthread`.
2. **util:** `read_full` and `write_full` (loop on partial transfers, retry on `EINTR`), both with a wake fd: return a distinct code for "wake fired", for EOF, for error. Add a monotonic time helper.
3. **frame_buf:** item 3 (section 3.6).
4. **out_queue (item 2):** bounded FIFO, mutex + `not_empty` condvar. API: `out_queue_init(q, capacity)`, `out_queue_push(q, fb, prio)` (never blocks; returns queued / dropped-full / closed), `out_queue_pop(q, &fb)` (blocks on condvar; returns -1 when closed), `out_queue_close`, `out_queue_reset`. `prio` is accepted but ignored in Phase 1 (Phase 2 uses it; do not change the API later).
5. **mac_table extension (item 4).** Keep old API. Add:
   - `mac_table_set_limits(t, max_entries, max_age_sec, max_per_port)` (values readable atomically so Phase 2 can change them live)
   - fields: `last_used` in each entry (updated on lookup and insert, used for LRU), `last_seen` (updated on insert/refresh, used for aging)
   - atomic total count and `port_count[MAC_TABLE_MAX_PORTS]` (define 64)
   - new return code `MAC_TABLE_PORT_LIMIT` (3): a **new** MAC on a port already holding M MACs is not inserted. Only the port's own receiver thread inserts for that port, so no race on its count.
   - LRU eviction: after an insert pushes the total above K, find the entry with the oldest `last_used` by scanning buckets **one lock at a time** (never two bucket locks), then lock that bucket and remove it. Release the insert's own bucket lock before evicting.
   - `mac_table_age(t, max_age_sec)`: scan buckets one by one, remove old entries.
   - `mac_table_remove_port(t, port)`: remove all entries of that port.
   - Keep all counts correct on insert, move, evict, age, remove.
   - `mac_table_destroy` must free everything.
6. **stats:** API like `stats_in(port)`, `stats_out(port)`, `stats_drop(port)`, `stats_mirror(port)`, `stats_snapshot(...)`. In Phase 1 the backing is atomic counters. Phase 2 swaps the backing for shared memory without changing callers.
7. **config:** `switch_config_t`, defaults, `config_set(cfg, key, value)`.
8. **switch.h / port:** `port_t` (state, `rfd`, `wfd`, `wake[2]`, queue, counters, threads) and `switch_t` (ports, mac table, semaphore, locks, config). Functions to start a port (create the two threads), stop a port (section 3.4), and the **reaper thread** (joins both port threads, closes fds, resets queue, `mac_table_remove_port`, sets `FREE`, `sem_post`). Threads never join themselves.
9. **fwd (items 1, 2, 3):** on each received frame: learn source (log "MAC moved" through the existing code; on `MAC_TABLE_PORT_LIMIT` shut the port down with state `SECURITY_DOWN`), look up destination. Known and not the ingress port: one push. Unknown or broadcast: flood to every `ACTIVE` port except ingress, **one buffer, one retain per destination**. Dropped push: release and count a drop. Frame to own ingress port: discard.
10. **sigthread:** block all signals in `main`, one thread does `sigwait` for `SIGINT`/`SIGTERM` and triggers the clean shutdown (section 3.9). `SIGPIPE` ignored.
11. **aging thread** (in `switch.c` or `port.c`): `pthread_cond_timedwait` loop calling `mac_table_age`.
12. **switch.c (item 5):** `./switch <tcp_port> <num_ports> [key=value ...]`. Validate arguments (`num_ports` 1 to 64). Create listen socket (`SO_REUSEADDR`). Semaphore `sem_init(&free_ports, 0, num_ports)`. Acceptor thread; per-connection admission thread (cap how many can wait; extra ones get "switch full" immediately); if `sem_timedwait` times out, send `SWITCH FULL\n` and close.
13. **host.c** (write early, needed for tests): `./host <switch_ip> <tcp_port> <mac> [options]`.
    - stdin commands: `send <dst_mac> <prio> <vlan> <text>`, `bcast <prio> <vlan> <text>`, `quit`
    - stdout for each received frame: `RX <src> <dst> v=<vlan> p=<prio> len=<n> <text>`
    - options: `--trickle` (write the frame 1 byte at a time), `--die-mid-frame` (send half a frame then exit), `--no-read` (never read: a frozen host), `--slow-read <ms>`, `--coalesce` (send 2 frames in one write)
    - also needs correct partial read handling itself
14. **Tests (7 scripts in `tests/`):**
    1. `t1_forward.sh`: A to B flooded first, B replies, then A to B goes only to B; C sees nothing.
    2. `t2_broadcast.sh`: broadcast reaches everyone except the sender.
    3. `t3_partial.sh`: `--trickle` and `--coalesce` hosts work.
    4. `t4_die_mid_frame.sh`: switch survives, port becomes free again.
    5. `t5_frozen_host.sh`: one `--no-read` host plus heavy traffic; other hosts keep full speed; drop counter rises.
    6. `t6_port_limit.sh`: host N+1 gets `SWITCH FULL` after about `accept_wait_ms`; if a host leaves before the timeout, the waiting host gets in.
    7. `t7_mac_table.sh`: MAC moved log, aging (use `mac_age_sec=2`), LRU (`mac_max_entries=4`), port security (`mac_max_per_port=2`).

### 1C. Phase 1 is done when
- [ ] `make` builds with zero warnings. `make clean` works.
- [ ] All 7 tests pass.
- [ ] `make tsan` run with tests: no TSan reports.
- [ ] `valgrind --leak-check=full --error-exitcode=1 ./switch ...` then Ctrl-C: no errors, no leaks.
- [ ] 100 hosts connecting and leaving repeatedly: no crash, no leak, semaphore invariant holds.
- [ ] README has: build, run, config keys, the `SWITCH FULL` reply, the access-port leniency assumption.
- [ ] Tell the user: files created, files changed, what is left for Phase 2.

---

## PHASE 2: Tier 2 Advanced (items 6 to 13)

**Goal:** QoS, rate limit, VLANs and trunk, mirroring, live admin, `swctl`, proper signals, logger. Do not break any Phase 1 test.

### 2A. Files to create or change (about 16 files)
- New (5 C/H pairs and 1 C): `ratelimit.h/.c`, `logger.h/.c`, `admin.h/.c`, `swctl.c`
- Modified: `queue.h/.c`, `common.c`, `config` and `stats` sections of `common.c`, `switch.h`, `port.h/.c`, `switch.c`, `mac_table.h/.c`, `host.c`, `Makefile`, `README.md`
- New tests: 6 scripts in `tests/`

### 2B. Build order (one item at a time, run all tests after each)
1. **Item 13 Logger first** (everything else logs through it). `log_msg(level, fmt, ...)` formats, timestamps (`CLOCK_REALTIME`) and pushes to a bounded queue under L7; **never blocks** (drop and count if full). One logger thread writes to `log_file`. Replace every `fprintf(stderr, ...)` that runs after startup, including the `MAC moved` one in `mac_table.c` (log it from the caller instead). Startup errors may still use stderr.
2. **Item 11 shared memory stats.** Change the stats section of `common.c` backing to `shm_open` + `ftruncate` + `mmap`. One struct: a `PTHREAD_PROCESS_SHARED` mutex (use `pthread_mutexattr_setpshared`) plus per-port `frames_in`, `frames_out`, `frames_dropped`, `frames_mirrored`. The switch creates it and calls `shm_unlink` on exit. Handle `swctl` opening it read-only side.
3. **Item 6 QoS** in `queue.c` only (API unchanged): 3 FIFOs inside one queue under one lock. Pop uses weighted round robin from `wrr` (for example 5:3:1). Aging: each frame has an enqueue time; a frame older than `prio_age_ms` is served next, so low priority is never starved. Shared capacity counted over all three FIFOs.
4. **Item 7 rate limit.** Token bucket state lives inside the queue (under `queue->lock`). `ratelimit.c` timer thread wakes with `pthread_cond_timedwait` on a stop condvar and refills every port's tokens (lock one queue at a time). The sender with 0 tokens waits with `pthread_cond_timedwait` on a tokens condvar, never sleeps or spins. `rate_fps=0` means off.
5. **Item 8 VLANs and trunk** in `port.c`:
   - Access port ingress: frame VLAN := port VLAN (set in the buffer before fan-out).
   - Trunk ingress: use the frame's `vlan` field; if outside 1 to 4094, drop and count.
   - Unicast delivered only if the destination port is in the same VLAN or is the trunk. Broadcast and unknown unicast flood only inside the VLAN, plus the trunk.
   - Frames to the trunk keep the `vlan` field. Only one trunk port (`trunk_port`).
6. **Item 9 mirroring.** `mirror_to` (atomic) on the port being watched. For every frame received on X and every frame queued to X, also retain and push the same buffer to the sniffer queue as a normal non-blocking push (drop and count if full). This never blocks X. Count `frames_mirrored`.
7. **Item 10 live reconfiguration** (hardest part, follow section 3.7 strictly):
   - `port_shutdown(p)`: state `ADMIN_DOWN`, close queue (drains and releases frames), write wake pipe, reaper joins both threads and frees. No `sem_post`.
   - `port_up(p)`: `ADMIN_DOWN` or `SECURITY_DOWN` to `FREE`, then `sem_post`.
   - If the port is `FREE` when shut down, consume its token with `sem_trywait`; keep the invariant.
   - `vlan <port> <vlan>`: update VLAN and remove that port's MAC entries.
   - `mirror <port> <sniffer>`: lock both ports in ascending index order.
   - `clear mac`: add `mac_table_clear(t)` (one bucket lock at a time).
   - Add `mac_table_dump(t, FILE*)` for items 11 and 12.
8. **Item 11 admin.** `admin.c`: thread listening on a UNIX domain socket (`sock_path`, remove stale path first, `poll` with stop pipe). Line based text protocol, one reply per command, replies end by closing the connection. `swctl.c` commands: `show mac`, `show ports`, `shutdown <port>`, `up <port>`, `vlan <port> <vlan>`, `mirror <port> <sniffer_port>`, `clear mac`, `stats`, `stats --watch` (reads shared memory directly, redraws every second; a plain sleep is fine here because it is only a display loop). Errors print a clear message and a non-zero exit code.
9. **Item 12 signals.** Extend the signal section of `switch.c`: `SIGHUP` reloads the config file (parse into a new struct, then swap under L1, apply new limits and rates live, keep old config if parsing fails). `SIGUSR1` writes `mac_table_dump` to `dump_file`. `SIGINT` unchanged but now also stops admin, timer, logger in the order of section 3.9. Check with `grep` that no thread except the signal thread can receive signals.
10. **Item 7/10 config file.** `config_load_file(cfg, path)` and the `config=<path>` argument.
11. **host.c additions:** `--sniffer` (print every frame, any destination), `--flood <n>` (send n frames as fast as possible with a given priority).
12. **Tests (6 scripts):** `t8_qos.sh` (low priority not starved, high first), `t9_ratelimit.sh`, `t10_vlan_trunk.sh`, `t11_mirror.sh`, `t12_admin_signals.sh` (every swctl command; SIGHUP, SIGUSR1, SIGINT while traffic flows), `t13_stress.sh` (many hosts, floods, admin commands, shutdown/up in a loop at the same time, for use with TSan).

### 2C. Phase 2 is done when
- [ ] All Phase 1 and Phase 2 tests pass.
- [ ] TSan and valgrind clean, **including** the stress test with admin commands and signals during traffic.
- [ ] `swctl stats --watch` shows live numbers from shared memory; no leftover shm or socket file after Ctrl-C.
- [ ] A `grep -rn "sleep" *.c` shows `sleep` only in `host.c` and `swctl.c`.
- [ ] Lock order in section 3.7 was followed (list any place where you had to take 2+ port locks and show it is ascending).
- [ ] README updated: all swctl commands, config file format, signals.
- [ ] `BUGLOG.md` has at least one real entry. Tell the user which one is best for the report.

---

## PHASE 3: Tier 3 Bonus + Report + Final Checks

**Goal:** the three bonus items, the 2 to 3 page report, final README, final cleanliness runs. Bonus never breaks Tier 1 and Tier 2.

### 3A. Files to create or change (about 12 files)
- New: `twoswitch.c`, `supervisor.c`, `mac_shadow.h/.c`, `bench.c`, `bench_tables.h/.c`, `tests/` 2 scripts, `REPORT.md`
- Modified: `port.h/.c` (trunk over two pipes), `switch.c`, `sw.h`, `Makefile`, `README.md`

### 3B. Bonus items
1. **Two switches over two pipes.**
   - `twoswitch.c` creates two pipes (A to B and B to A), forks and execs two `./switch` processes, passing the fds with extra args like `trunk_rfd=N trunk_wfd=M` (config keys; add them). Different TCP ports and different `sock_path`/`shm_name` for each switch.
   - In each switch, the trunk port uses `rfd` != `wfd` (already prepared in Phase 1). It is the trunk port of item 8, so VLAN tags are kept.
   - Frames on the pipe use the same 17-byte frame format, so the same receiver code reads them.
   - MAC learning across the link: the other switch's hosts are learned behind the trunk port, with no special code if the normal receiver path is used.
   - Test `t14_two_switch.sh`: host on A reaches host on B on the same VLAN; a different VLAN does not; broadcast stays in its VLAN across the link.
2. **Supervisor.** `supervisor.c`: `fork` + `exec` the switch, `waitpid`. Restart on crash or non-zero exit. Stop on clean exit (exit 0 after SIGINT) and forward SIGINT/SIGTERM to the child. Do not restart in a tight loop (use `sem`-free waiting: a `nanosleep` backoff in the supervisor is acceptable, it is not synchronisation). To keep the MAC table across a crash without touching `mac_table.c`: `mac_shadow.c` keeps a fixed-size copy of the entries in shared memory (process-shared mutex); the fwd path writes to it on insert, move, evict, age; on start the switch reloads it with `mac_table_insert`. Note in the report that TCP connections die with the crash, so hosts must reconnect. Test `t15_supervisor.sh`: `kill -9` the switch, it comes back, `show mac` still has entries.
3. **Benchmark.** `bench.c` is a multithreaded load generator for three table variants in `bench_tables.c`: one global lock, striped locks (same design as `mac_table.c`), and a read-write lock. Mix of 90% lookups and 10% inserts, thread counts 1, 2, 4, 8, 16. Output CSV. Make a graph (PNG) with a small Python or gnuplot command, and add 5 to 8 lines of result text to the report. Add `make bench`.

### 3C. Report (`REPORT.md`, 2 to 3 pages)
Use **short, simple English**. Short sentences. No filler. Sections:
1. **Overview** (3 to 4 lines): what was built.
2. **Threads** (a diagram and a table). Draw an ASCII diagram: acceptor to admission to port; per port receiver to queue to sender; reaper, aging, timer, admin, logger, signal thread. Then one line per thread type.
3. **Locks** (table): every lock, what data it protects. Include atomics and the semaphore.
4. **Lock order:** copy the table from section 3.7 and add two sentences on why it prevents deadlock (which commands take many locks and how).
5. **One real bug:** take it from `BUGLOG.md`. What happened, how it was found (TSan, valgrind, gdb, logs), the fix.
6. **Completed items:** list Tier 1 (1 to 5), Tier 2 (6 to 13), Tier 3 (which of the three), and anything not finished or any assumption (access-port leniency, `SWITCH FULL` text).
7. **Benchmark graph** (only if done).

Keep it to about 2 to 3 pages. Tell the user how to make a PDF (`pandoc REPORT.md -o REPORT.pdf`).

### 3D. Final checklist (run all of it)
- [ ] `make clean && make` from a fresh folder: zero warnings with `-std=c11 -Wall -Wextra -Werror -g -O0`.
- [ ] All tests `t1` to `t15` pass.
- [ ] TSan: full stress test, no reports.
- [ ] Valgrind: normal run, SIGINT, no errors, no leaks, no open fds left.
- [ ] No busy waiting; `sleep` only where allowed (host, swctl display loop, supervisor backoff, test scripts).
- [ ] Every system call return value is checked (grep for unchecked `close`, `write`, `pthread_`, `sem_`).
- [ ] No unnecessary comments in the code.
- [ ] README: build, run every program, config format, signals, swctl commands, assumptions.
- [ ] Hand-in folder has: all source, `Makefile`, `README.md`, `REPORT.md` (or PDF). `BUGLOG.md` and `tests/` are fine to include.