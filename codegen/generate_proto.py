#!/usr/bin/env python3
"""
Zephlet Proto Generator

Generates a full zephlet .proto file from:
  - A schema proto (zephlet.proto) defining base structure (Invoke, Report,
    Config, Events, ZephletBase service)
  - A simplified base proto (e.g. zlet_ui.proto) defining zephlet-specific
    types and custom RPCs

The generator merges the schema's base lifecycle RPCs with the zephlet's
custom RPCs, generates Invoke/Report oneofs, and qualifies type references
with the zephlet's message name. Output is rendered via a Jinja2 template.

Usage:
    python3 generate_proto.py \
        --schema zephlet.proto \
        --proto zlet_ui.proto \
        --output build/zlet_ui.proto
"""

import argparse
import os
import re
import sys

try:
    from proto_schema_parser.parser import Parser
except ImportError:
    print("Error: proto-schema-parser not installed")
    print("Install with: pip install proto-schema-parser")
    sys.exit(1)

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    print("Error: jinja2 not installed")
    print("Install with: pip install jinja2")
    sys.exit(1)


# --- Proto AST helpers ---

def parse_proto_file(path):
    """Parse a .proto file and return the parsed AST."""
    parser = Parser()
    with open(path, 'r') as f:
        content = f.read()
    try:
        return parser.parse(content)
    except Exception as e:
        print(f"Error parsing {path}: {e}")
        sys.exit(1)


def find_elements_by_type(file_elements, type_name):
    """Find all elements of a given class name in file_elements."""
    return [e for e in file_elements if e.__class__.__name__ == type_name]


def find_element_by_name(file_elements, type_name, name):
    """Find a named element of a given class name."""
    for e in file_elements:
        if e.__class__.__name__ == type_name and hasattr(e, 'name') and e.name == name:
            return e
    return None


def extract_oneof_fields(message):
    """Extract oneof name and fields from a message with a single oneof."""
    for element in message.elements:
        if element.__class__.__name__ == 'OneOf':
            fields = []
            for f in element.elements:
                if hasattr(f, 'name') and f.__class__.__name__ == 'Field':
                    fields.append({
                        'name': f.name,
                        'number': f.number,
                        'type': f.type,
                    })
            return element.name, fields
    return None, []


def extract_rpc_methods(service):
    """Extract RPC methods from a service definition."""
    methods = []
    for element in service.elements:
        if element.__class__.__name__ != 'Method':
            continue

        input_type = element.input_type
        if hasattr(input_type, 'type'):
            input_type_name = input_type.type
        elif hasattr(input_type, 'name'):
            input_type_name = input_type.name
        else:
            input_type_name = str(input_type)

        output_type = element.output_type
        output_streaming = hasattr(output_type, 'stream') and output_type.stream

        if hasattr(output_type, 'type'):
            output_type_name = output_type.type
        elif hasattr(output_type, 'name'):
            output_type_name = output_type.name
        else:
            output_type_name = str(output_type)

        methods.append({
            'name': element.name,
            'input_type': input_type_name,
            'output_type': output_type_name,
            'output_streaming': output_streaming,
        })
    return methods


def has_extends_base(service):
    """Check if a service has the extends_base option."""
    for element in service.elements:
        if element.__class__.__name__ == 'Option':
            if hasattr(element, 'name') and 'extends_base' in str(element.name):
                return True
    return False


def camel_to_snake(name):
    """Convert CamelCase to snake_case."""
    name = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
    return re.sub('([a-z0-9])([A-Z])', r'\1_\2', name).lower()


# --- Nested type serialization helpers ---

def serialize_enum(enum_element):
    """Serialize an enum element into a template-friendly dict."""
    values = []
    for e in enum_element.elements:
        if e.__class__.__name__ == 'EnumValue':
            values.append({'name': e.name, 'number': e.number})
    return {'name': enum_element.name, 'kind': 'enum', 'enum_values': values}


def serialize_message(msg_element):
    """Serialize a message element into a template-friendly dict."""
    fields = []
    enums = []
    for e in msg_element.elements:
        if e.__class__.__name__ == 'Field':
            cardinality = ""
            if hasattr(e, 'cardinality') and e.cardinality is not None:
                card_str = str(e.cardinality).lower()
                if 'optional' in card_str:
                    cardinality = "optional "
                elif 'repeated' in card_str:
                    cardinality = "repeated "
            fields.append({
                'name': e.name,
                'number': e.number,
                'type': e.type,
                'cardinality': cardinality,
            })
        elif e.__class__.__name__ == 'Enum':
            enums.append(serialize_enum(e))
    return {
        'name': msg_element.name,
        'kind': 'message',
        'fields': fields,
        'enums': enums,
    }


def extract_nested_types(message):
    """Extract nested type definitions from a top-level message."""
    nested = []
    for element in message.elements:
        if element.__class__.__name__ == 'Message':
            nested.append(serialize_message(element))
        elif element.__class__.__name__ == 'Enum':
            nested.append(serialize_enum(element))
    return nested


# --- Type qualification ---

BASE_TYPES = {'Empty', 'ZephletStatus', 'ZephletResult'}
SCHEMA_PLACEHOLDER_TYPES = {'Settings', 'Events'}


def qualify_type(type_name, msg_name, nested_type_names):
    """
    Qualify a type reference with the zephlet message name.
    - Base types (Empty, ZephletStatus, ZephletResult) stay unqualified.
    - Already-qualified types (containing '.') stay as-is.
    - Schema placeholders and nested types become Msg.Type.
    """
    if type_name in BASE_TYPES:
        return type_name
    if '.' in type_name:
        return type_name
    if type_name in SCHEMA_PLACEHOLDER_TYPES or type_name in nested_type_names:
        return f"{msg_name}.{type_name}"
    return type_name


# --- Main ---

def main():
    parser = argparse.ArgumentParser(
        description="Generate full zephlet proto from schema + base proto"
    )
    parser.add_argument('--schema', required=True,
                        help='Path to schema proto (zephlet.proto)')
    parser.add_argument('--proto', required=True,
                        help='Path to base proto (e.g. zlet_ui.proto)')
    parser.add_argument('--output', required=True,
                        help='Output path for generated proto')
    args = parser.parse_args()

    if not os.path.exists(args.schema):
        print(f"Error: Schema file not found: {args.schema}")
        sys.exit(1)
    if not os.path.exists(args.proto):
        print(f"Error: Base proto not found: {args.proto}")
        sys.exit(1)

    # Parse schema and base file
    schema_ast = parse_proto_file(args.schema)
    base_ast = parse_proto_file(args.proto)

    # --- Extract schema information ---

    schema_invoke = find_element_by_name(
        schema_ast.file_elements, 'Message', 'Invoke')
    if not schema_invoke:
        print("Error: Schema missing 'Invoke' message")
        sys.exit(1)
    _, schema_invoke_fields = extract_oneof_fields(schema_invoke)

    schema_report = find_element_by_name(
        schema_ast.file_elements, 'Message', 'Report')
    if not schema_report:
        print("Error: Schema missing 'Report' message")
        sys.exit(1)
    _, schema_report_fields = extract_oneof_fields(schema_report)

    schema_service = find_element_by_name(
        schema_ast.file_elements, 'Service', 'ZephletBase')
    if not schema_service:
        print("Error: Schema missing 'ZephletBase' service")
        sys.exit(1)
    schema_rpcs = extract_rpc_methods(schema_service)

    # --- Extract base file information ---

    # Find top-level zephlet message (skip utility types)
    skip_names = {'_', 'Empty', 'ZephletStatus', 'ZephletResult',
                  'Invoke', 'Report', 'Settings', 'Events'}
    base_messages = find_elements_by_type(base_ast.file_elements, 'Message')
    zephlet_msg = None
    for msg in base_messages:
        if msg.name not in skip_names:
            zephlet_msg = msg
            break

    if not zephlet_msg:
        print("Error: No zephlet message found in base proto")
        sys.exit(1)

    msg_name = zephlet_msg.name
    msg_name_lower = msg_name.lower()
    print(f"Zephlet message: {msg_name}")

    # Find service with extends_base
    base_services = find_elements_by_type(base_ast.file_elements, 'Service')
    base_service = None
    for svc in base_services:
        if has_extends_base(svc):
            base_service = svc
            break

    if not base_service:
        print("Error: No service with extends_base option found in base proto")
        sys.exit(1)

    service_name = base_service.name
    print(f"Service: {service_name}")

    # Extract custom RPCs and nested types
    custom_rpcs = extract_rpc_methods(base_service)
    print(f"Custom RPCs: {[r['name'] for r in custom_rpcs]}")

    nested_types = extract_nested_types(zephlet_msg)
    nested_type_names = {n['name'] for n in nested_types}

    has_settings = 'Settings' in nested_type_names
    has_events = 'Events' in nested_type_names

    # --- Validate Settings message contract (proto3 `optional` on every field) ---
    if has_settings:
        settings_msg = next(
            (n for n in nested_types if n['name'] == 'Settings'), None)
        if settings_msg is not None:
            non_optional = [
                f['name'] for f in settings_msg['fields']
                if 'optional' not in (f.get('cardinality') or '')
            ]
            if non_optional:
                print(
                    f"Error: {msg_name}.Settings fields must be declared "
                    f"`optional` to support partial updates via "
                    f"update_settings: {', '.join(non_optional)}"
                )
                sys.exit(1)

            # Detect the nanopb has_<field> companion name collision: if the
            # user defined a field literally named `has_<x>` it will clash
            # with the nanopb presence flag of an optional field `x`.
            field_names = {f['name'] for f in settings_msg['fields']}
            for field in settings_msg['fields']:
                name = field['name']
                if name.startswith('has_') and name[4:] in field_names:
                    print(
                        f"Error: {msg_name}.Settings field '{name}' collides "
                        f"with the nanopb presence flag for optional field "
                        f"'{name[4:]}'"
                    )
                    sys.exit(1)

    # --- Build Invoke fields ---

    invoke_fields = []
    for field in schema_invoke_fields:
        invoke_fields.append({
            'name': field['name'],
            'number': field['number'],
            'type': qualify_type(field['type'], msg_name, nested_type_names),
        })

    next_invoke_num = max(f['number'] for f in schema_invoke_fields) + 1
    for rpc in custom_rpcs:
        invoke_fields.append({
            'name': rpc['name'],
            'number': next_invoke_num,
            'type': qualify_type(rpc['input_type'], msg_name, nested_type_names),
        })
        next_invoke_num += 1

    # --- Build Report fields ---

    report_fields = []
    for field in schema_report_fields:
        report_fields.append({
            'name': field['name'],
            'number': field['number'],
            'type': qualify_type(field['type'], msg_name, nested_type_names),
        })

    # Collect existing report type base names for dedup
    existing_base_types = set()
    for field in schema_report_fields:
        t = field['type']
        existing_base_types.add(t.split('.')[-1] if '.' in t else t)

    next_report_num = max(f['number'] for f in schema_report_fields) + 1
    for rpc in custom_rpcs:
        output_type = rpc['output_type']
        base_output = output_type.split('.')[-1] if '.' in output_type else output_type

        # Skip types already covered
        if base_output in existing_base_types:
            continue
        if output_type in BASE_TYPES:
            continue

        qualified = qualify_type(output_type, msg_name, nested_type_names)
        field_name = camel_to_snake(base_output)

        report_fields.append({
            'name': field_name,
            'number': next_report_num,
            'type': qualified,
        })
        existing_base_types.add(base_output)
        next_report_num += 1

    # --- Build RPC list (base + custom) ---

    all_rpcs = []
    for rpc in schema_rpcs:
        all_rpcs.append({
            'name': rpc['name'],
            'input_type': qualify_type(
                rpc['input_type'], msg_name, nested_type_names),
            'output_type': qualify_type(
                rpc['output_type'], msg_name, nested_type_names),
            'stream_prefix': "stream " if rpc['output_streaming'] else "",
        })
    for rpc in custom_rpcs:
        all_rpcs.append({
            'name': rpc['name'],
            'input_type': qualify_type(
                rpc['input_type'], msg_name, nested_type_names),
            'output_type': qualify_type(
                rpc['output_type'], msg_name, nested_type_names),
            'stream_prefix': "stream " if rpc['output_streaming'] else "",
        })

    # --- Render Jinja template ---

    script_dir = os.path.dirname(os.path.abspath(__file__))
    template_dir = os.path.join(script_dir, 'templates')
    env = Environment(loader=FileSystemLoader(template_dir))
    template = env.get_template('zephlet_proto.proto.jinja')

    context = {
        'schema_proto_name': os.path.basename(args.schema),
        'base_proto_name': os.path.basename(args.proto),
        'msg_name': msg_name,
        'msg_name_lower': msg_name_lower,
        'service_name': service_name,
        'nested_types': nested_types,
        'invoke_oneof_name': f"{msg_name_lower}_invoke_tag",
        'invoke_fields': invoke_fields,
        'report_oneof_name': f"{msg_name_lower}_report_tag",
        'report_fields': report_fields,
        'all_rpcs': all_rpcs,
        'has_settings': has_settings,
        'has_events': has_events,
    }

    output_content = template.render(**context)

    # Write output
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    with open(args.output, 'w') as f:
        f.write(output_content)

    print(f"Generated: {args.output}")


if __name__ == '__main__':
    main()
