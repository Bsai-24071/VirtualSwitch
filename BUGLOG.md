# Bug Log

## Bug 1: Data race on `sw->stop` during signal shutdown
- **What happened:** Signal thread wrote `sw->stop = 1` while the aging thread was concurrently reading `while (!sw->stop)`.
- **How found:** ThreadSanitizer reported a data race in `switch.c` during shutdown.
- **How fixed:** Converted `stop` in `switch_t` to `atomic_int` using C11 `stdatomic.h` and accessed it via `atomic_load` and `atomic_store`.

## Bug 2: Uninitialized `pthread_t` join on rapid client disconnect
- **What happened:** When a client disconnected immediately on connection (`die-mid-frame`), `rx_thread` exited and requested port teardown before `port_start` finished initializing `tx_thread`, causing the reaper thread to call `pthread_join` on an uninitialized thread handle.
- **How found:** ThreadSanitizer crashed with `ConsumeThreadUserId CHECK failed: ((t)) != (0)` during `t4_die_mid_frame.sh`.
- **How fixed:** Held `port->lock` in `port_start` across both thread creations, tracked `rx_running`/`tx_running` flags, and ensured reaper only joins threads marked running.

## Bug 3: Slot allocation race in admission thread
- **What happened:** When multiple clients connected concurrently, admission threads located a `PORT_FREE` slot under `slot_lock` but did not change its state before releasing the lock, allowing multiple threads to assign the same port slot.
- **How found:** Code inspection of `admission_thread_fn` during concurrent admission debugging.
- **How fixed:** Marked `sw->ports[i].state = PORT_STARTING` immediately under `slot_lock` before unlocking.

## Bug 4: File descriptor race on host shutdown
- **What happened:** In `host.c`, the main thread called `close(g_sock)` before joining `rtid`, racing with `reader_thread_fn` which was still reading from the socket.
- **How found:** ThreadSanitizer reported a data race on file descriptor 4 between `close()` in `main` and `read()` in `reader_thread_fn`.
- **How fixed:** Replaced premature close with `shutdown(g_sock, SHUT_RDWR)`, called `pthread_join(rtid, NULL)`, and closed the descriptor only after the reader thread terminated.

## Bug 5: Hang in `out_queue_pop` on closed queue with zero rate tokens
- **What happened:** When closing a rate-limited queue that still held frames, `out_queue_pop` looped indefinitely waiting on `not_empty` for token refills that would never occur because `total > 0`.
- **How found:** Switch hang during port teardown in `t8_qos_rate.sh`.
- **How fixed:** Bypassed token rate-limiting wait whenever `q->closed` is set, allowing immediate flush or exit.

## Bug 6: Segfault in `swctl` locking process-shared mutex on read-only mmap
- **What happened:** `swctl` mapped stats shared memory with `PROT_READ` and then called `pthread_mutex_lock(&sh->mtx)`, which attempted to write to read-only memory, triggering SIGSEGV (signal 11).
- **How found:** Segmentation fault during `swctl stats` execution in `t10_admin.sh`.
- **How fixed:** Read counters lock-free directly from the read-only mapped shared memory segment without taking the mutex in `swctl`.

## Bug 7: Cross-port data race on `fb->enqueue_time` during broadcast
- **What happened:** When fan-out pushed a single ref-counted `frame_buf_t` to multiple ports, each port's `out_queue_push` wrote to `fb->enqueue_time` under different locks, racing with `pick_aged` reads on other ports.
- **How found:** ThreadSanitizer reported data race at `queue.c:94` vs `queue.c:72` during `t11_signals_stress.sh`.
- **How fixed:** Moved `enqueue_time` out of `frame_buf_t` into per-queue slot arrays `enqueue_times[QUEUE_PRIOS]` inside `out_queue_t`, protected by `q->lock`.
