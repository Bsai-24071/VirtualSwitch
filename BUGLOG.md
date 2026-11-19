# Bug Log

## Bug 1: Data race on `sw->stop` during signal shutdown
- **What happened:** Signal thread wrote `sw->stop = 1` while the aging thread was concurrently reading `while (!sw->stop)`.
- **How found:** ThreadSanitizer reported a data race at `sigthread.c:25` against `switch.c:44` during shutdown.
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
