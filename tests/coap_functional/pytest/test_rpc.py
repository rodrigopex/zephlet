"""Functional cases for the zephlet CoAP RPC dispatch path.

The Zephyr binary boots a `tick_fast` instance of the `tick` type and a
`ui_main` instance of the `ui` type, both opted into CoAP. The
codegen-emitted per-type resources sit at `/zlet/tick/#` and `/zlet/ui/#`.

The cases exercise:
  - the happy POST path with a non-empty response envelope (`tick.start`
    returns `Lifecycle.Status`),
  - five unhappy paths bracketing the routing + decode error map,
  - the GET stub that returns 4.05 until the events bridge lands.
"""

from __future__ import annotations

import pytest
from aiocoap import GET, POST, BAD_REQUEST, CHANGED, CONTENT, METHOD_NOT_ALLOWED, NOT_FOUND, Message
from twister_harness import DeviceAdapter


def _post(host: str, port: int, path: str, payload: bytes = b"") -> Message:
	return Message(code=POST, uri=f"coap://{host}:{port}/{path}", payload=payload)


def _get(host: str, port: int, path: str) -> Message:
	return Message(code=GET, uri=f"coap://{host}:{port}/{path}")


@pytest.mark.asyncio
async def test_happy_post_returns_2_05(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	req = _post(host, port, "zlet/tick/tick_fast/start")
	resp = await aiocoap_client.request(req).response
	assert resp.code in (CHANGED, CONTENT), f"expected 2.04/2.05, got {resp.code}"
	# tick.start returns a non-empty Lifecycle.Status, so the dispatcher
	# always upgrades to 2.05 Content when encode produces bytes.
	assert resp.code == CONTENT, f"expected 2.05 Content, got {resp.code}"
	assert resp.payload, "expected a non-empty protobuf payload"


@pytest.mark.asyncio
async def test_unknown_type_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	req = _post(host, port, "zlet/zoo/tick_fast/start")
	resp = await aiocoap_client.request(req).response
	assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"


@pytest.mark.asyncio
async def test_unknown_instance_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	req = _post(host, port, "zlet/tick/no_such/start")
	resp = await aiocoap_client.request(req).response
	assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"


@pytest.mark.asyncio
async def test_api_mismatch_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	# /zlet/ui/tick_fast/start: ui resource matches, ui handler resolves
	# instance "tick_fast" (a tick), api check rejects.
	req = _post(host, port, "zlet/ui/tick_fast/start")
	resp = await aiocoap_client.request(req).response
	assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"


@pytest.mark.asyncio
async def test_unknown_method_returns_4_05(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	req = _post(host, port, "zlet/tick/tick_fast/dance")
	resp = await aiocoap_client.request(req).response
	assert resp.code == METHOD_NOT_ALLOWED, f"expected 4.05, got {resp.code}"


@pytest.mark.asyncio
async def test_malformed_body_returns_4_00(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	# tick.config decodes into struct tick_config (two uint32 fields).
	# An all-0xFF body starts an infinite-continuation varint that nanopb
	# rejects with a decode error.
	req = _post(host, port, "zlet/tick/tick_fast/config", payload=b"\xff" * 10)
	resp = await aiocoap_client.request(req).response
	assert resp.code == BAD_REQUEST, f"expected 4.00, got {resp.code}"


@pytest.mark.asyncio
async def test_oversized_segment_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	# CONFIG_ZEPHLETS_COAP_MAX_SEGMENT_LEN default = 32. A 33-char
	# instance segment overflows the handler's stack copy buffer.
	long_instance = "x" * 33
	req = _post(host, port, f"zlet/tick/{long_instance}/start")
	resp = await aiocoap_client.request(req).response
	assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"


@pytest.mark.asyncio
async def test_get_returns_4_05(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	req = _get(host, port, "zlet/tick/tick_fast/events")
	resp = await aiocoap_client.request(req).response
	assert resp.code == METHOD_NOT_ALLOWED, f"expected 4.05, got {resp.code}"
