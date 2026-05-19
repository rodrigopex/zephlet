"""Smoke test: the Zephyr binary listens on UDP/5683 and the CoAP server
returns `4.04 Not Found` for an unknown path. Proves the aiocoap fixture
reaches the Zephyr stack end-to-end via the host-side `eth_native_tap`.
"""

from __future__ import annotations

import pytest
from aiocoap import GET, Message, NOT_FOUND
from twister_harness import DeviceAdapter


@pytest.mark.asyncio
async def test_unknown_path_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	"""The `dut` fixture boots the Zephyr binary before the body runs."""
	host, port = coap_endpoint
	request = Message(code=GET, uri=f"coap://{host}:{port}/no-such-path")
	response = await aiocoap_client.request(request).response
	assert response.code == NOT_FOUND, f"expected 4.04, got {response.code}"
