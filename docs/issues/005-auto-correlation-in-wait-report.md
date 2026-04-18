# Issue 005: Auto-correlate responses in wait_report

> **Resolved by v0.3.** Correlation is gone entirely. `wait_report` is
> gone. Sync RPC uses zbus sync-listener semantics: by the time
> `zbus_chan_pub()` returns, the dispatcher has already mutated the
> in-place envelope. Identity is the pointer; nothing else needs to
> correlate.

## Problem

`wait_report()` currently filters only by report tag (e.g., `TICK_REPORT_STATUS_TAG`). It does **not** filter by `correlation_id`. This means:

- If two threads invoke `start` concurrently with different correlation IDs, either thread could receive the other's response.
- The caller must manually verify `report->context.correlation_id` after `wait_report` returns — but the generated code doesn't enforce this.
- The correlation ID is chosen by the caller with no uniqueness guarantee. Two callers could accidentally use the same ID.

This is the minimal-change fix: keep the current two-channel architecture but make correlation actually work.

## Proposal

1. **Auto-allocate correlation IDs** using an atomic counter internal to the generated code. Remove `correlation_id` from the public inline helpers.

2. **Filter by correlation ID in `wait_report`**. The function signature becomes:
   ```c
   struct tick_report *zlet_tick_wait_report(
       uint32_t correlation_id, int filter_tag, k_timeout_t timeout);
   ```
   It loops until it finds a report matching both tag and correlation ID (or times out).

3. **Return the allocated ID from invoke helpers** so callers can pass it to `wait_report`:
   ```c
   uint32_t id = zlet_tick_start(K_FOREVER);
   report = zlet_tick_wait_report(id, TICK_REPORT_STATUS_TAG, K_MSEC(500));
   ```

   Or, combine into a single sync helper (see Issue 002).

## Trade-offs

- **Pro:** Minimal architecture change — same channels, same dispatcher, same adapters.
- **Pro:** Fixes the concurrent-caller bug by construction.
- **Pro:** Removes the burden of ID management from callers.
- **Con:** `wait_report` may discard reports meant for other callers — needs a queue or multi-subscriber model for true concurrent access.
- **Con:** Atomic counter wraps at 2^32 — acceptable for most embedded use cases but not infinite.

## Questions

- Should discarded reports (wrong correlation ID) be re-published or buffered for other waiters?
- Is a single `zlet_tick_last_report` static variable sufficient, or does concurrent access require per-caller buffers?
- Could this be combined with Issue 002 (sync helpers) as a single change?
