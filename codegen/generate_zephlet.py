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


def detect_coap_opt_in(tree) -> bool:
    """
    Return True iff a service in the parsed proto AST declares
    `option (zephlet.coap) = true;`.

    `tree` is a `proto_schema_parser.ast.File` (the result of
    `Parser().parse(text)`). Walking the AST keeps the parser as the
    single source of truth for proto structure — comments, string
    literals, and option-shaped text outside a service body are
    naturally ignored without a second tokenization pass. Multiple
    services in one file are tolerated; any service-level opt-in
    flips the whole zephlet on.
    """
    for elem in tree.file_elements:
        if elem.__class__.__name__ != "Service":
            continue
        for inner in elem.elements:
            if (inner.__class__.__name__ == "Option"
                    and inner.name == "(zephlet.coap)"
                    and inner.value is True):
                return True
    return False


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


def nanopb_descriptor(c_name: str) -> str:
    """
    Nanopb with --c-style appends '_t_msg' to the message descriptor symbol.
    """
    return f"{c_name}_t_msg" if c_name else ""


def parse_proto(proto_path: str) -> dict:
    """
    Parse the .proto and return a flat dict for the Jinja templates.
    """
    with open(proto_path, "r") as f:
        content = f.read()
    tree = Parser().parse(content)
    coap_opt_in = detect_coap_opt_in(tree)

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

    # Locate the service block. Exactly one service expected.
    service = None
    for elem in tree.file_elements:
        if elem.__class__.__name__ == "Service":
            service = elem
            break

    if service is None:
        print(f"{proto_path}: no service block found", file=sys.stderr)
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

        commands.append({
            "name": method.name,
            "method_id": next_id,
            "req_c_name": req_c,
            "resp_c_name": resp_c,
            "req_is_empty": (input_type == "Empty"),
            "resp_is_empty": (output_type == "Empty"),
            "req_desc": nanopb_descriptor(req_c) or None,
            "resp_desc": nanopb_descriptor(resp_c) or None,
        })
        next_id += 1

    if not commands:
        print(f"{proto_path}: service block has no rpc methods", file=sys.stderr)
        sys.exit(1)

    return {
        "owning_type_camel": owning_type,
        "type_snake": camel_to_snake(owning_type),
        "type_upper": camel_to_snake(owning_type).upper(),
        "commands": commands,
        "num_methods_including_reserved": commands[-1]["method_id"] + 1,
        "coap_opt_in": coap_opt_in,
    }


def render_templates(ctx: dict, prefix: str, template_dir: str,
                     output_dir: str) -> tuple[str, str, str, str]:
    """
    Render the four per-zephlet artifacts:
      <prefix>_interface.{h,c}      — always populated (core dispatch).
      <prefix>_coap_interface.{h,c} — populated only when the proto opts in;
                                      empty stubs otherwise. CMake compiles
                                      the .c unconditionally under
                                      `CONFIG_ZEPHLETS_COAP=y`, so the empty
                                      stub is a 0-symbol TU for non-opted
                                      types.
    """
    env = Environment(
        loader=FileSystemLoader(template_dir),
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )
    header_tpl = env.get_template("zephlet_interface.h.jinja")
    source_tpl = env.get_template("zephlet_interface.c.jinja")
    coap_header_tpl = env.get_template("zephlet_coap_interface.h.jinja")
    coap_source_tpl = env.get_template("zephlet_coap_interface.c.jinja")

    ctx = {**ctx, "prefix": prefix}
    header = header_tpl.render(**ctx)
    source = source_tpl.render(**ctx)
    coap_header = coap_header_tpl.render(**ctx)
    coap_source = coap_source_tpl.render(**ctx)

    os.makedirs(output_dir, exist_ok=True)
    header_path = os.path.join(output_dir, f"{prefix}_interface.h")
    source_path = os.path.join(output_dir, f"{prefix}_interface.c")
    coap_header_path = os.path.join(output_dir, f"{prefix}_coap_interface.h")
    coap_source_path = os.path.join(output_dir, f"{prefix}_coap_interface.c")

    with open(header_path, "w") as f:
        f.write(header)
    with open(source_path, "w") as f:
        f.write(source)
    with open(coap_header_path, "w") as f:
        f.write(coap_header)
    with open(coap_source_path, "w") as f:
        f.write(coap_source)

    return header_path, source_path, coap_header_path, coap_source_path


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--proto", required=True, help="Path to per-zephlet .proto")
    p.add_argument("--output-dir", required=True,
                   help="Directory to emit <prefix>_interface.{h,c} into")
    p.add_argument("--type", required=True,
                   help="Zephlet type (e.g. 'tick'). Used for symbol names "
                        "(tick_api, lis_tick, tick_<rpc>_impl, ...).")
    p.add_argument("--prefix", required=True,
                   help="File-name prefix (e.g. 'zlet_tick'). "
                        "Output: <prefix>_interface.{h,c}.")
    args = p.parse_args()

    if not os.path.exists(args.proto):
        print(f"Proto not found: {args.proto}", file=sys.stderr)
        return 1

    ctx = parse_proto(args.proto)

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

    header_path, source_path, coap_header_path, coap_source_path = \
        render_templates(ctx, args.prefix, template_dir, args.output_dir)

    print(f"generated: {header_path}")
    print(f"generated: {source_path}")
    print(f"generated: {coap_header_path}")
    print(f"generated: {coap_source_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
