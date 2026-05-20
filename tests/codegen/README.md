# Codegen tests

Standalone pytest suite for `codegen/generate_zephlet.py`. No Zephyr toolchain
or board required — runs end-to-end against the generator in a temp dir.

## Run

```
pytest modules/lib/zephlet/tests/codegen/
```

The repo's `zephyr` venv (selected via `.python-version`) provides
`jinja2` and `proto-schema-parser`. From a workspace where that venv is
active, the bare `pytest` command above works.

## What it covers

- `detect_coap_opt_in()` — positive, negative, comment-stripping, and
  whitespace-robust regex behavior.
- `_interface.h` carries the per-type aggregator and default-empty hook
  for both opted-in and non-opted-in zephlets.
- Non-opted-in: `_coap_interface.{h,c}` are content-empty stubs (0-symbol
  TU under `CONFIG_ZEPHLETS_COAP=y`).
- Opted-in: `_coap_interface.{h,c}` carry the override, the per-method
  table, the `STRUCT_SECTION_ITERABLE(zephlet_coap_type, …)` record, and
  the stub event callback.
- **Regression invariant:** the `_interface.c` file is byte-identical
  with and without the opt-in — the pre-CoAP dispatch surface never
  changes as a side-effect of opting a zephlet in.
