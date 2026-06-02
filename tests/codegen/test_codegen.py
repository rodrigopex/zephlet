"""Codegen tests for `generate_zephlet.py`.

Run with `pytest modules/lib/zephlet/tests/codegen/` from any directory
that has `proto_schema_parser` and `jinja2` on the path (the repo's
`zephyr` venv satisfies both).

Coverage:
  - opt-in detection over the `proto_schema_parser` AST (positive,
    negative, and the free property that commented-out options are
    invisible to the parser);
  - non-opted-in zephlet emits aggregator + default-empty hook in
    `_interface.h`, an empty stub for `_coap_interface.{h,c}`, and a
    `_interface.c` that is bit-identical to the pre-CoAP baseline;
  - opted-in zephlet emits the `#include` to the coap header, a
    populated `_coap_interface.h` (under `#ifdef CONFIG_ZEPHLETS_COAP`)
    and a `_coap_interface.c` with the per-method table + section
    iterable + cb stub;
  - the `_interface.c` is invariant across the opt-in toggle (Phase 1
    acceptance: pre-CoAP regression diff is zero).
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest
from proto_schema_parser.parser import Parser


_ZEPHLET_ROOT = Path(__file__).resolve().parents[2]
_CODEGEN = _ZEPHLET_ROOT / "codegen" / "generate_zephlet.py"
_FIXTURES = Path(__file__).resolve().parent / "fixtures"


@pytest.fixture(scope="session")
def codegen_module():
	"""Import `generate_zephlet` as a module for unit-testing helpers."""
	spec = importlib.util.spec_from_file_location(
		"generate_zephlet", str(_CODEGEN))
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def _run_codegen(proto: Path, out_dir: Path, *, type_name: str = "tick",
		 prefix: str = "zlet_tick") -> None:
	subprocess.run(
		[sys.executable, str(_CODEGEN),
		 "--proto", str(proto),
		 "--output-dir", str(out_dir),
		 "--type", type_name,
		 "--prefix", prefix],
		check=True,
		capture_output=True)


def _tree(path: Path):
	"""Parse a proto fixture into a `proto_schema_parser` AST."""
	return Parser().parse(path.read_text())


def test_detect_opt_in_positive(codegen_module):
	assert codegen_module.detect_coap_opt_in(
		_tree(_FIXTURES / "tick_opted.proto")) is True


def test_detect_opt_in_negative(codegen_module):
	assert codegen_module.detect_coap_opt_in(
		_tree(_FIXTURES / "tick_no_opt.proto")) is False


def test_detect_opt_in_ignores_comments(codegen_module):
	"""The AST never surfaces commented-out options, so the opt-in
	detector inherits that immunity for free."""
	assert codegen_module.detect_coap_opt_in(
		_tree(_FIXTURES / "tick_commented_opt.proto")) is False


def test_no_opt_in_emits_empty_stubs(tmp_path):
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)

	header = (tmp_path / "zlet_tick_interface.h").read_text()
	assert "_ZLET_FRONTEND_HOOKS_tick(_name)" in header
	assert "_ZLET_COAP_HOOK_tick(_name)" in header
	assert "#ifndef _ZLET_COAP_HOOK_tick" in header
	assert '#include "zlet_tick_coap_interface.h"' not in header

	coap_h = (tmp_path / "zlet_tick_coap_interface.h").read_text()
	assert "GENERATED_ZLET_TICK_COAP_INTERFACE_H_" in coap_h
	assert "#ifdef CONFIG_ZEPHLETS_COAP" not in coap_h
	assert "_tick_coap_event_cb" not in coap_h

	coap_c = (tmp_path / "zlet_tick_coap_interface.c").read_text()
	assert "STRUCT_SECTION_ITERABLE" not in coap_c
	assert "_tick_coap_event_cb" not in coap_c


def test_opt_in_emits_full_coap_interface(tmp_path):
	_run_codegen(_FIXTURES / "tick_opted.proto", tmp_path)

	header = (tmp_path / "zlet_tick_interface.h").read_text()
	assert '#include "zlet_tick_coap_interface.h"' in header
	assert "#ifndef _ZLET_COAP_HOOK_tick" in header

	coap_h = (tmp_path / "zlet_tick_coap_interface.h").read_text()
	assert "#ifdef CONFIG_ZEPHLETS_COAP" in coap_h
	# Header is included before the default-empty hook in
	# `_interface.h`, so a bare `#define` wins — no `#undef`
	# directive is needed. (Comments mentioning the word are fine.)
	directive_lines = [
		line for line in coap_h.splitlines()
		if line.lstrip().startswith("#undef")
	]
	assert directive_lines == [], directive_lines
	assert "#define _ZLET_COAP_HOOK_tick(_name)" in coap_h
	assert ("ZEPHLET_EVENTS_LISTENER(_name, tick, _tick_coap_event_cb)"
		in coap_h)
	assert "void _tick_coap_event_cb(" in coap_h

	# Include order in `_interface.h`: the coap header MUST appear
	# before the `#ifndef`-guarded default-empty, otherwise the
	# override would be shadowed.
	header = (tmp_path / "zlet_tick_interface.h").read_text()
	include_pos = header.index('#include "zlet_tick_coap_interface.h"')
	default_pos = header.index("#ifndef _ZLET_COAP_HOOK_tick")
	assert include_pos < default_pos

	coap_c = (tmp_path / "zlet_tick_coap_interface.c").read_text()
	assert "STRUCT_SECTION_ITERABLE(zephlet_coap_type, tick_coap_type)" \
		in coap_c
	assert 'type_name = "tick"' in coap_c
	assert ".api = &tick_api" in coap_c
	assert 'path_segment = "start"' in coap_c
	assert 'path_segment = "config"' in coap_c
	assert ".req_desc = &tick_config_t_msg" in coap_c
	assert "void _tick_coap_event_cb(" in coap_c

	# Opted into CoAP but NOT discoverable → the wildcard resource is
	# tagged `ZEPHLET_COAP_HIDDEN` so `/.well-known/core` skips it.
	assert "ZEPHLET_COAP_HIDDEN" in coap_c


def test_interface_c_invariant_across_opt_in(tmp_path):
	"""The core `_interface.c` must be byte-identical with vs. without the
	CoAP opt-in. Phase 1's hard regression guarantee."""
	dir_no = tmp_path / "no"
	dir_yes = tmp_path / "yes"
	dir_no.mkdir()
	dir_yes.mkdir()

	_run_codegen(_FIXTURES / "tick_no_opt.proto", dir_no)
	_run_codegen(_FIXTURES / "tick_opted.proto", dir_yes)

	assert ((dir_no / "zlet_tick_interface.c").read_bytes()
		== (dir_yes / "zlet_tick_interface.c").read_bytes())


def test_detect_discoverable_positive(codegen_module):
	assert codegen_module.detect_coap_discoverable_opt_in(
		_tree(_FIXTURES / "tick_discoverable_full.proto")) is True


def test_detect_discoverable_negative(codegen_module):
	assert codegen_module.detect_coap_discoverable_opt_in(
		_tree(_FIXTURES / "tick_opted.proto")) is False


def test_load_base_method_names(codegen_module):
	"""`zephlet.proto`'s LifecycleApi service is the single source of
	truth for the base method set."""
	base = codegen_module.load_base_method_names()
	assert set(base) == {"start", "stop", "get_status", "config",
			     "get_config"}


def test_discoverable_with_custom_emits_apis_listing_customs_only(tmp_path):
	"""A discoverable service with at least one custom rpc emits both
	`/instances` and `/apis`. The apis blob contains only the custom —
	base names are intentionally absent (the shared `/zlet/apis`
	resource serves them once)."""
	_run_codegen(_FIXTURES / "tick_discoverable_custom.proto", tmp_path)
	coap_c = (tmp_path / "zlet_tick_coap_interface.c").read_text()
	assert "tick_coap_instances_resource" in coap_c
	assert "tick_coap_apis_resource" in coap_c
	assert "dump_state" in coap_c
	# The base names must not appear inside the per-type apis blob.
	# (They will appear elsewhere in the file — the existing RPC
	# dispatcher still handles every method — so scope the check to
	# the blob literal only.)
	blob_start = coap_c.index("tick_coap_apis_link_format[] =")
	blob_end = coap_c.index("\n\t;", blob_start)
	blob = coap_c[blob_start:blob_end]
	for base in ("start", "stop", "get_status", "config", "get_config"):
		assert f"/{base}>" not in blob, (
			f"base method {base} leaked into apis blob: {blob}"
		)


def test_discoverable_base_only_emits_instances_no_apis(tmp_path):
	"""A discoverable service declaring ONLY base methods generates the
	`/instances` resource but NOT `/apis` — base names are served once
	by the framework's shared `/zlet/apis`, so per-type `/apis` is only
	emitted for types that have custom rpcs. The wildcard stays
	discoverable (no HIDDEN tag)."""
	_run_codegen(_FIXTURES / "tick_discoverable_full.proto", tmp_path)
	coap_c = (tmp_path / "zlet_tick_coap_interface.c").read_text()
	assert "COAP_RESOURCE_DEFINE(tick_coap_instances_resource" in coap_c
	assert "COAP_RESOURCE_DEFINE(tick_coap_apis_resource" not in coap_c
	assert "tick_coap_apis_link_format[]" not in coap_c
	assert "ZEPHLET_COAP_HIDDEN" not in coap_c


def test_discoverable_missing_method_is_rejected(tmp_path):
	"""Codegen must surface a diagnostic naming the missing base methods
	and exit non-zero rather than emit a half-broken interface."""
	result = subprocess.run(
		[sys.executable, str(_CODEGEN),
		 "--proto", str(_FIXTURES / "tick_discoverable_missing.proto"),
		 "--output-dir", str(tmp_path),
		 "--type", "tick",
		 "--prefix", "zlet_tick"],
		check=False, capture_output=True, text=True)
	assert result.returncode != 0, result.stdout
	for missing in ("stop", "get_status", "config", "get_config"):
		assert missing in result.stderr, result.stderr
