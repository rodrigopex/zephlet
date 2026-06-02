"""Truncation case for the per-type `/instances` handler.

Run against the `zephlet.coap_functional.discovery_truncate` sub-target,
which pins `CONFIG_ZEPHLETS_COAP_DISCOVERY_BUF_SIZE=16`. Even a single
instance link (`</zlet/tick/tick_fast>;rt="zlet.instance"` ≈ 48 chars)
exceeds the buffer, so the first snprintf into `buf` overflows and the
handler must surface 4.13 Request Entity Too Large.

`/apis` is unaffected by the Kconfig — its response is a fully
codegen-baked static const string, no runtime sizing. It must still
return 2.05 in this sub-target.
"""

from __future__ import annotations

import pytest
from aiocoap import GET, CONTENT, REQUEST_ENTITY_TOO_LARGE, Message
from twister_harness import DeviceAdapter


@pytest.mark.asyncio
async def test_instances_truncation_returns_4_13(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	host, port = coap_endpoint
	req = Message(code=GET, uri=f"coap://{host}:{port}/zlet/tick/instances")
	resp = await aiocoap_client.request(req).response
	assert resp.code == REQUEST_ENTITY_TOO_LARGE, (
		f"expected 4.13, got {resp.code}"
	)


@pytest.mark.asyncio
async def test_apis_unaffected_by_discovery_buf(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	host, port = coap_endpoint
	req = Message(code=GET, uri=f"coap://{host}:{port}/zlet/tick/apis")
	resp = await aiocoap_client.request(req).response
	assert resp.code == CONTENT, (
		f"/apis should be static const; expected 2.05, got {resp.code}"
	)
