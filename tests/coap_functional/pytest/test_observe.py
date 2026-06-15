"""Functional cases for the zephlet CoAP Observe (event bridge) path.

`tick_fast` (period 100 ms) emits a `Tick.Events { timestamp }` on every
tick once started. Each notification carries the per-instance Observe
sequence number, which advances once per event — so the values a
subscriber receives are strictly increasing and, absent loss, contiguous.
A gap would mark a missed notification, which is the loss signal this
phase ships in place of a drop counter.
"""

from __future__ import annotations

import asyncio

import pytest
from aiocoap import GET, POST, CONTENT, NOT_FOUND, Message
from twister_harness import DeviceAdapter


def _post(host: str, port: int, path: str, payload: bytes = b"") -> Message:
	return Message(code=POST, uri=f"coap://{host}:{port}/{path}", payload=payload)


def _events_uri(host: str, port: int, instance: str) -> str:
	return f"coap://{host}:{port}/zlet/tick/{instance}/events"


async def _collect_observe(observation, count: int) -> list[int]:
	seqs: list[int] = []
	async for notification in observation:
		assert notification.opt.observe is not None, "notification missing Observe option"
		seqs.append(notification.opt.observe)
		if len(seqs) >= count:
			break
	return seqs


@pytest.mark.asyncio
async def test_observe_registration_acked(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	req = Message(code=GET, uri=_events_uri(host, port, "tick_fast"), observe=0)
	requester = aiocoap_client.request(req)
	ack = await requester.response
	requester.observation.cancel()
	assert ack.code == CONTENT, f"expected 2.05 Content, got {ack.code}"
	assert ack.opt.observe is not None, "registration ack must carry an Observe option"


@pytest.mark.asyncio
async def test_observe_delivers_monotonic_sequence(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	host, port = coap_endpoint
	# tick must be running to emit. start is effectively idempotent here:
	# an already-running instance just answers 4.09, which we ignore.
	await aiocoap_client.request(_post(host, port, "zlet/tick/tick_fast/start")).response

	req = Message(code=GET, uri=_events_uri(host, port, "tick_fast"), observe=0)
	requester = aiocoap_client.request(req)
	ack = await requester.response
	assert ack.code == CONTENT, f"expected 2.05 Content, got {ack.code}"

	seqs = await asyncio.wait_for(_collect_observe(requester.observation, 4), timeout=15)
	requester.observation.cancel()

	assert len(seqs) >= 4, f"too few notifications: {seqs}"
	diffs = [b - a for a, b in zip(seqs, seqs[1:])]
	# Strictly increasing is the RFC 7641 ordering guarantee; every diff == 1
	# is what "no loss" looks like. A gap here would be a dropped
	# notification — the signal this phase relies on.
	assert all(d >= 1 for d in diffs), f"sequence not strictly increasing: {seqs}"
	assert all(d == 1 for d in diffs), f"sequence gap (lost notification?): {seqs}"


@pytest.mark.asyncio
async def test_observe_unknown_instance_4_04(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	# Instance resolution runs before Observe handling, so an unknown
	# instance is rejected 4.04 — a plain GET is enough to exercise it.
	req = Message(code=GET, uri=_events_uri(host, port, "no_such"))
	resp = await aiocoap_client.request(req).response
	assert resp.code == NOT_FOUND, f"expected 4.04, got {resp.code}"
