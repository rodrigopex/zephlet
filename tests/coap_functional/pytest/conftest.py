"""Pytest fixtures for the zephlet CoAP functional harness.

Twister runs the Zephyr binary on `native_sim` with `eth_native_tap`;
the host-side TAP endpoint reaches the guest at `192.0.2.1:5683`. This
fixture provides an `aiocoap` client targeting that endpoint, with a
startup wait so the guest has time to bind the port.
"""

from __future__ import annotations

import asyncio
import logging
import os

import pytest
import pytest_asyncio
from aiocoap import GET, Context, Message
from aiocoap.error import NetworkError

COAP_HOST = os.environ.get("ZEPHLET_COAP_HOST", "192.0.2.1")
COAP_PORT = int(os.environ.get("ZEPHLET_COAP_PORT", "5683"))
READY_TIMEOUT_S = float(os.environ.get("ZEPHLET_COAP_READY_TIMEOUT_S", "20"))
READY_POLL_INTERVAL_S = 0.5

log = logging.getLogger("zlet.coap.fixture")


async def _wait_for_server(ctx: Context, host: str, port: int) -> None:
	deadline = asyncio.get_event_loop().time() + READY_TIMEOUT_S
	probe_uri = f"coap://{host}:{port}/_zlet_ready_probe"
	while asyncio.get_event_loop().time() < deadline:
		try:
			req = Message(code=GET, uri=probe_uri)
			await asyncio.wait_for(
				ctx.request(req).response,
				timeout=READY_POLL_INTERVAL_S,
			)
			return
		except NetworkError:
			pass
		except (asyncio.TimeoutError, OSError):
			pass
		await asyncio.sleep(READY_POLL_INTERVAL_S)
	raise TimeoutError(
		f"CoAP server at {host}:{port} not reachable within {READY_TIMEOUT_S}s"
	)


@pytest.fixture(scope="session")
def coap_endpoint() -> tuple[str, int]:
	return COAP_HOST, COAP_PORT


@pytest_asyncio.fixture(scope="function")
async def aiocoap_client(coap_endpoint):
	host, port = coap_endpoint
	ctx = await Context.create_client_context()
	try:
		await _wait_for_server(ctx, host, port)
		yield ctx
	finally:
		await ctx.shutdown()
		await asyncio.sleep(0)
