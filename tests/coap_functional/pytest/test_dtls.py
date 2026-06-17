"""DTLS-PSK functional cases for the CoAP frontend.

Built against the `zephlet.coap_functional.dtls` sub-target, which sets
`CONFIG_ZEPHLETS_COAP_DTLS=y` with a test PSK (identity `Client_identity`,
key `0123456789`). With DTLS the frontend serves CoAPS on 5684 instead of
plain CoAP on 5683.

aiocoap has no PSK-DTLS binding in this environment, so the client is
libcoap's `coap-client` driven as a subprocess (CI installs `libcoap3-bin`;
the binary is usually `coap-client-gnutls`). The cases cover the issue's
DTLS acceptance: handshake succeeds with the test PSK, an RPC round-trips,
and a wrong PSK is rejected.
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


def _run(cc: str, method: str, path: str, key: str = PSK_KEY, timeout: float = 15.0):
    uri = f"coaps://{HOST}:{PORT}/{path}"
    return subprocess.run(
        [cc, "-u", PSK_ID, "-k", key, "-m", method, uri],
        capture_output=True,
        timeout=timeout,
    )


@pytest.fixture
def coaps(dut: DeviceAdapter) -> str:
    """A coap-client path, once the DTLS service answers a handshake.

    Function-scoped to match twister's function-scoped `dut`; once the
    server is up the readiness GET returns on the first attempt, so the
    per-test cost is negligible.
    """
    cc = _coap_client()
    deadline = time.time() + READY_TIMEOUT_S
    last = None
    while time.time() < deadline:
        try:
            last = _run(cc, "get", "zlet/tick/instances")
            if b"tick_fast" in last.stdout:
                return cc
        except subprocess.TimeoutExpired as exc:
            last = exc
        time.sleep(1.0)
    raise AssertionError(
        f"CoAPS server not reachable on {HOST}:{PORT} within {READY_TIMEOUT_S}s; last={last!r}"
    )


def test_dtls_handshake_and_discovery(coaps: str):
    """Handshake with the test PSK, then a GET that returns the instance list."""
    out = _run(coaps, "get", "zlet/tick/instances")
    assert b"tick_fast" in out.stdout, f"expected instances, got {out.stdout!r} / {out.stderr!r}"


def test_dtls_rpc_roundtrips(coaps: str):
    """An RPC POST over DTLS reaches the dispatcher and a response comes back.

    `tick.start` returns a non-empty `Lifecycle.Status`, so a 2.05 with a
    protobuf payload proves the call round-tripped over the secure socket.
    """
    out = _run(coaps, "post", "zlet/tick/tick_fast/start")
    assert out.stdout, f"expected a non-empty RPC response, got stderr={out.stderr!r}"


def test_dtls_wrong_psk_rejected(coaps: str):
    """A client presenting the wrong PSK cannot complete the handshake."""
    try:
        out = _run(coaps, "get", "zlet/tick/instances", key="WRONGKEY99", timeout=12.0)
        stdout = out.stdout
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or b""
    assert b"tick_fast" not in stdout, "wrong PSK must not yield a valid response"
