# Issue 002: Simplify request-response with synchronous call helpers

> **Resolved by v0.3.** Sync RPC happens natively via zbus sync-listener
> semantics on the `rpc` pointer channel. No semaphores, no correlation
> IDs, no blocking-helper layer. Wrappers return `call.return_code`
> directly.

## Problem

The current request-response pattern requires the caller to:
1. Pick a `correlation_id` manually (no allocation scheme).
2. Wrap the call in `ZEPHLET_OBSERVE_REPORT()`.
3. Call the invoke helper (e.g., `zlet_tick_start(200, K_FOREVER)`).
4. Call `wait_report()` filtering by tag.
5. Manually check `has_context`, `correlation_id`, and `return_code`.

This is verbose and error-prone. The correlation ID is caller-managed with no guarantees of uniqueness. `wait_report` filters by tag but not by correlation ID, so concurrent callers could receive each other's responses.

**Current usage:**
```c
ZEPHLET_OBSERVE_REPORT(zlet_tick) {
    zlet_tick_start(200, K_FOREVER);
    report = zlet_tick_wait_report(TICK_REPORT_STATUS_TAG, K_MSEC(500));
}
if (report != NULL && report->has_context) {
    printk("return code=%d\n", report->context.return_code);
}
```

## Proposal

Generate synchronous call helpers that encapsulate the full request-response cycle:

```c
struct tick_report *report;
int err = zlet_tick_call_start(&report, K_MSEC(500));
/* err = zbus error, report->context.return_code = application error */
```

Internally:
- Auto-generate correlation IDs via atomic counter.
- Subscribe, publish invoke, wait for report matching both tag and correlation ID, unsubscribe.
- Return the report directly.

Keep the existing async helpers for fire-and-forget and adapter use cases.

## Trade-offs

- **Pro:** Natural RPC feel. Minimal caller code.
- **Pro:** Correct correlation by construction — no manual ID management.
- **Con:** Blocking call on RTOS — requires careful timeout and thread priority design.
- **Con:** Not suitable for ISR or time-critical paths (those should use the async path).

## Questions

- Should the sync helper be generated alongside the async one, or opt-in via proto annotation?
- What's the right default timeout behavior — caller-provided, or per-zephlet config?
- Should `wait_report` also filter by correlation ID even in the low-level API?
