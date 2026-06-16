# CoAP frontend footprint gate

A standing CI gate that asserts the CoAP frontend adds **zero footprint**
to an app built with `CONFIG_ZEPHLETS_COAP=n`. The only frontend-aware
change to core infra is the `ZEPHLET_NEW_PRIO` hook, which expands to
nothing when no frontend is opted in — this gate keeps that promise
honest as the frontend grows.

## What it checks

`footprint_gate.sh check <build_dir>` extracts the runtime image of
`<build_dir>/zephyr/zephyr.elf` and compares its hash to the committed
baseline:

```
objcopy --only-section .text --only-section .rodata \
        --only-section .data  --only-section .bss   zephyr.elf out
sha256(out)
```

`objcopy` is read from the build's own `CMakeCache.txt`
(`CMAKE_OBJCOPY`), so the gate always uses the toolchain that produced
the ELF.

## Why it reproduces

- **Board `mps2/an385`** is an ARM cross-compile target, so the image
  does not depend on the build host's architecture.
- **`CONFIG_BUILD_OUTPUT_STRIP_PATHS=y`** (Zephyr default) strips
  `__FILE__` paths out of `.rodata`, so the image does not depend on the
  build directory.
- The hash is therefore tied only to the pinned **Zephyr SDK** and the
  Zephyr/module revisions in the manifest.

`baseline.mps2_an385.txt` holds the expected hash.

## Regenerating the baseline

Regenerate **only** for an intentional SDK or manifest bump — never to
paper over an unexplained drift. Use the same SDK the CI pins, which the
`zephlet-tester` image already ships:

```sh
# from the workspace root
docker run --rm -u root -v "$PWD":/workdir -w /workdir/ports_adapters_zbus \
    zephlet-tester:latest \
    west build -b mps2/an385 . -d build_n -- -DCONFIG_ZEPHLETS_COAP=n

modules/lib/zephlet/tests/coap_buildhash/footprint_gate.sh \
    update ports_adapters_zbus/build_n
```

Commit the changed `baseline.mps2_an385.txt` together with the change
that motivated the bump, and say why in the commit message.

## CI

The `buildhash` job in `.github/workflows/ci.yml` builds the example app
with `CONFIG_ZEPHLETS_COAP=n` for `mps2/an385` and runs
`footprint_gate.sh check`.
