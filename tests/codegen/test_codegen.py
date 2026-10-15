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
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


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


def _options(codegen_module, fixture, service="TickApi"):
	"""Read a fixture proto's zephlet options file."""
	return codegen_module.read_zephlet_options(
		codegen_module.options_path_for(str(_FIXTURES / fixture)), service)


def test_detect_opt_in_positive(codegen_module):
	assert _options(codegen_module, "tick_opted.proto")["coap"] is True


def test_detect_opt_in_negative(codegen_module):
	"""No options file: every option is off."""
	assert not (_FIXTURES / "tick_no_opt.zephlet.options").exists()
	assert _options(codegen_module, "tick_no_opt.proto") == {
		"coap": False, "coap_discoverable": False, "lua": False}


def test_detect_opt_in_ignores_comments(codegen_module):
	"""`#`, `//` and `/* */` comments, as in nanopb's .options files, and
	a later line overrides an earlier one."""
	assert _options(codegen_module, "tick_commented_opt.proto") == {
		"coap": False, "coap_discoverable": False, "lua": False}


@pytest.mark.parametrize("text, expected", [
	# A `/*` inside a line comment opens no block comment.
	("// starts /*\nTickApi lua:true\n# ends */\n", {"lua": True}),
	("# starts /*\nTickApi lua:true\n// ends */\n", {"lua": True}),
	# A line comment inside a block comment does not hide the rest of the line.
	("/* // */ TickApi coap:true\n", {"coap": True}),
])
def test_options_file_comment_nesting(codegen_module, tmp_path, text, expected):
	"""Whichever comment starts first wins, as nanopb reads them."""
	path = tmp_path / "x.zephlet.options"
	path.write_text(text)
	opts = dict.fromkeys(("coap", "coap_discoverable", "lua"), False)
	opts.update(expected)
	assert codegen_module.read_zephlet_options(str(path), "TickApi") == opts


def test_options_file_error_line_after_block_comment(tmp_path):
	"""A multi-line block comment keeps the line numbers of what follows."""
	proto = tmp_path / "zlet_tick.proto"
	proto.write_text((_FIXTURES / "tick_no_opt.proto").read_text())
	(tmp_path / "zlet_tick.zephlet.options").write_text("/*\n\n*/ // x /*\nTickApi lua:yes\n")
	result = subprocess.run(
		[sys.executable, str(_CODEGEN), "--proto", str(proto),
		 "--output-dir", str(tmp_path), "--type", "tick", "--prefix", "zlet_tick"],
		check=False, capture_output=True, text=True)

	assert result.returncode != 0, result.stdout
	assert "zlet_tick.zephlet.options:4: " in result.stderr, result.stderr


def test_options_file_follows_nanopb_syntax(codegen_module, tmp_path):
	"""fnmatch patterns against the service name, several options per line,
	`key: value` with a space, later lines win."""
	path = tmp_path / "x.zephlet.options"
	path.write_text("* coap:true lua:true\nTick* lua: false\n")
	assert codegen_module.read_zephlet_options(str(path), "TickApi") == {
		"coap": True, "coap_discoverable": False, "lua": False}


@pytest.mark.parametrize("line, needle", [
	("UiApi lua:true", "matches no service"),
	("TickApi rest:true", "unknown option 'rest:true'"),
	("TickApi lua", "unknown option 'lua'"),
	("TickApi lua:yes", "expected true or false"),
	("TickApi", "expected `<service> option:value ...`"),
])
def test_options_file_errors(tmp_path, line, needle):
	"""A line that matches no service, an unknown option or a bad value
	stops the codegen with the file and line."""
	proto = tmp_path / "zlet_tick.proto"
	proto.write_text((_FIXTURES / "tick_no_opt.proto").read_text())
	(tmp_path / "zlet_tick.zephlet.options").write_text("# first\n" + line + "\n")
	result = subprocess.run(
		[sys.executable, str(_CODEGEN), "--proto", str(proto),
		 "--output-dir", str(tmp_path), "--type", "tick", "--prefix", "zlet_tick"],
		check=False, capture_output=True, text=True)

	assert result.returncode != 0, result.stdout
	assert "zlet_tick.zephlet.options:2: " in result.stderr, result.stderr
	assert needle in result.stderr, result.stderr


def _proto_with_options(tmp_path, *options, file_text=None):
	"""tick_no_opt.proto with @p options set in its TickApi service, and an
	options file holding @p file_text unless None."""
	proto = tmp_path / "zlet_tick.proto"
	lines = ""
	for o in options:
		key, value = (part.strip() for part in o.split("=", 1))
		lines += f"\n  option (zephlet.{key}) = {value};"
	proto.write_text((_FIXTURES / "tick_no_opt.proto").read_text().replace(
		'import "zephlet.proto";',
		'import "zephlet.proto";\nimport "zephlet_options.proto";').replace(
		"service TickApi {", "service TickApi {" + lines))
	if file_text is not None:
		(tmp_path / "zlet_tick.zephlet.options").write_text(file_text)
	return proto


def _run_codegen_rc(proto, out_dir):
	return subprocess.run(
		[sys.executable, str(_CODEGEN), "--proto", str(proto),
		 "--output-dir", str(out_dir), "--type", "tick", "--prefix", "zlet_tick"],
		check=False, capture_output=True, text=True)


def test_proto_options_are_read(codegen_module, tmp_path):
	"""The options can be set in the proto, without an options file."""
	proto = _proto_with_options(tmp_path, "coap = true", "lua = true")
	ctx = codegen_module.parse_proto(str(proto))
	assert (ctx["coap_opt_in"], ctx["coap_discoverable"], ctx["lua_opt_in"]) \
		== (True, False, True)


def test_proto_and_file_options_combine(codegen_module, tmp_path):
	"""Both places are read, and the proto overrides the file, as in nanopb."""
	proto = _proto_with_options(tmp_path, "lua = false",
				    file_text="TickApi coap:true lua:true\n")
	ctx = codegen_module.parse_proto(str(proto))
	assert (ctx["coap_opt_in"], ctx["lua_opt_in"]) == (True, False)


def test_proto_and_file_give_same_output(tmp_path):
	"""An option gives the same generated code from either place."""
	in_proto = tmp_path / "proto"
	in_file = tmp_path / "file"
	in_proto.mkdir()
	in_file.mkdir()
	a = _proto_with_options(in_proto, "coap = true", "lua = true")
	b = _proto_with_options(in_file, file_text="TickApi coap:true lua:true\n")
	for proto, out in ((a, in_proto / "out"), (b, in_file / "out")):
		out.mkdir()
		result = _run_codegen_rc(proto, out)
		assert result.returncode == 0, result.stderr
	for name in sorted(p.name for p in (in_proto / "out").iterdir()):
		assert ((in_proto / "out" / name).read_bytes()
			== (in_file / "out" / name).read_bytes()), name


@pytest.mark.parametrize("option, needle", [
	("rest = true", "unknown option `(zephlet.rest)`"),
	('lua = "yes"', "`option (zephlet.lua)` expects true or false"),
])
def test_proto_option_errors(tmp_path, option, needle):
	result = _run_codegen_rc(_proto_with_options(tmp_path, option), tmp_path)
	assert result.returncode != 0, result.stdout
	assert needle in result.stderr, result.stderr


def test_discoverable_needs_coap_in_either_place(tmp_path):
	"""coap_discoverable in the proto with coap in neither place is an error;
	coap in the options file satisfies it."""
	result = _run_codegen_rc(
		_proto_with_options(tmp_path, "coap_discoverable = true"), tmp_path)
	assert result.returncode != 0, result.stdout
	assert "coap_discoverable requires coap" in result.stderr, result.stderr


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
	assert _options(codegen_module,
			"tick_discoverable_full.proto")["coap_discoverable"] is True


def test_detect_discoverable_negative(codegen_module):
	assert _options(codegen_module,
			"tick_opted.proto")["coap_discoverable"] is False


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


def test_shell_methods_emitted_for_every_rpc(tmp_path):
	"""`<TYPE>_SHELL_METHODS(X)` / `_ZLET_SHELL_METHODS_APPLY_<type>(...)`
	always render (no opt-in gate — shell exposure is per-app, not
	per-proto), with one X() row per RPC in declaration order, the
	uppercase _FIELDLIST macro name for Empty request/response
	messages resolved to EMPTY (not the blank req_c_name), and the
	correct 4-shape call_shape token, and the paste RPC each response
	can be written back through."""
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)
	header = (tmp_path / "zlet_tick_interface.h").read_text()

	assert "#define TICK_SHELL_METHODS(X)" in header
	assert "#define _ZLET_SHELL_METHODS_APPLY_tick(_instance, X)" in header

	# start/stop/get_status/dump_state-less base set: Empty req, non-Empty resp.
	assert "X(start, empty, EMPTY, lifecycle_status, LIFECYCLE_STATUS, EMPTY_RESP, NULL," in header
	# config: non-Empty req and resp.
	assert 'X(config, tick_config, TICK_CONFIG, tick_config, TICK_CONFIG, REQ_RESP, "config",' in header
	# get_config: Empty req, non-Empty resp.
	assert 'X(get_config, empty, EMPTY, tick_config, TICK_CONFIG, EMPTY_RESP, "config",' in header

	# The _APPLY rows carry the same tuples with the type baked in
	# literally as the first field (not forwarded as a macro parameter).
	assert 'X(tick, _instance, config, tick_config, TICK_CONFIG, tick_config, TICK_CONFIG, REQ_RESP, "config",' in header


def test_shell_paste_rpc_resolves_writer_by_type(tmp_path):
	"""Each row's last column names the RPC that accepts that row's
	response as its request, so a printed response can carry the command
	line that writes it back. A reader RPC cannot name its own writer;
	codegen can, because it holds every request and response type at once.

	Three cases in one service: a differently-named pair, where only the
	type matches; an RPC that both takes and returns a type, which is its
	own writer; and a response type no RPC accepts, which has no command to
	name and resolves to NULL."""
	proto = tmp_path / "zlet_pair.proto"
	proto.write_text("""
syntax = "proto3";
import "nanopb.proto";
import "zephlet.proto";
option (nanopb_fileopt).long_names = false;

message Pair {
  message Config { uint32 a = 1; }
  message Shapes { uint32 b = 1; }
  message Events { int32 timestamp = 1; }
}

service PairApi {
  rpc start      (Empty)       returns (Lifecycle.Status);
  rpc stop       (Empty)       returns (Lifecycle.Status);
  rpc get_status (Empty)       returns (Lifecycle.Status);
  rpc config     (Pair.Config) returns (Pair.Config);
  rpc get_config (Empty)       returns (Pair.Config);
  rpc set_shapes (Pair.Shapes) returns (Pair.Shapes);
  rpc get_shapes (Empty)       returns (Pair.Shapes);
}
""")
	_run_codegen(proto, tmp_path, type_name="pair", prefix="zlet_pair")
	header = (tmp_path / "zlet_pair_interface.h").read_text()

	# Matched on type, not on a get_/set_ name convention.
	assert 'X(get_shapes, empty, EMPTY, pair_shapes, PAIR_SHAPES, EMPTY_RESP, "set_shapes",' in header
	assert 'X(set_shapes, pair_shapes, PAIR_SHAPES, pair_shapes, PAIR_SHAPES, REQ_RESP, "set_shapes",' in header

	# An RPC that both takes and returns a type is its own writer, which is
	# the right answer: `zlet <inst> config <msg>` is what you paste back.
	assert 'X(config, pair_config, PAIR_CONFIG, pair_config, PAIR_CONFIG, REQ_RESP, "config",' in header
	assert 'X(get_config, empty, EMPTY, pair_config, PAIR_CONFIG, EMPTY_RESP, "config",' in header

	# Nothing in the service accepts Lifecycle.Status as a request, so the
	# lifecycle RPCs print their response with no command prefix.
	for rpc in ("start", "stop", "get_status"):
		assert (f"X({rpc}, empty, EMPTY, lifecycle_status, LIFECYCLE_STATUS, "
			"EMPTY_RESP, NULL,") in header


def test_shell_help_renders_the_request_as_a_template(tmp_path):
	"""The subcommand help is the request spelled as text format with each
	value replaced by a `<type>` placeholder, so it doubles as a template:
	replace every `<...>`, or delete `, <...>` comma and all, and what is
	left is text the parser accepts.

	Every constraint lives *inside* the angle brackets. That is what keeps
	the one rule true -- decoration outside them (`opt_scalar?:` or
	`[<uint32> x4]`) would have to be deleted rather than replaced, and
	protobuf's own parser rejects both spellings."""
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)
	header = (tmp_path / "zlet_tick_interface.h").read_text()

	assert '"duration_ms: <uint32>, period_ms: <uint32>"' in header

	# An RPC with no request describes its response instead, prefixed `->`.
	# Lifecycle.Status comes from the shared zephlet.proto, so this also
	# covers the type index spanning both files.
	assert '"-> is_running: <bool>, is_ready: <bool>"' in header
	assert '"-> duration_ms: <uint32>, period_ms: <uint32>"' in header

	# The old help literal named the C struct and nothing else.
	assert '"<text-format' not in header


def test_shell_help_is_empty_for_an_rpc_with_neither_side(tmp_path):
	"""An RPC that takes nothing and returns nothing has no shape to show,
	so it names the `Empty` message it declares rather than describing the
	absence in prose."""
	proto = tmp_path / "zlet_void.proto"
	proto.write_text("""
syntax = "proto3";
import "nanopb.proto";
import "zephlet.proto";
option (nanopb_fileopt).long_names = false;

message Void {
  message Config { uint32 a = 1; }
  message Events { int32 timestamp = 1; }
}

service VoidApi {
  rpc start      (Empty)       returns (Lifecycle.Status);
  rpc stop       (Empty)       returns (Lifecycle.Status);
  rpc get_status (Empty)       returns (Lifecycle.Status);
  rpc config     (Void.Config) returns (Void.Config);
  rpc get_config (Empty)       returns (Void.Config);
  rpc ping       (Empty)       returns (Empty);
}
""")
	_run_codegen(proto, tmp_path, type_name="void", prefix="zlet_void")
	header = (tmp_path / "zlet_void_interface.h").read_text()

	assert '"(empty)"' in header
	assert "(no request)" not in header


def test_shell_help_carries_every_constraint_inside_the_brackets(tmp_path):
	"""max_size, max_count, fixed_length, enum values and optionality all
	render, and all of them inside a `<...>`.

	The final assertion is the invariant that keeps the help pasteable: with
	every placeholder removed, nothing decorative is left behind. A future
	marker added outside the brackets fails here rather than silently
	breaking copy-paste."""
	proto = tmp_path / "zlet_shapes.proto"
	proto.write_text("""
syntax = "proto3";
import "nanopb.proto";
import "zephlet.proto";
option (nanopb_fileopt).long_names = false;

enum Flag {
  FLAG_OFF = 0;
  FLAG_ON = 1;
}

message Shapes {
  message Inner { uint32 a = 1; int32 b = 2; }
  message Config {
    optional uint32 opt = 1;
    repeated uint32 rep = 2 [(nanopb).max_count = 4];
    optional string name = 3 [(nanopb).max_size = 12];
    bytes tag = 4 [(nanopb).max_size = 4, (nanopb).fixed_length = true];
    Flag flag = 5;
    Inner one = 6;
    repeated Inner many = 7 [(nanopb).max_count = 3];
  }
  message Events { int32 timestamp = 1; }
}

service ShapesApi {
  rpc start      (Empty)         returns (Lifecycle.Status);
  rpc stop       (Empty)         returns (Lifecycle.Status);
  rpc get_status (Empty)         returns (Lifecycle.Status);
  rpc config     (Shapes.Config) returns (Shapes.Config);
  rpc get_config (Empty)         returns (Shapes.Config);
}
""")
	_run_codegen(proto, tmp_path, type_name="shapes", prefix="zlet_shapes")
	header = (tmp_path / "zlet_shapes_interface.h").read_text()

	expected = (
		"opt: <uint32?>",                       # optional
		"rep: [<uint32>, <...max 4>]",          # max_count on the continuation
		"name: <string:12?>",                   # max_size and optional together
		"tag: <bytes:4 fixed>",                 # fixed_length
		"flag: <Flag 0|1>",                     # declared enum type and values
		"one {a: <uint32>, b: <int32>}",        # submessage expanded inline
		"many: [{a: <uint32>, b: <int32>}, <...max 3>]",
	)
	for want in expected:
		assert want in header, f"{want!r} missing from:\n{header}"

	# The invariant: no decoration survives outside a placeholder.
	row = next(line for line in header.splitlines()
		   if "X(shapes, _instance, config," in line)
	help_text = row.split('", "')[-1].rstrip('") \\')
	stripped = re.sub(r"<[^>]*>", "", help_text)
	for leaked in ("?", "max ", ":1", ":4", "|"):
		assert leaked not in stripped, (
			f"{leaked!r} leaked outside a placeholder: {stripped!r}")


def test_shell_methods_invariant_across_coap_opt_in(tmp_path):
	"""The shell macro table doesn't depend on the CoAP opt-in option —
	same rows whether or not `coap:true` is set."""
	dir_no = tmp_path / "no"
	dir_yes = tmp_path / "yes"
	dir_no.mkdir()
	dir_yes.mkdir()

	_run_codegen(_FIXTURES / "tick_no_opt.proto", dir_no)
	_run_codegen(_FIXTURES / "tick_opted.proto", dir_yes)

	def shell_block(d):
		header = (d / "zlet_tick_interface.h").read_text()
		start = header.index("#define TICK_SHELL_METHODS(X)")
		end = header.index("/* Per-type frontend aggregator")
		return header[start:end]

	assert shell_block(dir_no) == shell_block(dir_yes)


def test_shell_hook_chained_into_frontend_aggregator(tmp_path):
	"""`_ZLET_SHELL_HOOK_tick` is chained into `_ZLET_FRONTEND_HOOKS_tick`
	alongside the CoAP hook, mirroring ADR-0001's per-type aggregator
	pattern — so ZEPHLET_NEW(...) alone (no app-level call) registers
	the instance under the `zlet` shell root once CONFIG_ZEPHLETS_SHELL=y,
	with no dependency on the CoAP opt-in."""
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)
	header = (tmp_path / "zlet_tick_interface.h").read_text()

	assert ("_ZLET_COAP_HOOK_tick(_name); \\\n\t_ZLET_SHELL_HOOK_tick(_name);"
		in header)
	assert "#if defined(CONFIG_ZEPHLETS_SHELL)" in header
	assert '#include "zephlet_shell_macros.h"' in header
	assert ("#define _ZLET_SHELL_HOOK_tick(_name) ZLET_SHELL_INSTANCE(tick, _name)"
		in header)
	assert "#define _ZLET_SHELL_HOOK_tick(_name) /* empty: CONFIG_ZEPHLETS_SHELL disabled */" in header


def test_textformat_descriptors_emitted_per_message(tmp_path):
	"""One PB_TF_DEFINE per message in the proto, in the .c (a single
	translation unit), with a matching PB_TF_DECLARE in the header —
	the handlers that reference them are generated at the ZEPHLET_NEW()
	call site, in user code.

	Every message gets one, not just the RPC request/response types:
	PB_TF_DEFINE emits an `extern` for each submessage descriptor a
	field row references, so a message reachable only as a submessage
	must still be defined or the link fails."""
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)
	source = (tmp_path / "zlet_tick_interface.c").read_text()
	header = (tmp_path / "zlet_tick_interface.h").read_text()

	# Nested messages flatten to <parent>_<child> under
	# long_names = false, and PB_TF_DEFINE's two arguments are the
	# FIELDLIST prefix and the C struct — which differ under --c-style.
	assert "PB_TF_DEFINE(TICK_CONFIG, tick_config_t);" in source
	assert "PB_TF_DEFINE(TICK_EVENTS, tick_events_t);" in source

	assert "PB_TF_DECLARE(tick_config_t);" in header
	assert "PB_TF_DECLARE(tick_events_t);" in header

	# Declared under the library's own guard, so application code can print
	# a message without enabling a frontend.
	h_start = header.index("#if defined(CONFIG_NANOPB_TEXTFORMAT)")
	h_end = header.index("#endif /* CONFIG_NANOPB_TEXTFORMAT */")
	assert "PB_TF_DECLARE(tick_config_t);" in header[h_start:h_end]

	# Definitions belong to exactly one TU; the header only declares.
	assert "PB_TF_DEFINE(" not in header
	assert "PB_TF_DECLARE(" not in source


def test_textformat_descriptors_gated_on_the_library(tmp_path):
	"""The descriptors cost flash, so they are compiled only when the
	library is available -- but on the library, not on any frontend.

	Printing a proto struct as text format is useful wherever it can be
	done, application logging as much as a shell, so an app should not have
	to enable a frontend to get a descriptor for its own messages."""
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)
	source = (tmp_path / "zlet_tick_interface.c").read_text()

	start = source.index("#if defined(CONFIG_NANOPB_TEXTFORMAT)")
	end = source.index("#endif /* CONFIG_NANOPB_TEXTFORMAT */")
	block = source[start:end]

	# Not gated on the shell frontend, which is only one consumer.
	assert "CONFIG_ZEPHLETS_SHELL" not in block

	assert "PB_TF_DEFINE(TICK_CONFIG, tick_config_t);" in block
	assert "PB_TF_DEFINE(TICK_EVENTS, tick_events_t);" in block
	# ...and every invocation in the file is inside the guard. Counts
	# use "PB_TF_DEFINE(" so the surrounding comment, which names the
	# macro in prose, does not inflate them.
	assert source.count("PB_TF_DEFINE(") == block.count("PB_TF_DEFINE(")


def test_textformat_descriptor_emitted_for_field_less_message(tmp_path):
	"""Every message of the proto gets a descriptor, a field-less one too,
	such as an empty Config or the namespace wrapper `message Tick { ... }`.
	The shared zephlet.proto's messages are not this proto's: `Empty` gets
	none, and the infra defines the one for Lifecycle.Status by hand."""
	_run_codegen(_FIXTURES / "tick_no_opt.proto", tmp_path)
	source = (tmp_path / "zlet_tick_interface.c").read_text()

	assert "PB_TF_DEFINE(TICK, tick_t);" in source
	assert "PB_TF_DEFINE(EMPTY" not in source
	assert "PB_TF_DEFINE(LIFECYCLE_STATUS" not in source


def test_textformat_descriptors_collect_nested_and_dedupe(tmp_path):
	"""collect_tf_messages() walks nested messages to any depth and
	emits each exactly once, however many RPCs share it."""
	proto = tmp_path / "zlet_nest.proto"
	proto.write_text("""
syntax = "proto3";
import "nanopb.proto";
import "zephlet.proto";
option (nanopb_fileopt).long_names = false;

message Nest {
  message Point { int32 x = 1; }
  message Config {
    uint32 a = 1;
    Point origin = 2;
  }
  message Events { int32 timestamp = 1; }
}

service NestApi {
  rpc start      (Empty)       returns (Lifecycle.Status);
  rpc stop       (Empty)       returns (Lifecycle.Status);
  rpc get_status (Empty)       returns (Lifecycle.Status);
  rpc config     (Nest.Config) returns (Nest.Config);
  rpc get_config (Empty)       returns (Nest.Config);
}
""")
	_run_codegen(proto, tmp_path, type_name="nest", prefix="zlet_nest")
	source = (tmp_path / "zlet_nest_interface.c").read_text()

	# Point is reachable only as a submessage of Config, never as an RPC
	# request or response — and it still needs a descriptor.
	assert "PB_TF_DEFINE(NEST_POINT, nest_point_t);" in source

	# config and get_config both carry Nest.Config; one definition only,
	# or the link fails on a duplicate symbol.
	assert source.count("PB_TF_DEFINE(NEST_CONFIG, nest_config_t);") == 1


def test_textformat_descriptor_emitted_for_oneof_only_message(tmp_path):
	"""A message whose every field sits in a `oneof` still needs a
	descriptor.

	`proto_schema_parser` nests oneof members inside a OneOf element rather
	than listing them as direct children of the message, so a scan for
	direct Field children sees such a message as empty -- while nanopb emits
	an ordinary FIELDLIST for it. Skipping it leaves `<msg>_t_tf` undefined,
	which surfaces as a link error either from a handler that uses the
	message as an RPC request or from a parent's `extern`, not as anything
	that names the cause."""
	proto = tmp_path / "zlet_oneof.proto"
	proto.write_text("""
syntax = "proto3";
import "nanopb.proto";
import "zephlet.proto";
option (nanopb_fileopt).long_names = false;

message Oneof {
  message Config {
    oneof choice {
      uint32 c_num = 1;
      string c_str = 2 [(nanopb).max_size = 8];
    }
  }
  message Mixed {
    uint32 keep = 1;
    oneof pick { uint32 p_num = 2; }
  }
  message Wrapper {
    message Inner { uint32 x = 1; }
  }
  message Events { int32 timestamp = 1; }
}

service OneofApi {
  rpc start      (Empty)        returns (Lifecycle.Status);
  rpc stop       (Empty)        returns (Lifecycle.Status);
  rpc get_status (Empty)        returns (Lifecycle.Status);
  rpc config     (Oneof.Config) returns (Oneof.Config);
  rpc get_config (Empty)        returns (Oneof.Config);
}
""")
	_run_codegen(proto, tmp_path, type_name="oneof", prefix="zlet_oneof")
	source = (tmp_path / "zlet_oneof_interface.c").read_text()

	# Fields live only in a oneof -- and this one is an RPC request/response.
	assert "PB_TF_DEFINE(ONEOF_CONFIG, oneof_config_t);" in source
	# A oneof alongside an ordinary field must not double-count either.
	assert source.count("PB_TF_DEFINE(ONEOF_MIXED, oneof_mixed_t);") == 1
	# A field-less message gets one too, and so does the one nested in it.
	assert "PB_TF_DEFINE(ONEOF_WRAPPER, oneof_wrapper_t);" in source
	assert "PB_TF_DEFINE(ONEOF_WRAPPER_INNER, oneof_wrapper_inner_t);" in source


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


_LUA_PROTO = """
syntax = "proto3";
import "nanopb.proto";
import "zephlet.proto";
option (nanopb_fileopt).long_names = false;

message Gauge {
  enum Unit { UNIT_NONE = 0; UNIT_MV = 1; }
  message Inner { int32 a = 1; }
  message Config {
    optional uint32 limit = 1;
    repeated int32 samples = 2 [(nanopb).max_count = 4];
    Inner inner = 3;
    Unit unit = 4;
    string label = 5 [(nanopb).max_size = 8];
  }
  message Events { uint32 seq = 1; }
}

service GaugeApi {
  rpc start      (Empty)        returns (Lifecycle.Status);
  rpc get_status (Empty)        returns (Lifecycle.Status);
  rpc config     (Gauge.Config) returns (Gauge.Config);
  rpc reset      (Empty)        returns (Empty);
  rpc put        (Gauge.Config) returns (Empty);
}
"""


_LUA_OPT_IN = "GaugeApi lua:true\n"


def _write_lua_proto(tmp_path, text=_LUA_PROTO, options=_LUA_OPT_IN):
	"""The gauge proto, and its options file unless @p options is None."""
	proto = tmp_path / "zlet_gauge.proto"
	proto.write_text(text)
	if options is not None:
		(tmp_path / "zlet_gauge.zephlet.options").write_text(options)
	return proto


def _run_lua_codegen(tmp_path, text=_LUA_PROTO, options=_LUA_OPT_IN):
	proto = _write_lua_proto(tmp_path, text, options)
	_run_codegen(proto, tmp_path, type_name="gauge", prefix="zlet_gauge")
	return tmp_path


def test_lua_hook_chained_and_gated(tmp_path):
	"""The Lua hook joins the per-type aggregator, and the Lua header is
	only included under CONFIG_ZEPHLETS_LUA -- otherwise the hook is empty,
	so a build with the frontend off sees no Lua at all."""
	out = _run_lua_codegen(tmp_path)
	header = (out / "zlet_gauge_interface.h").read_text()

	assert ("_ZLET_SHELL_HOOK_gauge(_name); \\\n\t_ZLET_LUA_HOOK_gauge(_name);"
		in header)
	gated = header.index("#if defined(CONFIG_ZEPHLETS_LUA)")
	assert header.index('#include "zlet_gauge_lua_interface.h"') > gated
	assert ("#define _ZLET_LUA_HOOK_gauge(_name) /* empty: CONFIG_ZEPHLETS_LUA disabled */"
		in header)

	lua_h = (out / "zlet_gauge_lua_interface.h").read_text()
	assert "#define _ZLET_LUA_HOOK_gauge(_name)" in lua_h
	assert ("STRUCT_SECTION_ITERABLE(zephlet_lua_instance, __zlet_lua__gauge__##_name)"
		in lua_h)
	assert "ZEPHLET_EVENTS_LISTENER(_name, gauge, _zlet_lua_fwd_##_name)" in lua_h
	assert "sizeof(struct gauge_events) <= CONFIG_ZEPHLETS_LUA_SLOT_SIZE" in lua_h


def test_lua_not_opted_in_emits_no_bindings(tmp_path):
	"""Without the opt-in the hook is empty even under CONFIG_ZEPHLETS_LUA, and
	the Lua outputs are stubs: no record, listener, trampolines or classes."""
	out = _run_lua_codegen(tmp_path, options=None)
	header = (out / "zlet_gauge_interface.h").read_text()

	assert "#define _ZLET_LUA_HOOK_gauge(_name) /* empty */" in header
	assert "zlet_gauge_lua_interface.h" not in header
	assert "CONFIG_ZEPHLETS_LUA" not in header
	assert "_ZLET_LUA_HOOK_gauge" not in (out / "zlet_gauge_lua_interface.h").read_text()
	source = (out / "zlet_gauge_lua_interface.c").read_text()
	assert "#include" not in source and "gauge_lua_type" not in source
	assert "---@class" not in (out / "zlet_gauge_lua_meta.lua").read_text()


def test_lua_bindings_call_typed_wrappers(tmp_path):
	"""One trampoline per RPC, through the generated typed wrapper, with
	descriptors and struct sizes per shape."""
	source = (_run_lua_codegen(tmp_path) / "zlet_gauge_lua_interface.c").read_text()

	assert "return gauge_reset(z, timeout);" in source
	assert "return gauge_start(z, (struct lifecycle_status *)resp, timeout);" in source
	assert "return gauge_put(z, (const struct gauge_config *)req, timeout);" in source
	assert "return gauge_config(z, (const struct gauge_config *)req," in source

	row = source[source.index('.name = "config"'):]
	row = row[:row.index("},")]
	assert ".req_tf = &gauge_config_t_tf" in row
	assert ".resp_tf = &gauge_config_t_tf" in row
	assert ".req_size = sizeof(struct gauge_config)" in row

	row = source[source.index('.name = "reset"'):]
	row = row[:row.index("},")]
	assert ".req_tf = NULL" in row and ".req_size = 0U" in row
	assert ".resp_tf = NULL" in row and ".resp_size = 0U" in row

	assert ".resp_tf = &lifecycle_status_t_tf" in source
	assert ".events_tf = &gauge_events_t_tf" in source
	assert '{"UNIT_MV", 1},' in source
	assert '.name = "Unit"' in source
	assert ("sizeof(struct gauge_config) <= CONFIG_ZEPHLETS_LUA_SCRATCH_SIZE"
		in source)


def test_lua_meta_annotates_every_shape(tmp_path):
	"""LuaLS classes follow presence: `optional` scalars and submessages may be
	nil, repeated fields are arrays, enums are typed, commands take a
	required timeout."""
	meta = (_run_lua_codegen(tmp_path) / "zlet_gauge_lua_meta.lua").read_text()

	assert "---@enum pb.Gauge.Unit" in meta
	assert "---@class pb.Gauge.Config" in meta
	assert "---@field limit? integer" in meta
	assert "---@field samples integer[]" in meta
	assert "---@field inner? pb.Gauge.Inner" in meta
	assert "---@field unit pb.Gauge.Unit" in meta
	assert "---@field label string" in meta
	assert "---@field Unit pb.Gauge.Unit" in meta
	assert "---@param policy fun(ev: pb.Gauge.Events, inst: zephlets.gauge)" in meta
	assert "function _gauge:add_policy(policy) end" in meta
	assert "---@return pb.Gauge.Events|nil ev read-only" in meta
	assert "function _gauge:await(timeout_ms) end" in meta
	assert "function _gauge:config(req, timeout_ms) end" in meta
	assert "function _gauge:reset(timeout_ms) end" in meta
	assert "---@return pb.Lifecycle.Status|nil resp" in meta


def test_lua_bindings_leave_interface_c_untouched(tmp_path):
	"""The core TU carries no Lua: everything lives in the separate file
	CMake compiles only under CONFIG_ZEPHLETS_LUA."""
	source = (_run_lua_codegen(tmp_path) / "zlet_gauge_interface.c").read_text()

	assert "lua" not in source.lower()


@pytest.mark.parametrize("clash, needle", [
	("rpc Unit (Empty) returns (Empty);", "collides"),
	("rpc add_policy (Empty) returns (Empty);", "own instance method"),
	("rpc await (Empty) returns (Empty);", "own instance method"),
	("rpc noop (Empty) returns (Empty);\n}\nmessage Other { enum Unit { U = 0; } }\n//",
	 "share the name"),
])
def test_lua_name_clash_is_rejected(tmp_path, clash, needle):
	"""Enums and rpcs share each instance's namespace with add_policy and
	await, so an enum named like an rpc, two enums sharing a name, or an rpc
	named add_policy or await could not all be reached."""
	text = _LUA_PROTO.replace("rpc put        (Gauge.Config) returns (Empty);",
				  "rpc put        (Gauge.Config) returns (Empty);\n  " + clash)
	proto = _write_lua_proto(tmp_path, text)
	result = subprocess.run(
		[sys.executable, str(_CODEGEN), "--proto", str(proto),
		 "--output-dir", str(tmp_path), "--type", "gauge", "--prefix", "zlet_gauge"],
		check=False, capture_output=True, text=True)

	assert result.returncode != 0, result.stdout
	assert needle in result.stderr, result.stderr


def test_lua_names_unchecked_without_opt_in(tmp_path):
	"""The Lua-only name checks apply only to a type exposed to Lua: an rpc
	named await is fine in a zephlet that does not opt in."""
	text = _LUA_PROTO.replace(
		"rpc put        (Gauge.Config) returns (Empty);",
		"rpc put        (Gauge.Config) returns (Empty);\n  rpc await (Empty) returns (Empty);")
	_run_lua_codegen(tmp_path, text, options=None)


@pytest.mark.parametrize("rpc, needle", [
	("rpc ext (other.Thing) returns (Empty);", "request type 'other.Thing'"),
	("rpc ext (Empty) returns (Thing);", "response type 'Thing'"),
])
def test_lua_imported_message_is_rejected(tmp_path, rpc, needle):
	"""A request or response declared in another proto has no descriptor
	here, so Lua would read and build it as {}: an error, not lost fields."""
	text = _LUA_PROTO.replace("rpc put        (Gauge.Config) returns (Empty);",
				  "rpc put        (Gauge.Config) returns (Empty);\n  " + rpc)
	proto = _write_lua_proto(tmp_path, text)
	result = subprocess.run(
		[sys.executable, str(_CODEGEN), "--proto", str(proto),
		 "--output-dir", str(tmp_path), "--type", "gauge", "--prefix", "zlet_gauge"],
		check=False, capture_output=True, text=True)

	assert result.returncode != 0, result.stdout
	assert needle in result.stderr, result.stderr


def test_lua_fieldless_message_is_accepted(tmp_path):
	"""A field-less message of this proto converts as {}, like Empty."""
	text = _LUA_PROTO.replace("rpc put        (Gauge.Config) returns (Empty);",
				  "rpc put        (Gauge.Config) returns (Empty);\n"
				  "  rpc ping (Gauge) returns (Gauge);")
	source = (_run_lua_codegen(tmp_path, text) / "zlet_gauge_lua_interface.c").read_text()

	assert '.name = "ping"' in source


def test_luals_output_keeps_only_linked_types(tmp_path, monkeypatch):
	"""The LuaLS step copies the annotations of the types the ELF links and
	deletes those a removed or renamed zephlet left from earlier builds."""
	spec = importlib.util.spec_from_file_location(
		"zephlet_lua_instances",
		_ZEPHLET_ROOT / "frontends" / "lua" / "scripts" / "zephlet_lua_instances.py")
	script = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(script)

	meta = tmp_path / "meta"
	for prefix, body in (("zlet_tick", "---@class zephlets.tick\n"),
			     ("zlet_gone", "---@class zephlets.gone\n"),
			     ("zlet_plain", "-- The zephlet does not set the `lua` option.\n")):
		(meta / prefix).mkdir(parents=True)
		(meta / prefix / f"{prefix}_lua_meta.lua").write_text(body)
	out = tmp_path / "out"
	out.mkdir()
	(out / "zlet_old_lua_meta.lua").write_text("---@class zephlets.old\n")
	base = tmp_path / "zephlets_base.lua"
	base.write_text("---@meta\n")

	monkeypatch.setattr(script, "instances", lambda elf: [("tick_main", "tick")])
	monkeypatch.setattr(sys, "argv", ["x", "--elf", "unused", "--meta-dir", str(meta),
					  "--base", str(base), "--out", str(out)])
	assert script.main() == 0

	assert sorted(p.name for p in out.iterdir()) == [
		"zephlets_base.lua", "zephlets_instances.lua", "zlet_tick_lua_meta.lua"]
	assert "---@field tick_main zephlets.tick" in (out / "zephlets_instances.lua").read_text()


_CMAKE_PROJECT = """
cmake_minimum_required(VERSION 3.20)
project(zephlet_options_incremental NONE)
set(ZEPHYR_ZEPHLET_MODULE_DIR "{root}")
add_custom_target(zephyr_generated_headers)
foreach(_fn zephyr_include_directories zephyr_library zephyr_library_sources
            zephyr_library_sources_ifdef)
  cmake_language(EVAL CODE "function(${{_fn}})\\nendfunction()")
endforeach()
include("{root}/codegen/zephyr_zephlet_codegen.cmake")
zephyr_zephlet_generate(TYPE tick PREFIX zlet_tick)
"""


@pytest.mark.skipif(not (shutil.which("cmake") and shutil.which("ninja")),
		    reason="needs cmake and ninja")
def test_options_file_changes_regenerate_incrementally(tmp_path):
	"""Adding, changing and removing <prefix>.zephlet.options regenerate the
	outputs without a clean build. Removal used to leave Ninja nothing newer
	than the outputs, so the frontends it had enabled stayed on."""
	src = tmp_path / "src"
	build = tmp_path / "build"
	src.mkdir()
	(src / "CMakeLists.txt").write_text(_CMAKE_PROJECT.format(root=_ZEPHLET_ROOT.as_posix()))
	(src / "zlet_tick.proto").write_text((_FIXTURES / "tick_no_opt.proto").read_text())
	options = src / "zlet_tick.zephlet.options"
	gen = build / "modules" / "zlet_tick"

	def build_codegen():
		subprocess.run(["ninja", "-C", str(build), "zlet_tick_codegen"], check=True,
			       capture_output=True)

	def exposed():
		lua = "_lua_type = " in (gen / "zlet_tick_lua_interface.c").read_text()
		coap = "emits no symbols" not in (gen / "zlet_tick_coap_interface.c").read_text()
		return coap, lua

	subprocess.run(["cmake", "-G", "Ninja", "-S", str(src), "-B", str(build),
			f"-DPYTHON_EXECUTABLE={sys.executable}"], check=True, capture_output=True)
	build_codegen()
	assert exposed() == (False, False), "no options file"

	options.write_text("TickApi coap:true lua:true\n")
	build_codegen()
	assert exposed() == (True, True), "options file added"

	options.write_text("TickApi lua:true\n")
	build_codegen()
	assert exposed() == (False, True), "options file changed"

	options.unlink()
	build_codegen()
	assert exposed() == (False, False), "options file removed"
