"""Functional cases for the CONFIG_NET_CONNECTION_MANAGER start/stop path.

With the connection manager enabled, `zlet_coap_service` declares no
autostart flag: `zlet_coap_on_l4_event` (zephlet_coap_frontend.c) starts it
on `NET_EVENT_L4_CONNECTED` and stops it on `NET_EVENT_L4_DISCONNECTED`.
This test case's `prj.conf` enables the native-sim offloaded-sockets
driver's connectivity-sim binding, which turns the stock `net cm
connect`/`net cm disconnect` shell commands into real L4 connectivity
events on the DUT's default interface — the same event bus a real
ESP-AT/Wi-Fi link would drive, no fake iface or extra driver code needed.

This test case also runs with `CONFIG_ZEPHLETS_COAP_MAX_OBSERVERS=1`, so a
single Observe subscriber leaked across a disconnect is enough to exhaust
the pool on the very next cycle.
"""

from __future__ import annotations

import pytest
from aiocoap import GET, CONTENT, Context, Message
from twister_harness import DeviceAdapter, Shell

from conftest import _events_uri, _probe_once, _wait_until_reachable

READY_TIMEOUT_S = 10.0
UNREACHABLE_TIMEOUT_S = 5.0


async def _subscribe(ctx: Context, host: str, port: int, instance: str = "tick_fast"):
	req = Message(code=GET, uri=_events_uri(host, port, instance), observe=0)
	requester = ctx.request(req)
	ack = await requester.response
	if ack.code == CONTENT:
		requester.observation.cancel()
	return ack


@pytest.mark.asyncio
async def test_l4_connect_starts_and_disconnect_stops(dut: DeviceAdapter, coap_endpoint):
	host, port = coap_endpoint
	shell = Shell(dut)
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	ctx = await Context.create_client_context()
	try:
		# No autostart under CONFIG_NET_CONNECTION_MANAGER: the service must
		# stay down until the first L4 connect.
		assert not await _probe_once(ctx, host, port), "service bound before any L4 connect"

		shell.exec_command("net cm connect all")
		await _wait_until_reachable(ctx, host, port, want_up=True, timeout_s=READY_TIMEOUT_S)

		shell.exec_command("net cm disconnect all")
		await _wait_until_reachable(ctx, host, port, want_up=False, timeout_s=UNREACHABLE_TIMEOUT_S)
	finally:
		await ctx.shutdown()


@pytest.mark.asyncio
async def test_disconnect_purges_observers(dut: DeviceAdapter, coap_endpoint):
	host, port = coap_endpoint
	shell = Shell(dut)
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	ctx = await Context.create_client_context()
	try:
		for cycle in range(3):
			shell.exec_command("net cm connect all")
			await _wait_until_reachable(ctx, host, port, want_up=True, timeout_s=READY_TIMEOUT_S)

			ack = await _subscribe(ctx, host, port)
			assert ack.code == CONTENT, f"cycle {cycle}: subscribe got {ack.code}"

			shell.exec_command("net cm disconnect all")
			await _wait_until_reachable(ctx, host, port, want_up=False, timeout_s=UNREACHABLE_TIMEOUT_S)
	finally:
		await ctx.shutdown()
