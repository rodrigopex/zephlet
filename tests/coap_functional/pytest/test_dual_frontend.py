"""Proves `_ZLET_FRONTEND_HOOKS_tick` fires both `_ZLET_COAP_HOOK_tick`
and `_ZLET_SHELL_HOOK_tick` for the *same* `tick_fast` instance, and that
both frontends dispatch onto the same live zephlet, not two independent
copies: a `tick_config` write through one frontend must read back through
the other.

`tick_fast`'s handler (`tick_config_impl`, in the app's `zlet_tick.c`)
mutates `z->config` in place — a write via CoAP and a read via shell (or
vice versa) only agree if both frontends hit the same `struct zephlet`
instance and the same underlying `struct tick_config` storage.

Minimal hand-rolled protobuf codec for `Tick.Config` (two `uint32`
fields, STATIC allocation, ascending tag order — the same wire shape
`test_hardening.py` already hardcodes tag bytes against): no nanopb
Python bindings are generated for this test, and the shape is fixed and
tiny enough not to need them.
"""

from __future__ import annotations

import pytest
from aiocoap import POST, CONTENT, Message
from twister_harness import DeviceAdapter, Shell


def _varint(n: int) -> bytes:
	out = bytearray()
	while True:
		b = n & 0x7F
		n >>= 7
		if n:
			out.append(b | 0x80)
		else:
			out.append(b)
			return bytes(out)


def _read_varint(buf: bytes, i: int) -> tuple[int, int]:
	result = 0
	shift = 0
	while True:
		b = buf[i]
		i += 1
		result |= (b & 0x7F) << shift
		if not (b & 0x80):
			return result, i
		shift += 7


def _encode_tick_config(duration_ms: int, period_ms: int) -> bytes:
	return b"\x08" + _varint(duration_ms) + b"\x10" + _varint(period_ms)


def _decode_tick_config(payload: bytes) -> tuple[int, int]:
	assert payload[0] == 0x08, payload
	duration_ms, i = _read_varint(payload, 1)
	assert payload[i] == 0x10, payload
	period_ms, _ = _read_varint(payload, i + 1)
	return duration_ms, period_ms


async def _coap_config_set(aiocoap_client, host, port, duration_ms, period_ms):
	req = Message(code=POST, uri=f"coap://{host}:{port}/zlet/tick/tick_fast/config",
		       payload=_encode_tick_config(duration_ms, period_ms))
	resp = await aiocoap_client.request(req).response
	assert resp.code == CONTENT, f"expected 2.05, got {resp.code}"


async def _coap_config_get(aiocoap_client, host, port) -> tuple[int, int]:
	req = Message(code=POST, uri=f"coap://{host}:{port}/zlet/tick/tick_fast/get_config")
	resp = await aiocoap_client.request(req).response
	assert resp.code == CONTENT, f"expected 2.05, got {resp.code}"
	return _decode_tick_config(resp.payload)


def _shell_config_set(shell: Shell, duration_ms: int, period_ms: int) -> None:
	shell.exec_command(f"zlet tick_fast config {duration_ms} {period_ms}")


def _shell_config_get(shell: Shell) -> str:
	lines = shell.exec_command("zlet tick_fast get_config")
	return "\n".join(shell.get_filtered_output(lines))


@pytest.mark.asyncio
async def test_shell_write_visible_over_coap(dut: DeviceAdapter, shell: Shell, aiocoap_client,
					       coap_endpoint):
	assert shell.wait_for_prompt(), "shell prompt never appeared"
	host, port = coap_endpoint

	_shell_config_set(shell, 111, 111)

	duration_ms, period_ms = await _coap_config_get(aiocoap_client, host, port)
	assert (duration_ms, period_ms) == (111, 111)


@pytest.mark.asyncio
async def test_coap_write_visible_over_shell(dut: DeviceAdapter, shell: Shell, aiocoap_client,
					       coap_endpoint):
	assert shell.wait_for_prompt(), "shell prompt never appeared"
	host, port = coap_endpoint

	await _coap_config_set(aiocoap_client, host, port, 77, 77)

	out = _shell_config_get(shell)
	assert "duration_ms = 77" in out, out
	assert "period_ms = 77" in out, out
