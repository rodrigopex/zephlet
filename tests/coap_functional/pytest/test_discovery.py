"""Functional cases for the per-type discovery resources.

For every zephlet type whose service declares
`option (zephlet.coap_discoverable) = true;`, the codegen emits two
extra CoAP resources next to the existing `/zlet/<type>/#` wildcard:

  - `GET /zlet/<type>/instances`  → RFC 6690 link-format listing each
    instance via `rt="zlet.instance"`. Walks the section iterable at
    request time.
  - `GET /zlet/<type>/apis`       → RFC 6690 link-format listing every
    rpc method; base lifecycle methods carry `rt="zlet.rpc.base"`,
    custom methods carry `rt="zlet.rpc"`. Fully codegen-baked.

The test app boots `tick_fast` + `ui_main` (both discoverable) and
`silent_inst` (coap-exposed but NOT discoverable). Discovery requests
against `silent` must return 4.04 because no instances/apis resources
were registered for it.

Zephyr's built-in `/.well-known/core` handler is unchanged and lists
every registered resource, so a client first hits `.well-known/core` to
find the per-type discovery endpoints, then drills down.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

import pytest
import pytest_asyncio
from aiocoap import GET, POST, CONTENT, NOT_FOUND, Message
from twister_harness import DeviceAdapter


CT_LINK_FORMAT = 40
CT_TEXT_PLAIN = 0
RT_RPC = "zlet.rpc"
BASE_METHODS = ("start", "stop", "get_status", "config", "get_config")


@dataclass(frozen=True)
class Link:
	path: str
	attrs: dict[str, str | bool]


def _parse_link_format(body: bytes) -> list[Link]:
	"""Minimal RFC 6690 link-format splitter — handles only the attributes
	the codegen emits (`rt`, `ct`)."""
	text = body.decode("ascii")
	links: list[Link] = []
	for chunk in re.split(r",(?=<)", text):
		chunk = chunk.strip()
		if not chunk:
			continue
		head, _, tail = chunk.partition(">")
		assert head.startswith("<"), f"malformed link: {chunk!r}"
		path = head[1:]
		attrs: dict[str, str | bool] = {}
		for part in (p for p in tail.split(";") if p):
			if "=" in part:
				k, v = part.split("=", 1)
				attrs[k] = v.strip('"')
			else:
				attrs[part] = True
		links.append(Link(path=path, attrs=attrs))
	return links


async def _get_links(client, host: str, port: int, path: str) -> list[Link]:
	req = Message(code=GET, uri=f"coap://{host}:{port}/{path}")
	resp = await client.request(req).response
	assert resp.code == CONTENT, f"GET /{path}: expected 2.05, got {resp.code}"
	assert resp.opt.content_format == CT_LINK_FORMAT, (
		f"GET /{path}: expected ct={CT_LINK_FORMAT}, got {resp.opt.content_format}"
	)
	return _parse_link_format(resp.payload)


async def _get_plain_text(client, host: str, port: int, path: str) -> tuple[str, ...]:
	req = Message(code=GET, uri=f"coap://{host}:{port}/{path}")
	resp = await client.request(req).response
	assert resp.code == CONTENT, f"GET /{path}: expected 2.05, got {resp.code}"
	assert resp.opt.content_format == CT_TEXT_PLAIN, (
		f"GET /{path}: expected ct={CT_TEXT_PLAIN}, got {resp.opt.content_format}"
	)
	return tuple(resp.payload.decode("ascii").split("\n"))


@pytest_asyncio.fixture(scope="function")
async def tick_instances(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	return await _get_plain_text(aiocoap_client, host, port, "zlet/tick/instances")


@pytest.mark.asyncio
async def test_tick_instances_lists_every_live_instance(tick_instances):
	# Two `tick` instances are wired in main.c (`tick_fast`, `tick_slow`);
	# both must appear so the runtime section walk is exercised. Wire
	# format is plain text — one instance name per line, no trailing
	# newline.
	assert set(tick_instances) == {"tick_fast", "tick_slow"}, tick_instances


@pytest.mark.asyncio
async def test_base_apis_lists_lifecycle_methods_only(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	"""The shared `/zlet/apis` resource is the single advertisement of
	the base lifecycle method names. Plain text, one name per line, in
	the order declared by `LifecycleApi`."""
	host, port = coap_endpoint
	req = Message(code=GET, uri=f"coap://{host}:{port}/zlet/apis")
	resp = await aiocoap_client.request(req).response
	assert resp.code == CONTENT, resp.code
	assert resp.opt.content_format == CT_TEXT_PLAIN, resp.opt.content_format
	lines = tuple(resp.payload.decode("ascii").split("\n"))
	assert lines == BASE_METHODS, lines


@pytest.mark.asyncio
async def test_tick_apis_lists_only_customs(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	"""`tick` declares two custom rpcs (`dump_state`, `kick`); its
	`/apis` resource exposes both, never the base methods (those live
	on `/zlet/apis`). The two-entry blob also stresses the codegen's
	comma separator between adjacent link literals."""
	host, port = coap_endpoint
	links = await _get_links(aiocoap_client, host, port, "zlet/tick/apis")
	paths = {l.path for l in links}
	assert paths == {"/zlet/tick/dump_state", "/zlet/tick/kick"}, paths
	for link in links:
		assert link.attrs.get("rt") == RT_RPC
		assert link.attrs.get("ct") == "65001"
		segment = link.path.rsplit("/", 1)[-1]
		assert segment not in BASE_METHODS, (
			f"base method {segment} leaked into /apis: {link}"
		)


@pytest.mark.asyncio
async def test_ui_apis_not_registered_when_only_base(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	"""`ui` declares only base methods, so its `/apis` resource is not
	registered. The request falls through to the wildcard `/zlet/ui/#`
	whose GET stub returns 4.05."""
	host, port = coap_endpoint
	req = Message(code=GET, uri=f"coap://{host}:{port}/zlet/ui/apis")
	resp = await aiocoap_client.request(req).response
	assert 0x80 <= resp.code < 0xa0, (
		f"GET /zlet/ui/apis: expected 4.xx, got {resp.code}"
	)


@pytest.mark.asyncio
async def test_ui_instances_listed(dut: DeviceAdapter, aiocoap_client, coap_endpoint):
	host, port = coap_endpoint
	names = await _get_plain_text(aiocoap_client, host, port, "zlet/ui/instances")
	assert set(names) == {"ui_main"}, names


@pytest.mark.asyncio
async def test_silent_has_no_discovery_resources(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	host, port = coap_endpoint
	for path in ("zlet/silent/instances", "zlet/silent/apis"):
		req = Message(code=GET, uri=f"coap://{host}:{port}/{path}")
		resp = await aiocoap_client.request(req).response
		# silent declares (zephlet.coap)=true but NOT discoverable, so
		# neither resource was registered. Falls through the wildcard
		# to the per-type GET stub which surfaces -ENOSYS → 4.05.
		# (Either 4.04 or 4.05 are valid; assert "client error".)
		assert 0x80 <= resp.code < 0xa0, (
			f"GET /{path}: expected 4.xx client error, got {resp.code}"
		)


@pytest.mark.asyncio
async def test_hidden_silent_is_still_rpc_callable(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	"""Hiding a type from discovery must not break its RPC dispatch.
	`silent` is invisible in `/.well-known/core` and has no
	`/instances` or `/apis` resources, but clients that already know
	the URI can still POST to it. The weak-default impl returns
	-ENOSYS, which the dispatcher maps to 4.05 — proving the wildcard
	resource is still wired."""
	host, port = coap_endpoint
	req = Message(
		code=POST,
		uri=f"coap://{host}:{port}/zlet/silent/silent_inst/start",
	)
	resp = await aiocoap_client.request(req).response
	# -ENOSYS → 4.05 Method Not Allowed (per zephlet_coap_map_return_code).
	# The wildcard is alive; it just delegates to a stub.
	assert 0x80 <= resp.code < 0xa0, (
		f"POST /zlet/silent/silent_inst/start: expected 4.xx, got {resp.code}"
	)


@pytest.mark.asyncio
async def test_wellknown_core_lists_discoverable_only(
	dut: DeviceAdapter, aiocoap_client, coap_endpoint
):
	"""The custom `/.well-known/core` handler delegates to Zephyr's
	formatter after filtering out resources tagged `ZEPHLET_COAP_HIDDEN`
	(the wildcards of non-discoverable services). Discoverable types'
	`/instances` and `/apis` resources must appear; the `silent` type's
	wildcard must not."""
	host, port = coap_endpoint
	req = Message(code=GET, uri=f"coap://{host}:{port}/.well-known/core")
	resp = await aiocoap_client.request(req).response
	assert resp.code == CONTENT, resp.code
	body = resp.payload.decode("ascii")
	for path in ("/zlet/tick/instances", "/zlet/tick/apis",
		     "/zlet/ui/instances",
		     "/zlet/apis"):
		assert path in body, f"{path} missing from /.well-known/core: {body!r}"
	# `ui` has no custom methods → its /apis resource is NOT registered.
	assert "/zlet/ui/apis" not in body, (
		f"ui/apis surfaced despite no customs: {body!r}"
	)
	# silent wildcard is tagged hidden — must not leak.
	assert "/zlet/silent" not in body, (
		f"hidden silent wildcard leaked into /.well-known/core: {body!r}"
	)
