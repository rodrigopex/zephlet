# Issue 003: Split fire-and-forget from request-response at the API level

## Problem

Currently, fire-and-forget and request-response use the same API surface. The only difference is whether `correlation_id` is 0 or non-zero. This means:

- Every inline helper takes a `correlation_id` parameter even when the caller doesn't care about the response.
- The caller must know that `0` means "no correlation" — a convention, not a type-safe distinction.
- The report channel, dispatcher, and context machinery are always present, even for pure command sinks.

**Fire-and-forget today:**
```c
zlet_tick_stop(0, K_FOREVER);  /* 0 = magic "I don't care" value */
```

**Request-response today:**
```c
zlet_tick_start(200, K_FOREVER);  /* 200 = manually chosen ID */
report = zlet_tick_wait_report(TICK_REPORT_STATUS_TAG, K_MSEC(500));
```

## Proposal

Split the API into two distinct call styles:

```c
/* Fire-and-forget — no correlation, no report expectation */
zlet_tick_start(K_FOREVER);

/* Request-response — blocks until report, correlation managed internally */
zlet_tick_start_sync(&report, K_MSEC(500));
```

Remove `correlation_id` from the public API entirely. Make it an internal mechanism of the sync variant.

## Trade-offs

- **Pro:** Clear intent at the call site — you know immediately if the caller expects a response.
- **Pro:** Fire-and-forget becomes simpler (one less parameter).
- **Pro:** Type safety — you can't accidentally ignore a response or forget to check correlation.
- **Con:** Two generated helpers per RPC method instead of one.
- **Con:** Needs a clear rule for which variant adapters should use.

## Questions

- Should adapters always use fire-and-forget (since they react to events, not request-response)?
- Does the sync variant belong in `_interface.h` or in a separate `_sync.h` to keep the base lightweight?
- Should fire-and-forget still publish to the report channel (for observability), or skip it entirely?
