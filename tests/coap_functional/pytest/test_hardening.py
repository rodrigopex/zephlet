"""Hardening negatives for the zephlet CoAP RPC dispatch path.

These cases bracket the malformed/oversized input edges the generated
POST handler must reject *before* dispatch. They complement the routing
and decode cases in `test_rpc.py`; nothing here overlaps with those.

The binary boots a `tick_fast` instance of the `tick` type (CoAP opt-in).
`tick.config` decodes into `Tick.Config` (two uint32 fields), whose nanopb
envelope `TICK_CONFIG_SIZE` is 12 bytes; `tick.start` is an empty-request
method (`req_max_size == 0`). Response-code expectations:

  - body larger than the method envelope                     -> 4.13
  - any body on an empty-request method                      -> 4.13
  - decodable-size body that fails nanopb decode             -> 4.00
  - method segment longer than MAX_SEGMENT_LEN (32)          -> 4.04
  - a path with more than four segments                      -> 4.04
  - an RPC path driven with an unsupported CoAP method       -> 4.05
"""

from __future__ import annotations

import pytest
from aiocoap import (
    DELETE,
    POST,
    PUT,
    BAD_REQUEST,
    METHOD_NOT_ALLOWED,
    NOT_FOUND,
    REQUEST_ENTITY_TOO_LARGE,
    Message,
)
from twister_harness import DeviceAdapter


def _msg(code, host: str, port: int, path: str, payload: bytes = b"") -> Message:
    return Message(code=code, uri=f"coap://{host}:{port}/{path}", payload=payload)


@pytest.mark.asyncio
async def test_oversized_body_returns_4_13(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
    host, port = coap_endpoint
    # 64 bytes overflows TICK_CONFIG_SIZE (12); the handler must reject on
    # the size bound before nanopb ever sees the stream. Without the bound
    # this body would surface 4.00 from the decode failure instead.
    req = _msg(POST, host, port, "zlet/tick/tick_fast/config", payload=b"\x00" * 64)
    resp = await aiocoap_client.request(req).response
    assert resp.code == REQUEST_ENTITY_TOO_LARGE, f"expected 4.13, got {resp.code}"


@pytest.mark.asyncio
async def test_body_on_empty_method_returns_4_13(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
    host, port = coap_endpoint
    # tick.start takes Empty (req_max_size == 0); any payload is malformed.
    req = _msg(POST, host, port, "zlet/tick/tick_fast/start", payload=b"\x01")
    resp = await aiocoap_client.request(req).response
    assert resp.code == REQUEST_ENTITY_TOO_LARGE, f"expected 4.13, got {resp.code}"


@pytest.mark.asyncio
async def test_truncated_varint_returns_4_00(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
    host, port = coap_endpoint
    # 0x08 is the field-1 varint tag; the trailing 0xff sets the varint
    # continuation bit with no following byte, so nanopb fails the decode.
    # Two bytes stay under TICK_CONFIG_SIZE, so this exercises decode, not
    # the size bound.
    req = _msg(POST, host, port, "zlet/tick/tick_fast/config", payload=b"\x08\xff")
    resp = await aiocoap_client.request(req).response
    assert resp.code == BAD_REQUEST, f"expected 4.00, got {resp.code}"


@pytest.mark.asyncio
async def test_oversized_method_segment_returns_4_04(
    dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
    host, port = coap_endpoint
    # MAX_SEGMENT_LEN default = 32; a 33-char method segment overflows the
    # resolve buffer, so the URI never resolves to a method.
    long_method = "y" * 33
    req = _msg(POST, host, port, f"zlet/tick/tick_fast/{long_method}")
    resp = await aiocoap_client.request(req).response
    assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"


@pytest.mark.asyncio
async def test_too_many_segments_returns_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
    host, port = coap_endpoint
    # resolve requires exactly four path segments (zlet/<type>/<inst>/<m>).
    req = _msg(POST, host, port, "zlet/tick/tick_fast/start/extra")
    resp = await aiocoap_client.request(req).response
    assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"


@pytest.mark.asyncio
async def test_put_returns_4_05(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
    host, port = coap_endpoint
    # The resource registers .post and .get only; PUT has no handler.
    req = _msg(PUT, host, port, "zlet/tick/tick_fast/start")
    resp = await aiocoap_client.request(req).response
    assert resp.code == METHOD_NOT_ALLOWED, f"expected 4.05, got {resp.code}"


@pytest.mark.asyncio
async def test_delete_returns_4_05(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
    host, port = coap_endpoint
    # Same as PUT: no .delete handler registered on the resource.
    req = _msg(DELETE, host, port, "zlet/tick/tick_fast/start")
    resp = await aiocoap_client.request(req).response
    assert resp.code == METHOD_NOT_ALLOWED, f"expected 4.05, got {resp.code}"
