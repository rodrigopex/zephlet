"""DTLS-PSK functional case for the CoAP frontend.

Built against the `zephlet.coap_functional.dtls` sub-target, which sets
`CONFIG_ZEPHLETS_COAP_DTLS=y` with a test PSK (identity `Client_identity`,
key `0123456789`). With DTLS the frontend serves CoAPS on 5684 instead of
plain CoAP on 5683.

aiocoap has no PSK-DTLS binding in this environment, so the client is
libcoap's `coap-client` driven as a subprocess (CI installs `libcoap3-bin`;
the binary is usually `coap-client-gnutls`).

The whole DTLS acceptance is exercised in a single test: twister's `dut`
fixture is function-scoped, so each test function reboots the binary and
re-handshakes. Folding handshake, RPC, and the wrong-PSK check into one
case keeps that cost to a single boot — important on the slower 32-bit
`native_sim/native` runner CI uses.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time

import pytest
from twister_harness import DeviceAdapter

HOST = os.environ.get("ZEPHLET_COAP_HOST", "127.0.0.1")
PORT = int(os.environ.get("ZEPHLET_COAPS_PORT", "5684"))
PSK_ID = "Client_identity"
PSK_KEY = "0123456789"
READY_TIMEOUT_S = float(os.environ.get("ZEPHLET_COAPS_READY_TIMEOUT_S", "30"))


def _coap_client() -> str:
    """Locate a DTLS-capable libcoap client; skip if none is installed."""
    for name in ("coap-client-gnutls", "coap-client-openssl", "coap-client"):
        path = shutil.which(name)
        if path:
            return path
    pytest.skip("libcoap coap-client (DTLS-capable) not available")


def _run(cc: str, method: str, path: str, key: str = PSK_KEY, timeout: float = 12.0):
    """Run coap-client; return captured bytes, treating a timeout as no reply."""
    uri = f"coaps://{HOST}:{PORT}/{path}"
    try:
        out = subprocess.run(
            [cc, "-u", PSK_ID, "-k", key, "-m", method, uri],
            capture_output=True,
            timeout=timeout,
        )
        return out.stdout, out.stderr
    except subprocess.TimeoutExpired as exc:
        return (exc.stdout or b""), (exc.stderr or b"")


def _wait_ready(cc: str) -> None:
    deadline = time.time() + READY_TIMEOUT_S
    last = (b"", b"")
    while time.time() < deadline:
        last = _run(cc, "get", "zlet/tick/instances")
        if b"tick_fast" in last[0]:
            return
        time.sleep(1.0)
    raise AssertionError(
        f"CoAPS server not reachable on {HOST}:{PORT} within {READY_TIMEOUT_S}s; "
        f"last stdout={last[0]!r} stderr={last[1]!r}"
    )


def test_dtls_psk_acceptance(dut: DeviceAdapter):
    """Handshake + discovery, an RPC round-trip, and wrong-PSK rejection."""
    cc = _coap_client()
    _wait_ready(cc)

    # 1. Handshake with the test PSK, then a GET returns the instance list.
    stdout, stderr = _run(cc, "get", "zlet/tick/instances")
    assert b"tick_fast" in stdout, f"discovery GET failed: {stdout!r} / {stderr!r}"

    # 2. An RPC POST reaches the dispatcher; tick.start answers a non-empty
    #    Lifecycle.Status (2.05), so a payload comes back over the secure socket.
    stdout, stderr = _run(cc, "post", "zlet/tick/tick_fast/start")
    assert stdout, f"RPC POST returned no response: stderr={stderr!r}"

    # 3. A client presenting the wrong PSK cannot complete the handshake.
    bad_stdout, _ = _run(cc, "get", "zlet/tick/instances", key="WRONGKEY99", timeout=8.0)
    assert b"tick_fast" not in bad_stdout, "wrong PSK must not yield a valid response"
