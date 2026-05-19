"""Smoke test: the Zephyr binary listens on UDP/5683 and the CoAP server
returns `4.04 Not Found` for an unknown path. Proves the aiocoap fixture
reaches the Zephyr stack end-to-end through QEMU's SLIRP port forward.

Skipped on macOS: the Zephyr SDK's bundled `qemu-system-i386` is built
without the `user` netdev backend, so SLIRP port forwarding only works
with a host QEMU (Homebrew) plus extra environment wiring that isn't in
place yet. CI on Linux runs the test for real.
"""

from __future__ import annotations

import platform

import pytest
from aiocoap import GET, Message, NOT_FOUND
from twister_harness import DeviceAdapter

pytestmark = pytest.mark.skipif(
	platform.system() == "Darwin",
	reason="bundled Zephyr SDK qemu-system-i386 lacks slirp; tracked as a follow-up",
)


@pytest.mark.asyncio
async def test_unknown_path_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	"""The `dut` fixture boots the Zephyr binary in QEMU before the body runs."""
	host, port = coap_endpoint
	request = Message(code=GET, uri=f"coap://{host}:{port}/no-such-path")
	response = await aiocoap_client.request(request).response
	assert response.code == NOT_FOUND, f"expected 4.04, got {response.code}"
