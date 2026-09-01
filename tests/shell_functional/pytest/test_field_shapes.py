"""Every repetition rule the shell frontend can now carry, over the real
`zlet` command tree.

None of these could be expressed at all before parsing moved to
nanopb-textformat — and they did not merely fail at runtime, they failed
to *build*: the macro framework pasted nanopb's htype/ltype tokens into
macro names and only `STATIC SINGULAR <scalar>` had a definition, so a
single `optional` or one submessage anywhere in an RPC's request or
response broke the whole app's build.

Driven through `typelab_bench`'s `set_shapes`/`get_shapes` pair against
`Typelab.Shapes`, which mirrors the library's own
tests/textformat/src/fixture.proto shape matrix. Config's 18 scalars are
covered separately in test_typelab_types.py.

Note where input and output spellings differ on purpose:

  * repeated fields accept `[a, b]` but always print as repetition,
    one `name: value` per element;
  * bytes accept `\\xH[H]` escapes but always print three-digit octal,
    because a hex escape runs on whenever the next byte is also a hex
    digit while three octal digits cannot.

So round-trip holds at the value level, not at the text level.

Error assertions below match `pb_tf_strerror()` verbatim
(`nanopb_textformat_common.c:56`): "syntax error", "no such field",
"wrong value type", "value out of range", "too many values",
"value too long", "nesting too deep", "unsupported field shape",
"float support disabled".
"""

from __future__ import annotations

from twister_harness import DeviceAdapter, Shell


def _out(shell: Shell, cmd: str) -> str:
	return "\n".join(shell.get_filtered_output(shell.exec_command(cmd)))


def _set(shell: Shell, msg: str) -> str:
	return _out(shell, f"zlet typelab_bench set_shapes {msg}")


# ----- plain and optional scalars -------------------------------------


def test_plain_scalar(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	assert "plain_scalar: 7" in _set(shell, "plain_scalar: 7")


def test_optional_scalar_present(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	assert "opt_scalar: 9" in _set(shell, "opt_scalar: 9")


def test_optional_scalar_absent_prints_nothing(dut: DeviceAdapter, shell: Shell):
	"""Explicit presence is the whole point of `optional`: omitted means
	absent, and an absent field prints nothing at all."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "plain_scalar: 1")
	assert "opt_scalar:" not in out, out


def test_optional_scalar_present_with_zero(dut: DeviceAdapter, shell: Shell):
	"""Presence is set on a successful write even for a zero value —
	exactly what an implicit-presence field cannot express. `plain_scalar:
	0` is the contrast: it prints because implicit presence always does."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "opt_scalar: 0")
	assert "opt_scalar: 0" in out, out


# ----- repeated scalars ------------------------------------------------


def test_repeated_scalar_list_form(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "rep_scalar: [1, 2, 3]")
	for want in ("rep_scalar: 1", "rep_scalar: 2", "rep_scalar: 3"):
		assert want in out, out
	assert "[" not in out, out


def test_repeated_scalar_repetition_form(dut: DeviceAdapter, shell: Shell):
	"""Either a list or a repeated field name; both append. This is also
	the form the printer emits, so it round-trips textually where the
	list form does not."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "rep_scalar: 1 rep_scalar: 2 rep_scalar: 3")
	for want in ("rep_scalar: 1", "rep_scalar: 2", "rep_scalar: 3"):
		assert want in out, out


def test_repeated_scalar_past_max_count_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""rep_scalar declares max_count = 4."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "rep_scalar: [1, 2, 3, 4, 5]")
	assert "too many values" in out, out
	assert "rep_scalar" in out, out


def test_repeated_scalar_empty_list(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "rep_scalar: []")
	assert "rep_scalar:" not in out, out


# ----- submessages ----------------------------------------------------


def test_submessage(dut: DeviceAdapter, shell: Shell):
	"""The shape that made a macro-only design impossible: the C
	preprocessor cannot re-enter a field-dispatch macro inside its own
	expansion, so it could never descend into a nested field list."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "sing_msg {a: 7, b: -3}")
	assert "a: 7" in out, out
	assert "b: -3" in out, out


def test_submessage_colon_before_brace_is_optional(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "sing_msg: {a: 1, b: 2}")
	assert "a: 1" in out, out
	assert "b: 2" in out, out


def test_optional_submessage_behaves_as_singular(dut: DeviceAdapter, shell: Shell):
	"""`Inner sing_msg` and `optional Inner opt_msg` generate
	byte-identical nanopb rows (STATIC OPTIONAL MESSAGE) and identical
	storage, because in proto3 a singular submessage already carries a
	presence flag. Pinned here so the equivalence is a test, not a
	comment."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	sing = _set(shell, "sing_msg {a: 4, b: 5}")
	opt = _set(shell, "opt_msg {a: 4, b: 5}")

	assert "a: 4" in sing, sing
	assert "a: 4" in opt, opt
	assert "sing_msg" in sing, sing
	assert "opt_msg" in opt, opt


def test_absent_submessage_prints_nothing(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "plain_scalar: 1")
	assert "sing_msg" not in out, out
	assert "opt_msg" not in out, out


def test_empty_submessage_is_present_but_has_no_fields(dut: DeviceAdapter, shell: Shell):
	"""An empty brace pair sets presence without setting any field, which
	an implicit-presence scalar has no way to express."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "sing_msg {}")
	assert "sing_msg" in out, out


def test_repeated_submessage(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "rep_msg: [{a: 1, b: 2}, {a: 3, b: 4}]")
	for want in ("a: 1", "b: 2", "a: 3", "b: 4"):
		assert want in out, out


def test_repeated_submessage_past_max_count_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""rep_msg declares max_count = 3."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "rep_msg: [{a: 1}, {a: 2}, {a: 3}, {a: 4}]")
	assert "too many values" in out, out
	assert "rep_msg" in out, out


def test_unknown_field_inside_submessage_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""The descriptor tree really is walked into the submessage, rather
	than the braces being skipped wholesale."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "sing_msg {nope: 1}")
	assert "no such field" in out, out


# ----- optional / repeated strings and bytes ---------------------------


def test_optional_string(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, 'opt_string: "hi there"')
	assert 'opt_string: "hi there"' in out, out


def test_optional_string_empty_is_still_present(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, 'opt_string: ""')
	assert 'opt_string: ""' in out, out


def test_optional_string_past_max_size_is_rejected(dut: DeviceAdapter, shell: Shell):
	"""opt_string declares max_size = 12, which includes the NUL.

	There is no scratch buffer: decoded bytes go straight into the field
	and are refused at the byte that overflows, so the reported offset
	points into the literal rather than at its start."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, 'opt_string: "far too long to fit"')
	assert "value too long" in out, out
	assert "opt_string" in out, out


def test_repeated_string(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, 'rep_string: ["one", "two"]')
	assert 'rep_string: "one"' in out, out
	assert 'rep_string: "two"' in out, out


def test_optional_bytes_mixed_escapes_and_ascii(dut: DeviceAdapter, shell: Shell):
	"""A bytes value is a string literal, so escapes and printable ASCII
	mix freely — a payload the old `h<hex>` form could not express at
	all. Also proves the backslashes survived SHELL_OPT_ARG_RAW, the only
	reason they reach the parser intact."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, r'opt_bytes: "\x0F\x0Aab"')
	assert r'opt_bytes: "\017\012ab"' in out, out


def test_repeated_bytes(dut: DeviceAdapter, shell: Shell):
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, r'rep_bytes: ["\xDE\xAD", "\xBE\xEF"]')
	assert r'rep_bytes: "\336\255"' in out, out
	assert r'rep_bytes: "\276\357"' in out, out


# ----- nesting depth --------------------------------------------------


def test_nesting_three_levels_deep(dut: DeviceAdapter, shell: Shell):
	"""Shapes -> Deep1 -> Deep2 -> Deep3, so the descriptor tree is
	walked to depth 4. Iterative in both directions: the library uses an
	explicit frame array, so this costs a fixed 20 bytes of stack per
	level rather than a call frame."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	out = _set(shell, "deep {d2 {d3 {v: 42}}}")
	assert "v: 42" in out, out


# ----- everything at once ---------------------------------------------


def test_all_shapes_in_one_message_round_trip(dut: DeviceAdapter, shell: Shell):
	"""One call carrying every shape at once, then read back through the
	separate get_shapes RPC — so the descriptors are exercised in both
	directions, over the real command tree."""
	assert shell.wait_for_prompt(), "shell prompt never appeared"

	msg = (
		"plain_scalar: 1, opt_scalar: 2, rep_scalar: [3, 4], "
		"sing_msg {a: 5, b: -6}, opt_msg {a: 7, b: -8}, "
		"rep_msg: [{a: 9}, {a: 10}], "
		'opt_string: "s", rep_string: ["t", "u"], '
		r'opt_bytes: "\xAA", rep_bytes: ["\xBB"], '
		"deep {d2 {d3 {v: 11}}}"
	)

	expected = (
		"plain_scalar: 1",
		"opt_scalar: 2",
		"rep_scalar: 3",
		"rep_scalar: 4",
		"b: -6",
		"b: -8",
		'opt_string: "s"',
		'rep_string: "t"',
		'rep_string: "u"',
		r'opt_bytes: "\252"',
		r'rep_bytes: "\273"',
		"v: 11",
	)

	out = _set(shell, msg)
	for want in expected:
		assert want in out, out

	out = _out(shell, "zlet typelab_bench get_shapes")
	for want in expected:
		assert want in out, out
