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

# The service key that declares a required provider. Repeatable, and the
# one key whose value is not a boolean: `requires:[<name>=]<type>` in the
# options file, `option (zephlet.requires) = { type: "<type>" name: "<name>" }`
# in the proto. Both places add to one list, in that order.
REQUIRES = "requires"

# Every key a service line or a service body may set.
SERVICE_KEYS = ZEPHLET_OPTIONS + (REQUIRES,)

_C_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# A dependency name becomes a C struct field, so it must not be a keyword.
_C_KEYWORDS = frozenset("""
    auto break case char const continue default do double else enum extern
    float for goto if inline int long register restrict return short signed
    sizeof static struct switch typedef union unsigned void volatile while
    _Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn
    _Static_assert _Thread_local bool true false
    asm typeof typeof_unqual alignas alignof static_assert thread_local
    nullptr constexpr _BitInt _Decimal32 _Decimal64 _Decimal128
""".split())

# Keys set on one rpc, `TickApi.config shell:false`, with their defaults.
ZEPHLET_METHOD_OPTIONS = {"shell": True}


def options_path_for(proto_path: str) -> str:
    """The zephlet options file read for @p proto_path: `<prefix>.zephlet.options`
    beside it. nanopb reads only `<prefix>.options`, so the two never mix."""
    return re.sub(r"\.proto$", "", proto_path) + ".zephlet.options"


def _options_error(path: str, lineno: int, msg: str) -> None:
    print(f"{path}:{lineno}: {msg}", file=sys.stderr)
    sys.exit(1)


def _read_options_file(path: str, service_name: str,
                       method_names) -> tuple[dict, dict]:
    """
    Read @p path into the service's options and, per rpc in @p method_names,
    that rpc's options. See read_zephlet_options() for the syntax.
    """
    opts = dict.fromkeys(ZEPHLET_OPTIONS, False)
    opts[REQUIRES] = []
    method_opts = {name: dict(ZEPHLET_METHOD_OPTIONS) for name in method_names}
    if not os.path.exists(path):
        return opts, method_opts

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
        service_mask, dot, method_mask = mask.partition(".")
        if not fnmatch.fnmatchcase(service_name, service_mask):
            _options_error(path, lineno, f"'{service_mask}' matches no service; "
                           f"this file's service is {service_name}")
        if dot:
            targets = [n for n in method_names
                       if fnmatch.fnmatchcase(n, method_mask)]
            if not targets:
                _options_error(path, lineno, f"'{method_mask}' matches no rpc of "
                               f"{service_name}")
            known = tuple(ZEPHLET_METHOD_OPTIONS)
        else:
            known = SERVICE_KEYS
        for token in re.sub(r"\s*([:=])\s*", r"\1", rest).split():
            key, sep, value = token.partition(":")
            if not sep or key not in known:
                other = SERVICE_KEYS if dot else tuple(ZEPHLET_METHOD_OPTIONS)
                where = (f"set {key} on the service line `{service_mask} ...`"
                         if dot else
                         f"set {key} per rpc, `{service_mask}.<rpc> ...`")
                hint = f"; {where}" if sep and key in other else ""
                _options_error(path, lineno, f"unknown option '{token}'; known: "
                               + ", ".join(known) + hint)
            if key == REQUIRES:
                name, eq, dep_type = value.rpartition("=")
                if not dep_type or (eq and not name):
                    _options_error(path, lineno, f"{key}: expected "
                                   f"`requires:[<name>=]<type>`, got '{token}'")
                opts[REQUIRES].append({"type": dep_type, "name": name or None,
                                       "where": f"{path}:{lineno}"})
                continue
            if value not in ("true", "false"):
                _options_error(path, lineno, f"{key}: expected true or false, "
                               f"got '{value}'")
            if dot:
                for name in targets:
                    method_opts[name][key] = value == "true"
            else:
                opts[key] = value == "true"

    return opts, method_opts


def read_zephlet_options(path: str, service_name: str, method_names) -> dict:
    """
    Read the options of the service @p service_name from @p path.

    The syntax is that of nanopb's .options files: `#`, `//` and `/* */`
    comments, and lines of `<pattern> option:value ...`, where the pattern is
    an fnmatch() mask matched against the service name and a later line
    overrides an earlier one:

        TickApi coap:true lua:true

    The options are the ZEPHLET_OPTIONS keys, each `true` or `false`, and
    only those are returned. A pattern `<service>.<rpc>` sets the
    ZEPHLET_METHOD_OPTIONS keys of the rpcs in @p method_names its second
    half matches instead:

        TickApi.config shell:false

    A service line may also declare a required provider, `requires:<type>`,
    or `requires:<name>=<type>` to name its slot (see REQUIRES); those are
    checked here but returned by zephlet_options() only.

    Such a line is still parsed and checked here, which is all @p
    method_names is for, but its effect is not in the returned dict;
    zephlet_options() returns the per-rpc options. A line whose pattern
    matches no service or no rpc is an error, as is an unknown option. A
    missing file sets nothing.
    """
    opts = _read_options_file(path, service_name, method_names)[0]
    del opts[REQUIRES]
    return opts


def _proto_option_key(proto_path: str, inner, known, other, where) -> str | None:
    """The key of @p inner if it is an `option (zephlet.<key>)` among @p known,
    else None for any other element; exit on an unknown key or a non-bool. A
    key among @p other, the options of the other kind, is pointed @p where."""
    name = getattr(inner, "name", "")
    if inner.__class__.__name__ != "Option" or not name.startswith("(zephlet."):
        return None
    key = name[len("(zephlet."):-1]
    if key not in known:
        print(f"{proto_path}: unknown option `{name}` (the proto parser "
              "gives no line numbers); known: "
              + ", ".join(f"(zephlet.{k})" for k in known)
              + (f"; set {name} {where}" if key in other else ""),
              file=sys.stderr)
        sys.exit(1)
    if key == REQUIRES:
        return key
    if not isinstance(inner.value, bool):
        print(f"{proto_path}: `option {name}` expects true or false (the "
              "proto parser gives no line numbers).",
              file=sys.stderr)
        sys.exit(1)
    return key


def _proto_requires(proto_path: str, value) -> dict:
    """One `option (zephlet.requires) = { type: "..." name: "..." }` value
    as a REQUIRES entry; exit unless it is that message literal."""
    usage = ('expects `{ type: "<type>" }` or `{ type: "<type>" name: '
             '"<name>" }` (the proto parser gives no line numbers)')
    if value.__class__.__name__ != "MessageLiteral":
        print(f"{proto_path}: `option (zephlet.requires)` {usage}.",
              file=sys.stderr)
        sys.exit(1)
    fields = {}
    for field in value.elements:
        # The literal also carries any comment written inside its braces.
        if field.__class__.__name__ != "MessageLiteralField":
            continue
        if field.name not in ("type", "name") or field.name in fields:
            what = "repeats" if field.name in fields else "has unknown field"
            print(f"{proto_path}: `option (zephlet.requires)` {what} "
                  f"'{field.name}'; it {usage}.", file=sys.stderr)
            sys.exit(1)
        if not isinstance(field.value, str):
            print(f"{proto_path}: `option (zephlet.requires)`: '{field.name}' "
                  f"must be a quoted string; it {usage}.", file=sys.stderr)
            sys.exit(1)
        fields[field.name] = field.value
    if not fields.get("type"):
        print(f"{proto_path}: `option (zephlet.requires)` has no type; it "
              f"{usage}.", file=sys.stderr)
        sys.exit(1)
    return {"type": fields["type"], "name": fields.get("name"),
            "where": proto_path}


def read_proto_zephlet_options(proto_path: str, service) -> dict:
    """
    Return the options that @p service sets in its proto, e.g.
    `option (zephlet.lua) = true;` (declared in zephlet_options.proto), as a
    dict holding only the keys it sets. Each `option (zephlet.requires)`
    adds one entry to a list under that key, in declaration order.
    """
    opts = {}
    for inner in service.elements:
        key = _proto_option_key(proto_path, inner, SERVICE_KEYS,
                                tuple(ZEPHLET_METHOD_OPTIONS), "in an rpc body")
        if key == REQUIRES:
            opts.setdefault(REQUIRES, []).append(
                _proto_requires(proto_path, inner.value))
        elif key is not None:
            opts[key] = inner.value
    return opts


def read_proto_method_options(proto_path: str, method) -> dict:
    """
    Return the options that the rpc @p method sets in its body, e.g.
    `rpc config (...) returns (...) { option (zephlet.shell) = false; }`, as a
    dict holding only the keys it sets.
    """
    opts = {}
    for inner in getattr(method, "elements", None) or ():
        key = _proto_option_key(proto_path, inner, tuple(ZEPHLET_METHOD_OPTIONS),
                                SERVICE_KEYS, "in the service body")
        if key is not None:
            opts[key] = inner.value
    return opts


def zephlet_options(proto_path: str, service, methods,
                    options_path: str) -> tuple[dict, dict]:
    """
    The options of @p service, and per rpc the options of each of its
    @p methods, from both places they can be set: the options file
    @p options_path, then `option (zephlet.<key>)` in the proto. As in
    nanopb, an option set in the proto overrides the options file. The
    `requires` entries of both places add up instead, the file's first.
    """
    opts, method_opts = _read_options_file(options_path, service.name,
                                           [m.name for m in methods])
    proto_opts = read_proto_zephlet_options(proto_path, service)
    proto_requires = proto_opts.pop(REQUIRES, [])
    opts.update(proto_opts)
    opts[REQUIRES] = opts[REQUIRES] + proto_requires
    for method in methods:
        method_opts[method.name].update(
            read_proto_method_options(proto_path, method))
    return opts, method_opts


def resolve_requires(entries, own_type: str) -> list[dict]:
    """
    Check the REQUIRES @p entries of the zephlet type @p own_type and return
    them as `{"type", "name"}` slots in declaration order, each name
    defaulted to its type. Exit, naming where an entry was declared, on a
    type or name that is not a C identifier, a name that is a C keyword, a
    zephlet that requires its own type, or two slots of one name.
    """
    slots = []
    by_name = {}
    for entry in entries:
        where = entry["where"]
        dep_type = entry["type"]
        if not _C_IDENT.fullmatch(dep_type):
            print(f"{where}: requires: type '{dep_type}' is not a zephlet type "
                  "(a C identifier, as passed to ZEPHLET_NEW).", file=sys.stderr)
            sys.exit(1)
        if dep_type == own_type:
            print(f"{where}: requires: zephlet '{own_type}' cannot require its "
                  "own type; a type that requires itself is a synchronous "
                  "dependency cycle.", file=sys.stderr)
            sys.exit(1)
        name = entry["name"] if entry["name"] is not None else dep_type
        if not _C_IDENT.fullmatch(name) or name in _C_KEYWORDS:
            print(f"{where}: requires: '{name}' cannot name a dependency; it "
                  "becomes a C struct field, so it must be a C identifier and "
                  "not a keyword.", file=sys.stderr)
            sys.exit(1)
        if name in by_name:
            first = by_name[name]
            if entry["name"] is None and first["name"] is None:
                hint = (f"type '{dep_type}' is required twice; give each slot "
                        f"its own name, e.g. `requires:intake={dep_type}` or "
                        f'`{{ type: "{dep_type}" name: "intake" }}`')
            else:
                hint = f"dependency name '{name}' is already used"
            print(f"{where}: requires: {hint} (first at {first['where']}).",
                  file=sys.stderr)
            sys.exit(1)
        by_name[name] = entry
        slots.append({"type": dep_type, "name": name})
    return slots


def check_dependency_graph(graph: dict) -> None:
    """
    Check the build's whole dependency graph, @p graph mapping each zephlet
    type to the list of types it requires. Exit when a required type is not
    in the build, or when the types form a cycle of any length (`a -> b ->
    a`, `a -> b -> c -> a`, ...), printing the cycle. A cycle of types
    would let a synchronous RPC wait on itself.
    """
    for consumer in sorted(graph):
        for provider in graph[consumer]:
            if provider not in graph:
                print(f"zephlet type '{consumer}' requires '{provider}', but no "
                      f"zephlet of type '{provider}' is in this build. Enable "
                      f"it (e.g. CONFIG_ZEPHLET_{provider.upper()}=y) and add "
                      "its module to the build.", file=sys.stderr)
                sys.exit(1)

    # Iterative DFS, colouring a type grey while it is on the current path.
    white, grey, black = 0, 1, 2
    colour = dict.fromkeys(graph, white)
    for root in sorted(graph):
        if colour[root] != white:
            continue
        path = [root]
        stack = [iter(graph[root])]
        colour[root] = grey
        while stack:
            provider = next(stack[-1], None)
            if provider is None:
                colour[path.pop()] = black
                stack.pop()
                continue
            if colour[provider] == grey:
                cycle = path[path.index(provider):] + [provider]
                print("dependency cycle between zephlet types: "
                      + " -> ".join(cycle) + ". A zephlet must not reach "
                      "itself through (zephlet.requires).", file=sys.stderr)
                sys.exit(1)
            if colour[provider] == white:
                colour[provider] = grey
                path.append(provider)
                stack.append(iter(graph[provider]))


def init_ranks(graph: dict) -> dict:
    """
    The init rank of every type of @p graph, which check_dependency_graph()
    accepted (so it is acyclic): 0 for a type that requires nothing, else one
    more than the highest rank among its providers. Every provider type then
    ranks below each of its consumer types.
    """
    rank = {}
    for root in sorted(graph):
        stack = [root]
        while stack:
            t = stack[-1]
            todo = [p for p in graph[t] if p not in rank]
            if todo:
                stack.extend(todo)
                continue
            stack.pop()
            rank[t] = 1 + max((rank[p] for p in graph[t]), default=-1)
    return rank


# The linker sorts init priorities of up to three digits (CREATE_OBJ_LEVEL).
INIT_PRIO_MAX = 999


def write_init_prio_header(path: str, graph: dict, base: int) -> None:
    """
    Write @p path: one `_ZLET_INIT_PRIO_<type>` per type, the decimal
    literal base + rank that its instances' SYS_INIT entries use. Rewritten
    only when its content changes, so an edit that keeps every rank
    recompiles nothing.
    """
    rank = init_ranks(graph)
    top = base + max(rank.values(), default=0)
    if top > INIT_PRIO_MAX:
        print(f"zephlet init priorities reach {top}, above {INIT_PRIO_MAX}: "
              "lower CONFIG_ZEPHLET_INIT_PRIORITY or shorten the dependency "
              "chain.", file=sys.stderr)
        sys.exit(1)
    lines = [
        "/* Generated by generate_zephlet.py from the build's dependency graph. */",
        "#ifndef ZEPHLET_INIT_PRIO_H",
        "#define ZEPHLET_INIT_PRIO_H",
        "",
        "/* SYS_INIT priority (APPLICATION) of each type's instances:",
        f" * CONFIG_ZEPHLET_INIT_PRIORITY ({base}) + the type's init rank. */",
    ]
    lines += [f"#define _ZLET_INIT_PRIO_{t} {base + rank[t]}" for t in sorted(rank)]
    lines += ["", "#endif /* ZEPHLET_INIT_PRIO_H */", ""]
    write_if_changed(path, "\n".join(lines))


def _check_not_self(proto_path: str, ctx: dict, type_name: str) -> None:
    """parse_proto() checks self-dependency against the type it derives from
    the owning message; --type, which names the symbols, may differ, so
    check it too."""
    if type_name in ctx["requires_types"]:
        print(f"{proto_path}: requires: zephlet '{type_name}' cannot require "
              "its own type.", file=sys.stderr)
        sys.exit(1)


def required_types(proto_path: str, options_path: str, type_name: str) -> list:
    """The provider types the zephlet @p type_name, generated from
    @p proto_path and @p options_path, requires, after every check
    parse_proto() makes."""
    if not os.path.exists(proto_path):
        print(f"Proto not found: {proto_path}", file=sys.stderr)
        sys.exit(1)
    ctx = parse_proto(proto_path, options_path)
    _check_not_self(proto_path, ctx, type_name)
    return ctx["requires_types"]


def write_if_changed(path: str, text: str) -> None:
    """Write @p text to @p path unless it already holds exactly that, so a
    build step that depends on it does not re-run for nothing."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)


def read_requires(path: str) -> list:
    """The provider types a `<prefix>.requires` file lists, one per line."""
    try:
        with open(path) as f:
            return [line.strip() for line in f if line.strip()]
    except FileNotFoundError:
        print(f"zephlet requires file not found: {path}", file=sys.stderr)
        sys.exit(1)


def parse_graph_args(items) -> dict:
    """`<type>=<dep>,<dep>` arguments of --check-graph as a dict."""
    graph = {}
    for item in items:
        consumer, sep, deps = item.partition("=")
        if not sep or not consumer:
            print(f"--check-graph: expected <type>=<dep>,..., got '{item}'",
                  file=sys.stderr)
            sys.exit(2)
        graph[consumer] = [d for d in deps.split(",") if d]
    return graph


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
    methods = [m for m in service.elements if m.__class__.__name__ == "Method"]
    options, method_options = zephlet_options(proto_path, service, methods,
                                              options_path)
    requires = resolve_requires(options[REQUIRES], camel_to_snake(owning_type))
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
    for method in methods:
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
            # False hides this RPC from the shell frontend only: it keeps its
            # method_id and its slot in every other table.
            "shell": method_options[method.name]["shell"],
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
    # Only an RPC the shell exposes prints a response or can be pasted back.
    commands_shell = [c for c in commands if c["shell"]]
    for cmd in commands_shell:
        paste_rpc = None
        if cmd["resp_c_name"]:
            for other in commands_shell:
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

    for cmd, method in zip(commands, methods):
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
        "commands_shell": commands_shell,
        "num_methods_including_reserved": commands[-1]["method_id"] + 1,
        "coap_opt_in": coap_opt_in,
        "coap_discoverable": coap_discoverable,
        "lua_opt_in": lua_opt_in,
        "requires": requires,
        "requires_types": list(dict.fromkeys(r["type"] for r in requires)),
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
    p.add_argument("--proto", help="Path to per-zephlet .proto")
    p.add_argument("--output-dir",
                   help="Directory to emit <prefix>_interface.{h,c} into")
    p.add_argument("--type",
                   help="Zephlet type (e.g. 'tick'). Used for symbol names "
                        "(tick_api, lis_tick, tick_<rpc>_impl, ...).")
    p.add_argument("--options",
                   help="Zephlet options file (default: the proto's path with "
                        "`.zephlet.options` for `.proto`). Optional.")
    p.add_argument("--prefix",
                   help="File-name prefix (e.g. 'zlet_tick'). "
                        "Output: <prefix>_interface.{h,c}.")
    p.add_argument("--check-graph", nargs="*", metavar="TYPE=DEP,...",
                   help="Check a dependency graph given directly, one argument "
                        "per zephlet type: every required type is in it and "
                        "no cycle exists. Takes no other option.")
    p.add_argument("--check-build", nargs=3, action="append",
                   metavar=("TYPE", "PROTO", "OPTIONS"),
                   help="Check the build's dependency graph, read from every "
                        "zephlet's proto and options file; repeat once per "
                        "zephlet. With --stamp, touch that file on success.")
    p.add_argument("--check-requires", nargs=2, action="append",
                   metavar=("TYPE", "REQUIRES"),
                   help="Like --check-build, but read the zephlet's provider "
                        "types from the `<prefix>.requires` file its own "
                        "generation wrote (--requires-out), instead of "
                        "parsing its proto again.")
    p.add_argument("--requires-out", metavar="PATH",
                   help="While generating, also write the zephlet's provider "
                        "types here, one per line (only when they change).")
    p.add_argument("--stamp", help="File --check-build writes on success.")
    p.add_argument("--init-prio-header", metavar="PATH",
                   help="With --check-build, write the per-type SYS_INIT "
                        "priorities (--init-base + init rank) here.")
    p.add_argument("--init-base", type=int, default=0,
                   help="SYS_INIT priority of init rank 0 "
                        "(CONFIG_ZEPHLET_INIT_PRIORITY).")
    args = p.parse_args()

    if args.check_graph is not None:
        check_dependency_graph(parse_graph_args(args.check_graph))
        return 0

    if args.check_build is not None or args.check_requires is not None:
        graph = {}
        for type_name, proto, options in args.check_build or []:
            graph[type_name] = required_types(proto, options, type_name)
        for type_name, requires in args.check_requires or []:
            graph[type_name] = read_requires(requires)
        check_dependency_graph(graph)
        if args.init_prio_header:
            write_init_prio_header(args.init_prio_header, graph, args.init_base)
        if args.stamp:
            with open(args.stamp, "w") as f:
                f.write("".join(f"{t}={','.join(d)}\n" for t, d in sorted(graph.items())))
        return 0

    missing = [f"--{n.replace('_', '-')}" for n in ("proto", "type", "output_dir", "prefix")
               if getattr(args, n) is None]
    if missing:
        p.error("the following arguments are required: " + ", ".join(missing))

    if not os.path.exists(args.proto):
        print(f"Proto not found: {args.proto}", file=sys.stderr)
        return 1

    ctx = parse_proto(args.proto, args.options)
    _check_not_self(args.proto, ctx, args.type)

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
    if args.requires_out:
        write_if_changed(args.requires_out,
                         "".join(f"{t}\n" for t in sorted(set(ctx["requires_types"]))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
