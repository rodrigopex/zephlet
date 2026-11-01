"""Pytest fixtures for the zephlet CoAP functional harness.

The Zephyr binary runs on `native_sim` with the native-simulator
offloaded sockets driver (`CONFIG_NET_NATIVE_OFFLOADED_SOCKETS=y`),
so its `socket()`/`bind()` calls forward into the host BSD stack and
the CoAP server listens on a real host socket. The pytest fixture
reaches the guest at `127.0.0.1`, on the scenario's
`CONFIG_ZEPHLETS_COAP_PORT`, with no TAP setup, no root,
and no host-side networking config — works identically on bare
Linux, Docker, and Colima.
"""

from __future__ import annotations

import asyncio
import logging
import os
import re
from pathlib import Path

import pytest
import pytest_asyncio
from aiocoap import GET, Context, Message
from aiocoap.error import NetworkError

COAP_HOST = os.environ.get("ZEPHLET_COAP_HOST", "127.0.0.1")
COAP_PORT = int(os.environ.get("ZEPHLET_COAP_PORT", "5683"))
READY_TIMEOUT_S = float(os.environ.get("ZEPHLET_COAP_READY_TIMEOUT_S", "20"))
READY_POLL_INTERVAL_S = 0.5

log = logging.getLogger("zlet.coap.fixture")


async def _probe_once(ctx: Context, host: str, port: int, timeout_s: float = READY_POLL_INTERVAL_S) -> bool:
	"""Single reachability check: any response (even an error code) means
	the server is up; a network/timeout error means it isn't."""
	req = Message(code=GET, uri=f"coap://{host}:{port}/_zlet_ready_probe")
	try:
		await asyncio.wait_for(ctx.request(req).response, timeout=timeout_s)
		return True
	except (NetworkError, asyncio.TimeoutError, OSError):
		return False


async def _wait_until_reachable(
	ctx: Context, host: str, port: int, want_up: bool, timeout_s: float
) -> None:
	"""Poll _probe_once until it matches @want_up, or raise TimeoutError."""
	deadline = asyncio.get_event_loop().time() + timeout_s
	while asyncio.get_event_loop().time() < deadline:
		if await _probe_once(ctx, host, port) == want_up:
			return
		await asyncio.sleep(READY_POLL_INTERVAL_S)
	state = "reachable" if want_up else "unreachable"
	raise TimeoutError(f"CoAP server at {host}:{port} did not become {state} within {timeout_s}s")


async def _wait_for_server(ctx: Context, host: str, port: int) -> None:
	await _wait_until_reachable(ctx, host, port, want_up=True, timeout_s=READY_TIMEOUT_S)


def _events_uri(host: str, port: int, instance: str, type_name: str = "tick") -> str:
	return f"coap://{host}:{port}/zlet/{type_name}/{instance}/events"


def _build_dir(request) -> str | None:
	"""The DUT's build directory: from the harness config (newer twister),
	or from --build-dir (older twister)."""
	harness = getattr(request.config, "twister_harness_config", None)
	if harness is not None and harness.devices:
		cfg = harness.devices[0]
		for attr in ("app_build_dir", "build_dir"):
			if getattr(cfg, attr, None):
				return str(getattr(cfg, attr))
	try:
		return request.config.getoption("--build-dir")
	except ValueError:
		return None


def _configured_port(build_dir: str | None) -> int | None:
	"""CONFIG_ZEPHLETS_COAP_PORT of the DUT's build, None without one."""
	if not build_dir:
		return None
	cfg = Path(build_dir) / "zephyr" / ".config"
	if not cfg.exists():
		return None
	m = re.search(r"^CONFIG_ZEPHLETS_COAP_PORT=(\d+)$", cfg.read_text(), re.MULTILINE)
	return int(m.group(1)) if m else None


@pytest.fixture(scope="session")
def coap_endpoint(request) -> tuple[str, int]:
	"""ZEPHLET_COAP_PORT if set, else the DUT's CONFIG_ZEPHLETS_COAP_PORT,
	else 5683. Each scenario takes its own port, so twister runs them in
	parallel."""
	if "ZEPHLET_COAP_PORT" in os.environ:
		return COAP_HOST, COAP_PORT
	port = _configured_port(_build_dir(request))
	return COAP_HOST, port if port is not None else COAP_PORT


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
