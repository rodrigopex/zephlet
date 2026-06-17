# CoAP wire interoperability checks

The `coap_functional` twister suite proves the frontend's behaviour against
**aiocoap** (plain CoAP) and **libcoap** (`coap_functional.dtls`, CoAPS-PSK).
Those run on every CI push. This note covers the **manual / occasional**
interop checks against independent implementations — they confirm the
frontend speaks the wire format other CoAP stacks expect, and are **not**
gating on PR merge.

## Target under test

Build and run the example app with the CoAP overlay, which binds the
frontend on UDP/5683 (and 5684 for DTLS):

```sh
# from the workspace root, inside the zephlet-tester image
west build -b native_sim/native/64 ports_adapters_zbus \
    -d build_coap -- -DOVERLAY_CONFIG=prj_coap.conf
build_coap/zephyr/zephyr.exe &
```

The native-simulator offloaded-sockets driver binds the host's
`127.0.0.1`, so any host CoAP client reaches the server directly.

## libcoap (plain CoAP)

```sh
coap-client -m get  coap://127.0.0.1:5683/zlet/tick/instances
coap-client -m post coap://127.0.0.1:5683/zlet/tick/tick_fast/start
coap-client -m get  coap://127.0.0.1:5683/.well-known/core
```

## libcoap (CoAPS / DTLS-PSK)

Build with `-DOVERLAY_CONFIG="prj_coap.conf;dtls.conf"` where `dtls.conf`
sets `CONFIG_ZEPHLETS_COAP_DTLS=y` and the PSK identity/key, then:

```sh
coap-client -u <identity> -k <key> -m get \
    coaps://127.0.0.1:5684/zlet/tick/instances
```

The DTLS-capable binary is usually `coap-client-gnutls` or
`coap-client-openssl` (Debian/Ubuntu `libcoap3-bin`).

## aiocoap (plain CoAP)

```python
import asyncio
from aiocoap import Context, Message, GET

async def main():
    ctx = await Context.create_client_context()
    req = Message(code=GET, uri="coap://127.0.0.1:5683/zlet/tick/instances")
    print((await ctx.request(req).response).payload)

asyncio.run(main())
```

aiocoap has no built-in PSK-DTLS binding in this environment, which is why
the automated DTLS suite uses libcoap.

## Running as an occasional CI job

These checks suit a manual `workflow_dispatch` or a scheduled (cron) job
rather than the per-push pipeline: they need a second CoAP implementation
installed and exercise the same paths the twister suite already gates. Add
them as a non-blocking job if drift against external stacks becomes a
concern.
