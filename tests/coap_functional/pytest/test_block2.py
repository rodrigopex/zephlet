"""Block-wise responses (RFC 7959 Block2), built with blocks of 16 bytes.

The `zephlet.coap_functional.block2` scenario sets
`CONFIG_ZEPHLETS_COAP_BLOCK2=y`, `CONFIG_ZEPHLETS_COAP_BLOCK2_SIZE=16` and
`CONFIG_ZEPHLET_BLOB=y`. `blob_inst.read` returns 120 bytes 0, 1, 2, ...
and the number of times the rpc ran; its encoding is 124 bytes, 8 blocks.

The cases exercise:
  - an rpc read whole by aiocoap's block-wise client runs the rpc once,
  - the blocks themselves: Block2, Size2 and one ETag across a transfer,
  - a later block with no kept response (4.08) and past the end (4.02),
  - discovery bodies longer than a block: /zlet/apis, /.well-known/core.
"""

from __future__ import annotations

import pytest
from aiocoap import (
	BAD_OPTION,
	CONTENT,
	GET,
	POST,
	REQUEST_ENTITY_INCOMPLETE,
	Context,
	Message,
)
from aiocoap.optiontypes import BlockOption
from twister_harness import DeviceAdapter

BLOB = "zlet/blob/blob_inst/read"
SIZE = 120
ENCODED = 2 + SIZE + 2  # field 1 tag and length, the bytes, field 2 tag and runs
SZX_16 = 0


def _decode_chunk(payload: bytes) -> tuple[bytes, int]:
	"""Blob.Chunk: field 1 bytes (one-byte length), field 2 varint runs."""
	assert payload[0] == 0x0A, payload[:4]
	n = payload[1]
	data = payload[2 : 2 + n]
	rest = payload[2 + n :]
	assert rest[0] == 0x10, rest
	return data, rest[1]


def _block(host: str, port: int, path: str, num: int, code=POST) -> Message:
	msg = Message(code=code, uri=f"coap://{host}:{port}/{path}")
	msg.opt.block2 = BlockOption.BlockwiseTuple(num, False, SZX_16)
	return msg


@pytest.mark.asyncio
async def test_blockwise_read_runs_the_rpc_once(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	runs = []
	for _ in range(2):
		req = Message(code=POST, uri=f"coap://{host}:{port}/{BLOB}")
		resp = await aiocoap_client.request(req).response
		assert resp.code == CONTENT, resp.code
		data, n = _decode_chunk(resp.payload)
		assert data == bytes(range(SIZE))
		runs.append(n)
	# one run per transfer, not one per block
	assert runs[1] == runs[0] + 1, runs


@pytest.mark.asyncio
async def test_blocks_carry_block2_size2_and_one_etag(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	body = b""
	etags = set()
	num = 0
	while True:
		resp = await aiocoap_client.request(
			_block(host, port, BLOB, num), handle_blockwise=False
		).response
		assert resp.code == CONTENT, (num, resp.code)
		b2 = resp.opt.block2
		assert (b2.block_number, b2.size_exponent) == (num, SZX_16)
		assert resp.opt.size2 == ENCODED
		etags.add(bytes(resp.opt.etag))
		body += resp.payload
		if not b2.more:
			break
		assert len(resp.payload) == 16
		num += 1
	assert num == (ENCODED - 1) // 16
	assert len(etags) == 1
	assert _decode_chunk(body)[0] == bytes(range(SIZE))


@pytest.mark.asyncio
async def test_block_past_the_end_is_4_02(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	first = await aiocoap_client.request(_block(host, port, BLOB, 0), handle_blockwise=False).response
	assert first.opt.block2.more
	resp = await aiocoap_client.request(_block(host, port, BLOB, 40), handle_blockwise=False).response
	assert resp.code == BAD_OPTION, resp.code


@pytest.mark.asyncio
async def test_later_block_without_kept_response_is_4_08(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	# a client of its own: no transfer of this peer reached block 0
	ctx = await Context.create_client_context()
	try:
		resp = await ctx.request(_block(host, port, BLOB, 3), handle_blockwise=False).response
		assert resp.code == REQUEST_ENTITY_INCOMPLETE, resp.code
	finally:
		await ctx.shutdown()


@pytest.mark.asyncio
async def test_discovery_bodies_longer_than_a_block(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	resp = await aiocoap_client.request(Message(code=GET, uri=f"coap://{host}:{port}/zlet/apis")).response
	assert resp.code == CONTENT
	assert resp.payload == b"start\nstop\nget_status\nconfig\nget_config"

	first = await aiocoap_client.request(_block(host, port, "zlet/apis", 0, GET), handle_blockwise=False).response
	assert first.opt.block2.more
	assert len(first.payload) == 16

	resp = await aiocoap_client.request(
		Message(code=GET, uri=f"coap://{host}:{port}/.well-known/core")
	).response
	assert resp.code == CONTENT
	assert b"</zlet/tick/instances>" in resp.payload
