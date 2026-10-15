#!/usr/bin/env python3
"""
Zephlet v0.3 code generator.

Reads a per-zephlet .proto file of the shape:

    message <Type> {
      message Config { ... }
      message Events { ... }
    }

    service <Type>Api {
      rpc start       (Empty)        returns (ZephletStatus);
      rpc stop        (Empty)        returns (ZephletStatus);
      rpc get_status  (Empty)        returns (ZephletStatus);
      rpc config      (<Type>.Config) returns (<Type>.Config);
      rpc get_config  (Empty)         returns (<Type>.Config);
      ...
    }

and emits:

    <output-dir>/<prefix>_interface.h
    <output-dir>/<prefix>_interface.c

The envelope shape, method-table layout, weak-handler contract, and
four-shape wrappers are fixed by the v0.3 architecture.

Usage:
    python3 generate_zephlet.py \
        --proto .../zlet_tick.proto \
        --output-dir ${CMAKE_BINARY_DIR}/modules/zlet_tick \
        --type tick \
        --prefix zlet_tick
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import sys

try:
    from proto_schema_parser.parser import Parser
except ImportError:
    print("Error: proto-schema-parser not installed", file=sys.stderr)
    sys.exit(1)

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    print("Error: jinja2 not installed", file=sys.stderr)
    sys.exit(1)


def camel_to_snake(name: str) -> str:
    s = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s).lower()


# Keys of a zephlet's options file. Every one is a boolean, false unless set.
ZEPHLET_OPTIONS = ("coap", "coap_discoverable", "lua")


def options_path_for(proto_path: str) -> str:
    """The zephlet options file read for @p proto_path: `<prefix>.zephlet.options`
    beside it. nanopb reads only `<prefix>.options`, so the two never mix."""
    return re.sub(r"\.proto$", "", proto_path) + ".zephlet.options"


def _options_error(path: str, lineno: int, msg: str) -> None:
    print(f"{path}:{lineno}: {msg}", file=sys.stderr)
    sys.exit(1)


def read_zephlet_options(path: str, service_name: str) -> dict:
    """
    Read the options of the service @p service_name from @p path.

    The syntax is that of nanopb's .options files: `#`, `//` and `/* */`
    comments, and lines of `<pattern> option:value ...`, where the pattern is
    an fnmatch() mask matched against the service name and a later line
    overrides an earlier one:

        TickApi coap:true lua:true

    The options are the ZEPHLET_OPTIONS keys, each `true` or `false`. A line
    whose pattern matches no service is an error, as is an unknown option. A
    missing file sets nothing.
    """
    opts = dict.fromkeys(ZEPHLET_OPTIONS, False)
    if not os.path.exists(path):
        return opts

    with open(path, "r") as f:
        data = f.read()
    # One left-to-right pass, so whichever comment starts first wins: a `/*`
    # inside a line comment opens nothing, and a `//` inside a block comment
    # ends nothing. Keep a block comment's newlines, so line numbers stay right.
    data = re.sub(r"/\*.*?\*/|//[^\n]*|#[^\n]*",
                  lambda m: "\n" * m.group(0).count("\n"), data, flags=re.DOTALL)

    for lineno, line in enumerate(data.split("\n"), start=1):
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            _options_error(path, lineno, f"expected `<service> option:value ...`, "
                           f"got '{line}'")
        mask, rest = parts
        if not fnmatch.fnmatchcase(service_name, mask):
            _options_error(path, lineno, f"'{mask}' matches no service; this "
                           f"file's service is {service_name}")
        for token in re.sub(r"\s*:\s*", ":", rest).split():
            key, sep, value = token.partition(":")
            if not sep or key not in ZEPHLET_OPTIONS:
                _options_error(path, lineno, f"unknown option '{token}'; known: "
                               + ", ".join(ZEPHLET_OPTIONS))
            if value not in ("true", "false"):
                _options_error(path, lineno, f"{key}: expected true or false, "
                               f"got '{value}'")
            opts[key] = value == "true"

    return opts


def read_proto_zephlet_options(proto_path: str, service) -> dict:
    """
    Return the options that @p service sets in its proto, e.g.
    `option (zephlet.lua) = true;` (declared in zephlet_options.proto), as a
    dict holding only the keys it sets.
    """
    opts = {}
    for inner in service.elements:
        name = getattr(inner, "name", "")
        if inner.__class__.__name__ != "Option" or not name.startswith("(zephlet."):
            continue
        key = name[len("(zephlet."):-1]
        if key not in ZEPHLET_OPTIONS:
            print(f"{proto_path}: unknown option `{name}` (the proto parser "
                  "gives no line numbers); known: "
                  + ", ".join(f"(zephlet.{k})" for k in ZEPHLET_OPTIONS),
                  file=sys.stderr)
            sys.exit(1)
        if not isinstance(inner.value, bool):
            print(f"{proto_path}: `option {name}` expects true or false (the "
                  "proto parser gives no line numbers).",
                  file=sys.stderr)
            sys.exit(1)
        opts[key] = inner.value
    return opts


def zephlet_options(proto_path: str, service, options_path: str) -> dict:
    """
    The options of @p service, from both places they can be set: the options
    file @p options_path, then `option (zephlet.<key>)` in the proto. As in
    nanopb, an option set in the proto overrides the options file.
    """
    opts = read_zephlet_options(options_path, service.name)
    opts.update(read_proto_zephlet_options(proto_path, service))
    return opts


_BASE_METHODS_CACHE: list[str] | None = None


_SHARED_ELEMENTS_CACHE = None

_OPT_MAX_SIZE = "(nanopb).max_size"
_OPT_MAX_COUNT = "(nanopb).max_count"
_OPT_FIXED_LENGTH = "(nanopb).fixed_length"

# Deep enough for any hand-written proto; present only to stop a runaway walk.
# proto permits a mutually recursive message reference and nanopb rejects it
# under STATIC allocation for having infinite size -- but codegen runs before
# nanopb does, so without a guard this would recurse forever on a proto that
# was never going to build.
_HELP_MAX_DEPTH = 8


def load_shared_elements():
    """File elements of the shared `zephlet.proto`, parsed once.

    Needed because a response type is frequently `Lifecycle.Status`, which
    lives there rather than in the per-zephlet proto -- so the help text for
    `get_status` cannot be rendered from the per-zephlet file alone.
    """
    global _SHARED_ELEMENTS_CACHE
    if _SHARED_ELEMENTS_CACHE is not None:
        return _SHARED_ELEMENTS_CACHE

    script_dir = os.path.dirname(os.path.abspath(__file__))
    zephlet_proto = os.path.normpath(
        os.path.join(script_dir, os.pardir, "zephlet.proto"))
    with open(zephlet_proto, encoding="utf-8") as f:
        _SHARED_ELEMENTS_CACHE = Parser().parse(f.read()).file_elements

    return _SHARED_ELEMENTS_CACHE


def build_type_index(*element_lists):
    """Map every message and enum name to its AST node.

    Indexed under both the qualified (`Typelab.Inner`) and bare (`Inner`)
    name, because `long_names = false` means a field's type reference can be
    spelled either way. A qualified entry wins if both exist, so a nested name
    cannot be shadowed by an unrelated top-level one.
    """
    index = {}

    def walk(elements, prefix):
        for element in elements:
            kind = element.__class__.__name__
            if kind not in ("Message", "Enum"):
                continue
            qualified = f"{prefix}.{element.name}" if prefix else element.name
            index[qualified] = (kind, element)
            index.setdefault(element.name, (kind, element))
            if kind == "Message":
                walk(element.elements, qualified)

    for elements in element_lists:
        walk(elements, "")

    return index


def _message_fields(message):
    """Every field of a message, with oneof members flattened in.

    A oneof nests its members one level down in the AST, so a walk that only
    looked at `Field` children would drop them silently -- the field would
    just be missing from the help with nothing failing. They are rendered
    flat; only one can actually be set, which the proto contract makes moot
    since it sanctions no oneofs.
    """
    fields = []
    for element in message.elements:
        kind = element.__class__.__name__
        if kind == "Field":
            fields.append(element)
        elif kind == "OneOf":
            fields.extend(e for e in element.elements
                          if e.__class__.__name__ == "Field")

    return fields


def _field_options(field):
    return {o.name: o.value for o in (getattr(field, "options", None) or [])}


def _continuation(options):
    """The `<...max N>` element that marks a list as repeatable.

    The bound rides here rather than on the element placeholder so that a
    repeated *submessage* can carry it too: a submessage's value is `{...}`,
    not `<...>`, so there is nowhere inside it to put a bound without leaking
    decoration into the syntax and breaking the paste-back property.
    """
    max_count = options.get(_OPT_MAX_COUNT)

    return f", <...max {max_count}>" if max_count is not None else ", <...>"


def _placeholder(field, kind, node, options, optional):
    """The `<...>` body: `type[:size][ fixed][?]`, decoration included.

    Everything a reader must not retype lives inside the angle brackets, so
    the single rule "replace each `<...>`" leaves text the parser accepts.
    """
    if kind == "Enum":
        values = "|".join(
            str(v.number) for v in node.elements
            if v.__class__.__name__ == "EnumValue")
        body = f"{field.type} {values}" if values else field.type
    else:
        max_size = options.get(_OPT_MAX_SIZE)
        body = f"{field.type}:{max_size}" if max_size is not None else field.type

    if options.get(_OPT_FIXED_LENGTH):
        body += " fixed"

    # Only scalars carry `?`. A submessage has no placeholder to put it in.
    if optional:
        body += "?"

    return body


def render_message_help(message, index, depth=0):
    """A message rendered as the text-format template a reader can fill in."""
    parts = []

    for field in _message_fields(message):
        options = _field_options(field)
        cardinality = str(getattr(field, "cardinality", "") or "").upper()
        repeated = "REPEATED" in cardinality
        optional = "OPTIONAL" in cardinality

        entry = index.get(field.type) or index.get(field.type.split(".")[-1])
        kind = entry[0] if entry is not None else None

        if kind == "Message":
            if depth >= _HELP_MAX_DEPTH:
                body = "..."
            else:
                body = render_message_help(entry[1], index, depth + 1)
            if repeated:
                parts.append(
                    f"{field.name}: [{{{body}}}{_continuation(options)}]")
            else:
                parts.append(f"{field.name} {{{body}}}")
            continue

        holder = _placeholder(field, kind, entry[1] if entry else None,
                              options, optional and not repeated)
        if repeated:
            parts.append(f"{field.name}: [<{holder}>{_continuation(options)}]")
        else:
            parts.append(f"{field.name}: <{holder}>")

    return ", ".join(parts)


def _c_string_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def _shell_help(input_type: str, output_type: str, index) -> str:
    """The subcommand help string for one RPC, ready to be quoted.

    A request is rendered as a template to fill in. An RPC with no request
    shows its response instead. One with neither shows `(empty)`, naming
    the `Empty` message it actually declares rather than describing the
    absence in prose.
    """
    if input_type != "Empty":
        entry = index.get(input_type) or index.get(input_type.split(".")[-1])
        if entry is not None:
            return _c_string_escape(render_message_help(entry[1], index))
        return f"<text-format {input_type}>"

    if output_type == "Empty":
        return "(empty)"

    entry = index.get(output_type) or index.get(output_type.split(".")[-1])
    if entry is None:
        return "(empty)"

    return "-> " + _c_string_escape(render_message_help(entry[1], index))


def load_base_method_names() -> list[str]:
    """
    Parse the shared `zephlet.proto` once per invocation and return the
    list of method names declared in its `LifecycleApi` service block.
    The list is the single source of truth for which RPCs are "base"
    (and therefore stripped from CoAP discovery link blocks).
    """
    global _BASE_METHODS_CACHE
    if _BASE_METHODS_CACHE is not None:
        return _BASE_METHODS_CACHE

    script_dir = os.path.dirname(os.path.abspath(__file__))
    zephlet_proto = os.path.join(script_dir, os.pardir, "zephlet.proto")
    zephlet_proto = os.path.normpath(zephlet_proto)
    if not os.path.exists(zephlet_proto):
        print(f"codegen: cannot locate shared zephlet.proto at "
              f"{zephlet_proto}", file=sys.stderr)
        sys.exit(1)

    with open(zephlet_proto, "r") as f:
        tree = Parser().parse(f.read())

    base = []
    for elem in tree.file_elements:
        if elem.__class__.__name__ != "Service":
            continue
        if elem.name != "LifecycleApi":
            continue
        for m in elem.elements:
            if m.__class__.__name__ == "Method":
                base.append(m.name)
        break

    if not base:
        print(f"{zephlet_proto}: LifecycleApi service block is missing or "
              "has no methods; codegen cannot derive the base method set.",
              file=sys.stderr)
        sys.exit(1)

    _BASE_METHODS_CACHE = base
    return base


def strip_parent_qualifier(type_ref: str) -> str:
    """
    Turn 'Tick.Config' into 'Config'. Leaves bare names untouched.
    """
    return type_ref.rsplit(".", 1)[-1] if "." in type_ref else type_ref


def message_c_name(type_ref: str, owning_type: str) -> str:
    """
    Map a proto message reference to its nanopb C struct name.

    - 'Empty'              -> ''    (caller handles NULL descriptor)
    - 'ZephletStatus'      -> 'zephlet_status'
    - 'Tick.Config'        -> 'tick_config'     (with owning_type='Tick')
    - 'Config'             -> 'tick_config'     (unqualified nested type)
    """
    if type_ref == "Empty":
        return ""

    if "." in type_ref:
        parent, child = type_ref.split(".", 1)
        return f"{camel_to_snake(parent)}_{camel_to_snake(child)}"

    # Unqualified. Best-effort: treat bare 'Config'/'Events' as owning_type
    # nested types; any other bare type passes through snake_cased.
    if owning_type and type_ref in ("Config", "Events"):
        return f"{camel_to_snake(owning_type)}_{camel_to_snake(type_ref)}"
    return camel_to_snake(type_ref)


def collect_tf_messages(tree) -> list[dict]:
    """
    Every message declared in the proto, innermost-first, as
    {'uc': <FIELDLIST prefix>, 'lc': <C struct name>}.

    Drives one PB_TF_DEFINE per message for the shell frontend's text-format
    descriptors. Enumerating *every* message rather than only the RPC
    request/response ones is deliberate: PB_TF_DEFINE emits an `extern` for
    each submessage descriptor a row references, so any message reachable as a
    submessage must also be defined or the link fails. Walking the whole file
    guarantees that closure without a reachability pass.

    Messages with no fields are included too: nanopb emits a struct and an
    empty FIELDLIST for them, and an empty Config is a valid request or
    response, so its descriptor must exist. A message whose fields all live in
    a oneof, which the AST nests one level down, is an ordinary message here.

    Nested messages flatten to `parent_child`, matching nanopb under
    `long_names = false`: `Typelab.Config` -> `typelab_config`, whose FIELDLIST
    is `TYPELAB_CONFIG_FIELDLIST` and whose struct is `typelab_config_t`.
    """
    out: list[dict] = []

    def walk(elem, prefix: list[str]) -> None:
        if elem.__class__.__name__ != "Message":
            return

        chain = prefix + [camel_to_snake(elem.name)]

        for inner in elem.elements:
            walk(inner, chain)

        lc = "_".join(chain)
        out.append({"uc": lc.upper(), "lc": lc})

    for elem in tree.file_elements:
        walk(elem, [])

    return out


_LUA_SCALAR_TYPES = {
    "bool": "boolean",
    "string": "string",
    "bytes": "string",
    "float": "number",
    "double": "number",
}
for _t in ("int32", "sint32", "sfixed32", "uint32", "fixed32",
           "int64", "sint64", "sfixed64", "uint64", "fixed64"):
    _LUA_SCALAR_TYPES[_t] = "integer"

# Members the Lua frontend gives every instance itself; an rpc or enum of the
# same name could never be reached through `inst:<rpc>` / `inst.<Enum>`.
_LUA_RESERVED_INSTANCE_KEYS = {"add_policy", "await"}


def collect_qualified_names(*element_lists) -> dict:
    """Map id(AST node) -> qualified proto name (`Ui.Config`) for every
    message and enum, so a field's resolved type can be named the same way
    wherever it is referenced."""
    names = {}

    def walk(elements, prefix):
        for element in elements:
            kind = element.__class__.__name__
            if kind not in ("Message", "Enum"):
                continue
            qualified = f"{prefix}.{element.name}" if prefix else element.name
            names[id(element)] = qualified
            if kind == "Message":
                walk(element.elements, qualified)

    for elements in element_lists:
        walk(elements, "")

    return names


def collect_enums(tree) -> list[dict]:
    """Every enum declared in the proto, nested or top-level, in declaration
    order, as {'name': <bare name>, 'qualified': ..., 'values': [(name, n)]}."""
    out: list[dict] = []

    def walk(elements, prefix):
        for element in elements:
            kind = element.__class__.__name__
            if kind == "Enum":
                qualified = f"{prefix}.{element.name}" if prefix else element.name
                values = [(v.name, int(v.number)) for v in element.elements
                          if v.__class__.__name__ == "EnumValue"]
                out.append({"name": element.name, "qualified": qualified,
                            "values": values})
            elif kind == "Message":
                qualified = f"{prefix}.{element.name}" if prefix else element.name
                walk(element.elements, qualified)

    walk(tree.file_elements, "")

    return out


def _lua_field_type(field, index, qualified_names) -> str:
    """The LuaLS type of one field's value, cardinality aside."""
    entry = index.get(field.type) or index.get(field.type.split(".")[-1])
    if entry is not None:
        return "pb." + qualified_names[id(entry[1])]

    return _LUA_SCALAR_TYPES.get(field.type, "any")


def collect_lua_messages(tree, index, qualified_names) -> list[dict]:
    """Every message declared in the proto, as LuaLS class rows.

    A field is marked optional (`name?`) when it may be absent from the table
    the frontend builds: an explicit `optional` scalar, a submessage (proto3
    tracks its presence, so nanopb gives it a `has_` flag), or a oneof
    member. Repeated fields become arrays and are always present.
    """
    out: list[dict] = []

    def walk(elements):
        for element in elements:
            if element.__class__.__name__ != "Message":
                continue
            walk(element.elements)

            oneof_members = set()
            for inner in element.elements:
                if inner.__class__.__name__ == "OneOf":
                    oneof_members.update(
                        id(e) for e in inner.elements
                        if e.__class__.__name__ == "Field")

            fields = []
            for field in _message_fields(element):
                cardinality = str(getattr(field, "cardinality", "") or "").upper()
                lua_type = _lua_field_type(field, index, qualified_names)
                entry = index.get(field.type) or index.get(field.type.split(".")[-1])
                is_msg = entry is not None and entry[0] == "Message"
                if "REPEATED" in cardinality:
                    fields.append({"name": field.name, "type": f"{lua_type}[]",
                                   "optional": False})
                    continue
                optional = ("OPTIONAL" in cardinality or is_msg
                            or id(field) in oneof_members)
                fields.append({"name": field.name, "type": lua_type,
                               "optional": optional})

            out.append({"class": "pb." + qualified_names[id(element)],
                        "fields": fields})

    walk(tree.file_elements)

    return out


def nanopb_descriptor(c_name: str) -> str:
    """
    Nanopb with --c-style appends '_t_msg' to the message descriptor symbol.
    """
    return f"{c_name}_t_msg" if c_name else ""


def parse_proto(proto_path: str, options_path: str | None = None) -> dict:
    """
    Parse the .proto, with its zephlet options (see zephlet_options(); the
    options file defaults to options_path_for(proto_path)), into a flat dict
    for the Jinja templates.
    """
    with open(proto_path, "r") as f:
        content = f.read()
    tree = Parser().parse(content)

    # Locate the outer zephlet message (the one containing Config / Events).
    owning_msg = None
    for elem in tree.file_elements:
        if elem.__class__.__name__ != "Message":
            continue
        nested_names = {
            n.name
            for n in elem.elements
            if n.__class__.__name__ == "Message"
        }
        if nested_names & {"Config", "Events"}:
            owning_msg = elem
            break

    if owning_msg is None:
        print(f"{proto_path}: no message with nested Config/Events found",
              file=sys.stderr)
        sys.exit(1)

    owning_type = owning_msg.name

    # Spans the shared proto too, because a response is frequently
    # Lifecycle.Status, which is declared there rather than here.
    type_index = build_type_index(tree.file_elements, load_shared_elements())

    # Locate the service block. Exactly one service expected.
    service = None
    for elem in tree.file_elements:
        if elem.__class__.__name__ == "Service":
            service = elem
            break

    if service is None:
        print(f"{proto_path}: no service block found", file=sys.stderr)
        sys.exit(1)

    if options_path is None:
        options_path = options_path_for(proto_path)
    options = zephlet_options(proto_path, service, options_path)
    coap_opt_in = options["coap"]
    coap_discoverable = options["coap_discoverable"]
    lua_opt_in = options["lua"]

    if coap_discoverable and not coap_opt_in:
        print(f"{proto_path}: coap_discoverable requires coap on the same "
              f"service (in the proto or {os.path.basename(options_path)}).",
              file=sys.stderr)
        sys.exit(1)

    # Walk service methods, allocate method_id in declaration order starting at 1.
    commands = []
    next_id = 1
    for method in service.elements:
        if method.__class__.__name__ != "Method":
            continue

        input_type = method.input_type
        output_type = method.output_type
        # proto-schema-parser exposes MessageType with a `.type` attr for the
        # name; strings pass through.
        input_type = getattr(input_type, "type",
                             getattr(input_type, "name", str(input_type)))
        output_type = getattr(output_type, "type",
                              getattr(output_type, "name", str(output_type)))

        # Reject streaming methods (out of scope for v0.3).
        for attr in ("input_streaming", "output_streaming"):
            if getattr(method, attr, False):
                print(
                    f"{proto_path}: rpc '{method.name}' uses streaming; "
                    f"streaming is out of scope for v0.3.",
                    file=sys.stderr)
                sys.exit(1)

        req_c = message_c_name(input_type, owning_type)
        resp_c = message_c_name(output_type, owning_type)
        req_shell_lc = req_c or "empty"
        resp_shell_lc = resp_c or "empty"

        commands.append({
            "name": method.name,
            "method_id": next_id,
            "req_c_name": req_c,
            "resp_c_name": resp_c,
            "req_is_empty": (input_type == "Empty"),
            "resp_is_empty": (output_type == "Empty"),
            "req_desc": nanopb_descriptor(req_c) or None,
            "resp_desc": nanopb_descriptor(resp_c) or None,
            # Nanopb's own per-message _FIELDLIST macro name, used by the
            # shell frontend's macro framework to walk each field. 'Empty'
            # has no generated struct (req_c/resp_c == ""), but the shared
            # zephlet.proto Empty message still emits EMPTY_FIELDLIST.
            "req_shell_lc": req_shell_lc,
            "req_shell_uc": req_shell_lc.upper(),
            "resp_shell_lc": resp_shell_lc,
            "resp_shell_uc": resp_shell_lc.upper(),
            # Which of the 4 wrapper call shapes this RPC needs — decided
            # here (codegen already knows req/resp_is_empty) so the shell
            # macro framework never has to infer it from a name match.
            # Help text for the shell subcommand entry: the request
            # rendered as a fillable text-format template. An RPC that
            # takes no request shows its response instead, prefixed `->`
            # -- that is the only thing such an RPC can usefully say, and
            # it is what four lines per instance used to waste on
            # `<text-format empty>`. An RPC with neither shows `(empty)`.
            "shell_help": _shell_help(
                input_type, output_type, type_index),
            "shell_call_shape": (
                "EMPTY_EMPTY" if (input_type == "Empty" and output_type == "Empty") else
                "EMPTY_RESP" if input_type == "Empty" else
                "REQ_EMPTY" if output_type == "Empty" else
                "REQ_RESP"
            ),
        })
        next_id += 1

    if not commands:
        print(f"{proto_path}: service block has no rpc methods", file=sys.stderr)
        sys.exit(1)

    # Resolve, per RPC, the RPC that *writes* the message this one returns, so
    # the shell frontend can print a pasteable command line beneath a response.
    # A reader such as get_config has no way to name its own setter; codegen
    # does, because every method's request and response type is already in this
    # one list. First match in declaration order wins, which makes an RPC that
    # both takes and returns a type (config) resolve to itself -- the correct
    # answer, since `zlet <inst> config <msg>` is exactly what you paste back.
    for cmd in commands:
        paste_rpc = None
        if cmd["resp_c_name"]:
            for other in commands:
                if other["req_c_name"] == cmd["resp_c_name"]:
                    paste_rpc = other["name"]
                    break
        # Emitted as a C token so the template pastes it verbatim: a string
        # literal, or NULL when nothing accepts this response's type --
        # start/stop/get_status all return Lifecycle.Status, which no RPC
        # takes as a request, so those print no command prefix.
        cmd["shell_paste_rpc"] = f'"{paste_rpc}"' if paste_rpc else "NULL"

    base_method_names = load_base_method_names()
    declared = {c["name"] for c in commands}

    if coap_discoverable:
        missing = [n for n in base_method_names if n not in declared]
        if missing:
            print(
                f"{proto_path}: coap_discoverable "
                f"requires the service to declare every base lifecycle "
                f"method. Missing: {', '.join(missing)}.",
                file=sys.stderr)
            sys.exit(1)

    base_set = set(base_method_names)
    commands_base = [c for c in commands if c["name"] in base_set]
    commands_custom = [c for c in commands if c["name"] not in base_set]

    # Lua frontend: which messages carry a text-format descriptor, so a method
    # row can point at it or say NULL. Every message of this proto does; of
    # the shared ones only Lifecycle.Status, whose descriptor the infra
    # defines itself (zephlet_textformat.c).
    tf_messages = collect_tf_messages(tree)
    tf_names = {m["lc"] for m in tf_messages} | {"lifecycle_status"}
    shared_elements = load_shared_elements()
    qualified_names = collect_qualified_names(tree.file_elements, shared_elements)

    for cmd, method in zip(commands, (m for m in service.elements
                                      if m.__class__.__name__ == "Method")):
        def lua_class(type_ref):
            if type_ref == "Empty":
                return "pb.Empty"
            entry = type_index.get(type_ref) or type_index.get(type_ref.split(".")[-1])
            return "pb." + qualified_names[id(entry[1])] if entry else "table"

        input_type = getattr(method.input_type, "type",
                             getattr(method.input_type, "name", str(method.input_type)))
        output_type = getattr(method.output_type, "type",
                              getattr(method.output_type, "name", str(method.output_type)))
        # A message without a descriptor reads and builds as {}: right for
        # Empty, but a message imported from another proto has fields and no
        # descriptor here, and would lose them.
        if lua_opt_in:
            for kind, type_ref, c_name in (("request", input_type, cmd["req_c_name"]),
                                           ("response", output_type, cmd["resp_c_name"])):
                entry = (type_index.get(type_ref)
                         or type_index.get(type_ref.split(".")[-1]))
                if c_name in tf_names or (entry and not _message_fields(entry[1])):
                    continue
                print(f"{proto_path}: rpc '{method.name}': the Lua frontend cannot "
                      f"convert {kind} type '{type_ref}': it is not declared in "
                      f"this proto. Declare the message here, or drop lua.",
                      file=sys.stderr)
                sys.exit(1)
        cmd["lua_req_tf"] = (f"&{cmd['req_c_name']}_t_tf"
                             if cmd["req_c_name"] in tf_names else "NULL")
        cmd["lua_resp_tf"] = (f"&{cmd['resp_c_name']}_t_tf"
                              if cmd["resp_c_name"] in tf_names else "NULL")
        cmd["lua_req_class"] = lua_class(input_type)
        cmd["lua_resp_class"] = lua_class(output_type)

    enums = collect_enums(tree)
    # Names the Lua frontend reserves, checked only for a type it exposes.
    if lua_opt_in:
        reserved_rpcs = sorted(declared & _LUA_RESERVED_INSTANCE_KEYS)
        if reserved_rpcs:
            print(f"{proto_path}: rpc '{reserved_rpcs[0]}' collides with the Lua "
                  "frontend's own instance method of that name.", file=sys.stderr)
            sys.exit(1)

        seen = {}
        for enum in enums:
            if enum["name"] in seen:
                print(f"{proto_path}: enums '{seen[enum['name']]}' and "
                      f"'{enum['qualified']}' share the name '{enum['name']}'; the "
                      "Lua frontend exposes enums by bare name on each instance.",
                      file=sys.stderr)
                sys.exit(1)
            seen[enum["name"]] = enum["qualified"]
            if enum["name"] in declared or enum["name"] in _LUA_RESERVED_INSTANCE_KEYS:
                print(f"{proto_path}: enum '{enum['qualified']}' collides with an "
                      f"rpc or reserved Lua instance field named '{enum['name']}'.",
                      file=sys.stderr)
                sys.exit(1)

    events_lc = f"{camel_to_snake(owning_type)}_events"

    return {
        "owning_type_camel": owning_type,
        "type_snake": camel_to_snake(owning_type),
        "type_upper": camel_to_snake(owning_type).upper(),
        "commands": commands,
        "commands_base": commands_base,
        "commands_custom": commands_custom,
        "num_methods_including_reserved": commands[-1]["method_id"] + 1,
        "coap_opt_in": coap_opt_in,
        "coap_discoverable": coap_discoverable,
        "lua_opt_in": lua_opt_in,
        "tf_messages": tf_messages,
        "enums": enums,
        "lua_events_tf": f"&{events_lc}_t_tf" if events_lc in tf_names else "NULL",
        "lua_events_class": f"pb.{owning_type}.Events",
        "lua_messages": collect_lua_messages(tree, type_index, qualified_names),
    }


def render_templates(ctx: dict, prefix: str, template_dir: str,
                     output_dir: str) -> list[str]:
    """
    Render the per-zephlet artifacts:
      <prefix>_interface.{h,c}      — always populated (core dispatch).
      <prefix>_coap_interface.{h,c} — populated only when the zephlet sets
                                      the `coap` option; empty stubs
                                      otherwise. CMake compiles the .c
                                      unconditionally under
                                      `CONFIG_ZEPHLETS_COAP=y`, so the empty
                                      stub is a 0-symbol TU for non-opted
                                      types.
      <prefix>_lua_interface.{h,c}  — Lua frontend bindings, populated only
                                      with the `lua` option (empty stubs
                                      otherwise). The header is included,
                                      and the .c compiled, only under
                                      `CONFIG_ZEPHLETS_LUA=y`.
      <prefix>_lua_meta.lua         — LuaLS annotations for the type, likewise
                                      opt-in; never compiled, collected
                                      after the build.

    Returns the written paths, in that order.
    """
    env = Environment(
        loader=FileSystemLoader(template_dir),
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )
    outputs = [
        ("zephlet_interface.h.jinja", f"{prefix}_interface.h"),
        ("zephlet_interface.c.jinja", f"{prefix}_interface.c"),
        ("zephlet_coap_interface.h.jinja", f"{prefix}_coap_interface.h"),
        ("zephlet_coap_interface.c.jinja", f"{prefix}_coap_interface.c"),
        ("zephlet_lua_interface.h.jinja", f"{prefix}_lua_interface.h"),
        ("zephlet_lua_interface.c.jinja", f"{prefix}_lua_interface.c"),
        ("zephlet_lua_meta.lua.jinja", f"{prefix}_lua_meta.lua"),
    ]

    ctx = {**ctx, "prefix": prefix}
    os.makedirs(output_dir, exist_ok=True)

    paths = []
    for template_name, file_name in outputs:
        text = env.get_template(template_name).render(**ctx)
        path = os.path.join(output_dir, file_name)
        with open(path, "w") as f:
            f.write(text)
        paths.append(path)

    return paths


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--proto", required=True, help="Path to per-zephlet .proto")
    p.add_argument("--output-dir", required=True,
                   help="Directory to emit <prefix>_interface.{h,c} into")
    p.add_argument("--type", required=True,
                   help="Zephlet type (e.g. 'tick'). Used for symbol names "
                        "(tick_api, lis_tick, tick_<rpc>_impl, ...).")
    p.add_argument("--options",
                   help="Zephlet options file (default: the proto's path with "
                        "`.zephlet.options` for `.proto`). Optional.")
    p.add_argument("--prefix", required=True,
                   help="File-name prefix (e.g. 'zlet_tick'). "
                        "Output: <prefix>_interface.{h,c}.")
    args = p.parse_args()

    if not os.path.exists(args.proto):
        print(f"Proto not found: {args.proto}", file=sys.stderr)
        return 1

    ctx = parse_proto(args.proto, args.options)

    # Sanity: --type must agree with the owning message's snake-cased name.
    if ctx["type_snake"] != args.type:
        print(
            f"warning: --type '{args.type}' differs from derived "
            f"'{ctx['type_snake']}'; using --type for symbol emission",
            file=sys.stderr)
    ctx["type_snake"] = args.type
    ctx["type_upper"] = args.type.upper()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    template_dir = os.path.join(script_dir, "templates")

    for path in render_templates(ctx, args.prefix, template_dir, args.output_dir):
        print(f"generated: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
