#!/usr/bin/env python3
"""
Adapter Generator for Zephyr Zephlets
Generates adapter boilerplate from zephlet proto files
"""

import os
import sys
import argparse
from pathlib import Path
from proto_schema_parser import Parser
from jinja2 import Environment, FileSystemLoader


def camel_to_snake(name):
    """Convert CamelCase to snake_case"""
    result = []
    for i, char in enumerate(name):
        if char.isupper() and i > 0:
            if name[i-1].islower() or (i < len(name) - 1 and name[i+1].islower()):
                result.append('_')
        result.append(char.lower())
    return ''.join(result)


def snake_to_camel(name):
    """Convert snake_case to CamelCase"""
    return ''.join(word.capitalize() for word in name.split('_'))


def discover_zephlets(zephlets_path):
    """
    Scan zephlets/* for proto files and extract metadata
    Returns: list of dicts with zephlet metadata
    """
    zephlets = []
    zephlets_dir = Path(zephlets_path)

    if not zephlets_dir.exists():
        print(f"Error: Zephlets path '{zephlets_path}' does not exist")
        return zephlets

    # Scan each subdirectory for proto files
    for zephlet_dir in zephlets_dir.iterdir():
        if not zephlet_dir.is_dir():
            continue

        zephlet_name = zephlet_dir.name

        # Skip shared and other non-zephlet directories
        if zephlet_name in ['shared']:
            continue

        # Look for zlet_<zephlet>.proto, <zephlet>_zephlet.proto, or <zephlet>.proto
        proto_path = zephlet_dir / f"zlet_{zephlet_name}.proto"
        if not proto_path.exists():
            proto_path = zephlet_dir / f"{zephlet_name}_zephlet.proto"
        if not proto_path.exists():
            proto_path = zephlet_dir / f"{zephlet_name}.proto"

        if not proto_path.exists():
            continue

        # Parse proto file
        try:
            with open(proto_path, 'r') as f:
                proto_content = f.read()

            parser = Parser()
            parsed = parser.parse(proto_content)

            # Get messages from file_elements
            messages = []
            for element in parsed.file_elements:
                if hasattr(element, 'name') and element.__class__.__name__ == 'Message':
                    messages.append(element)

            # Find zephlet message (e.g., MsgZletTick or MsgTickZephlet)
            zephlet_msg = None
            for message in messages:
                if message.name.startswith('Msg') and (message.name.endswith('Zephlet') or message.name.startswith('MsgZlet')):
                    zephlet_msg = message
                    break

            if not zephlet_msg:
                continue

            # Find nested Report message
            report_msg = None
            report_oneof_name = None
            report_fields = []

            for element in zephlet_msg.elements:
                if hasattr(element, 'name') and element.__class__.__name__ == 'Message':
                    if element.name == 'Report':
                        report_msg = element
                        break

            if report_msg:
                # Extract oneof from Report message
                for element in report_msg.elements:
                    if element.__class__.__name__ == 'OneOf':
                        report_oneof_name = element.name
                        # Extract fields from oneof
                        for field in element.elements:
                            if hasattr(field, 'name'):
                                report_fields.append(field)
                        break

                if report_fields:
                    zephlets.append({
                        'name': zephlet_name,
                        'proto_path': str(proto_path),
                        'report_oneof': report_oneof_name,
                        'report_fields': report_fields
                    })

        except Exception as e:
            print(f"Warning: Failed to parse {proto_path}: {e}")
            continue

    return sorted(zephlets, key=lambda z: z['name'])


def select_zephlets_interactive(zephlets):
    """
    Interactive zephlet selection
    Returns: (origin_dict, dest_dict)
    """
    if not zephlets:
        print("Error: No zephlets found")
        sys.exit(1)

    print("\nAvailable zephlets:")
    for i, zephlet in enumerate(zephlets, 1):
        print(f"  {i}. {zephlet['name']}")

    # Select origin
    while True:
        try:
            origin_idx = int(input("\nSelect origin zephlet (number): ")) - 1
            if 0 <= origin_idx < len(zephlets):
                origin = zephlets[origin_idx]
                break
            print("Invalid selection")
        except (ValueError, KeyboardInterrupt):
            print("\nCancelled")
            sys.exit(1)

    # Select destination (exclude origin)
    dest_zephlets = [z for i, z in enumerate(zephlets) if i != origin_idx]
    print(f"\nAvailable destination zephlets (excluding {origin['name']}):")
    for i, zephlet in enumerate(dest_zephlets, 1):
        print(f"  {i}. {zephlet['name']}")

    while True:
        try:
            dest_idx = int(input("\nSelect destination zephlet (number): ")) - 1
            if 0 <= dest_idx < len(dest_zephlets):
                dest = dest_zephlets[dest_idx]
                break
            print("Invalid selection")
        except (ValueError, KeyboardInterrupt):
            print("\nCancelled")
            sys.exit(1)

    return origin, dest


def filter_report_fields(origin, interactive=True):
    """
    Let user select which report fields to handle
    Returns: filtered list of report fields
    """
    if not interactive:
        return origin['report_fields']

    fields = origin['report_fields']
    if not fields:
        return []

    print(f"\nReport fields from {origin['name']} zephlet:")
    for i, field in enumerate(fields, 1):
        print(f"  {i}. {field.name}")

    print("\nSelect fields to handle (e.g., '1,3' or 'all' for all fields):")
    while True:
        try:
            selection = input("> ").strip().lower()
            if selection == 'all':
                return fields

            indices = [int(x.strip()) - 1 for x in selection.split(',')]
            selected = [fields[i] for i in indices if 0 <= i < len(fields)]
            if selected:
                return selected
            print("Invalid selection")
        except (ValueError, KeyboardInterrupt):
            print("\nCancelled")
            sys.exit(1)


def suggest_destination_api(destination):
    """
    Parse destination proto to suggest available API calls
    Returns: list of invoke field names
    """
    try:
        with open(destination['proto_path'], 'r') as f:
            proto_content = f.read()

        parser = Parser()
        parsed = parser.parse(proto_content)

        # Get messages from file_elements
        messages = []
        for element in parsed.file_elements:
            if hasattr(element, 'name') and element.__class__.__name__ == 'Message':
                messages.append(element)

        # Find zephlet message (e.g., MsgUiZephlet)
        zephlet_msg = None
        for message in messages:
            if message.name.startswith('Msg') and message.name.endswith('Zephlet'):
                zephlet_msg = message
                break

        if not zephlet_msg:
            return []

        # Find nested Invoke message
        invoke_msg = None
        for element in zephlet_msg.elements:
            if hasattr(element, 'name') and element.__class__.__name__ == 'Message':
                if element.name == 'Invoke':
                    invoke_msg = element
                    break

        if not invoke_msg:
            return []

        # Extract oneof fields from Invoke message
        invoke_fields = []
        for element in invoke_msg.elements:
            if element.__class__.__name__ == 'OneOf':
                # Extract fields from oneof
                for field in element.elements:
                    if hasattr(field, 'name'):
                        invoke_fields.append(field.name)
                break

        return invoke_fields

    except Exception as e:
        print(f"Warning: Failed to parse destination proto: {e}")
        return []


def build_adapter_context(origin, dest, selected_fields, dest_api_suggestions):
    """
    Build template context for adapter generation
    """
    origin_name = origin['name']
    dest_name = dest['name']

    origin_camel = snake_to_camel(origin_name)
    dest_camel = snake_to_camel(dest_name)

    # Extract base names for Kconfig (e.g., "zlet_tick" -> "tick")
    origin_base = origin_name
    if origin_name.startswith('zlet_'):
        origin_base = origin_name[5:]
    elif origin_name.endswith('_zephlet'):
        origin_base = origin_name[:-8]

    dest_base = dest_name
    if dest_name.startswith('zlet_'):
        dest_base = dest_name[5:]
    elif dest_name.endswith('_zephlet'):
        dest_base = dest_name[:-8]

    # Adapter name should be Origin+Dest_zlet_adapter (new pattern)
    adapter_name = f"{snake_to_camel(origin_base)}+{snake_to_camel(dest_base)}_zlet_adapter"

    context = {
        'origin_zephlet': origin_name,
        'origin_zephlet_upper': origin_name.upper(),
        'origin_zephlet_camel': origin_camel,
        'origin_base': origin_base,
        'origin_base_upper': origin_base.upper(),
        'origin_report_oneof': origin['report_oneof'],
        'origin_report_fields': origin['report_fields'],
        'selected_report_fields': selected_fields,
        'dest_zephlet': dest_name,
        'dest_zephlet_upper': dest_name.upper(),
        'dest_zephlet_camel': dest_camel,
        'dest_base': dest_base,
        'dest_base_upper': dest_base.upper(),
        'adapter_name': adapter_name,
        'adapter_config': f"{origin_base.upper()}_TO_{dest_base.upper()}_ADAPTER",
        'listener_name': f"lis_{origin_name}_to_{dest_name}_adapter",
        'function_name': f"{origin_name}_to_{dest_name}_adapter",
        'dest_api_suggestions': dest_api_suggestions
    }

    return context


def render_adapter(context, output_dir, templates_dir):
    """
    Render adapter.c file using Jinja2 template
    """
    env = Environment(loader=FileSystemLoader(templates_dir))
    env.filters['camel_to_snake'] = camel_to_snake
    env.filters['upper'] = str.upper
    env.filters['lower'] = str.lower

    # Render adapter.c
    template = env.get_template('adapter.c.jinja')
    adapter_content = template.render(context)

    adapter_path = Path(output_dir) / "src" / f"{context['adapter_name']}.c"
    adapter_path.parent.mkdir(parents=True, exist_ok=True)

    with open(adapter_path, 'w') as f:
        f.write(adapter_content)

    print(f"\nGenerated: {adapter_path}")

    # Render Kconfig entry
    kconfig_template = env.get_template('adapter_kconfig.jinja')
    kconfig_content = kconfig_template.render(context)

    return adapter_path, kconfig_content


def update_kconfig(kconfig_path, kconfig_entry):
    """
    Update adapters/Kconfig with new entry
    Inserts before the module= line (logging config section)
    """
    kconfig_file = Path(kconfig_path)

    if not kconfig_file.exists():
        print(f"\nWarning: {kconfig_path} not found")
        print("Manual step required: Create Kconfig file with:")
        print(kconfig_entry)
        return False

    try:
        with open(kconfig_file, 'r') as f:
            lines = f.readlines()

        # Check for duplicate config
        config_name = kconfig_entry.split('\n')[0].split()[1]  # Extract config name
        if any(config_name in line for line in lines):
            print(f"Info: {config_name} already exists in Kconfig, skipping")
            return True

        # Find the module= line (logging config section)
        insert_idx = -1
        for i, line in enumerate(lines):
            if line.strip().startswith('module =') or line.strip().startswith('module='):
                insert_idx = i
                break

        if insert_idx == -1:
            # No module= found, insert before endif
            for i in range(len(lines) - 1, -1, -1):
                if lines[i].strip().startswith('endif'):
                    insert_idx = i
                    break

        if insert_idx == -1:
            # No endif found, append at end
            with open(kconfig_file, 'a') as f:
                f.write('\n' + kconfig_entry + '\n')
        else:
            # Insert before the found line
            # Add blank line before entry if previous line is not blank
            if insert_idx > 0 and lines[insert_idx - 1].strip():
                lines.insert(insert_idx, '\n')
                insert_idx += 1
            lines.insert(insert_idx, kconfig_entry + '\n')
            # Add blank line after entry if next line is module=
            if insert_idx + 1 < len(lines) and (lines[insert_idx + 1].strip().startswith('module =') or
                                                  lines[insert_idx + 1].strip().startswith('module=')):
                lines.insert(insert_idx + 1, '\n')
            with open(kconfig_file, 'w') as f:
                f.writelines(lines)

        print(f"Updated: {kconfig_path}")
        return True

    except Exception as e:
        print(f"\nWarning: Failed to update {kconfig_path}: {e}")
        print("Manual step required: Add to Kconfig:")
        print(kconfig_entry)
        return False


def update_cmakelists(cmake_path, adapter_name, adapter_config):
    """
    Update adapters/CMakeLists.txt with new source
    Inserts before the blank line before endif()
    """
    cmake_file = Path(cmake_path)

    if not cmake_file.exists():
        print(f"\nWarning: {cmake_path} not found")
        print(f"Manual step required: Add to CMakeLists.txt:")
        print(f'    zephyr_library_sources_ifdef(CONFIG_{adapter_config} "src/{adapter_name}.c")')
        return False

    try:
        with open(cmake_file, 'r') as f:
            lines = f.readlines()

        new_line = f'    zephyr_library_sources_ifdef(CONFIG_{adapter_config} "src/{adapter_name}.c")\n'

        # Check for duplicate
        if any(adapter_name in line for line in lines):
            print(f"Info: {adapter_name} already exists in CMakeLists.txt, skipping")
            return True

        # Find the last zephyr_library_sources* line before endif()
        insert_idx = -1
        for i in range(len(lines) - 1, -1, -1):
            if 'zephyr_library_sources' in lines[i]:
                insert_idx = i + 1
                break

        if insert_idx == -1:
            # No library sources found, find endif() and insert before it
            for i in range(len(lines) - 1, -1, -1):
                if lines[i].strip().startswith('endif()'):
                    insert_idx = i
                    # Insert blank line before new entry if not already present
                    if i > 0 and lines[i-1].strip():
                        lines.insert(i, '\n')
                        insert_idx += 1
                    break

        if insert_idx == -1:
            # No endif found, append at end
            with open(cmake_file, 'a') as f:
                f.write(new_line)
        else:
            # Insert at the found position
            lines.insert(insert_idx, new_line)
            with open(cmake_file, 'w') as f:
                f.writelines(lines)

        print(f"Updated: {cmake_path}")
        return True

    except Exception as e:
        print(f"\nWarning: Failed to update {cmake_path}: {e}")
        print(f"Manual step required: Add to CMakeLists.txt:")
        print(f'    zephyr_library_sources_ifdef(CONFIG_{adapter_config} "{adapter_name}.c")')
        return False


def main():
    parser = argparse.ArgumentParser(description='Generate adapter boilerplate from zephlet protos')
    parser.add_argument('--zephlets-path', required=True, help='Path to zephlets directory')
    parser.add_argument('--output-dir', required=True, help='Output directory for adapter files')
    parser.add_argument('--interactive', action='store_true', help='Interactive mode')
    parser.add_argument('--origin', help='Origin zephlet name (non-interactive)')
    parser.add_argument('--destination', help='Destination zephlet name (non-interactive)')

    args = parser.parse_args()

    # Get templates directory (same dir as this script)
    script_dir = Path(__file__).parent
    templates_dir = script_dir / 'templates'

    if not templates_dir.exists():
        print(f"Error: Templates directory not found: {templates_dir}")
        sys.exit(1)

    # Discover zephlets
    print(f"Scanning zephlets in {args.zephlets_path}...")
    zephlets = discover_zephlets(args.zephlets_path)

    if not zephlets:
        print("No zephlets found")
        sys.exit(1)

    print(f"Found {len(zephlets)} zephlets")

    # Select zephlets
    if args.interactive:
        origin, dest = select_zephlets_interactive(zephlets)
        selected_fields = filter_report_fields(origin, interactive=True)
    else:
        if not args.origin or not args.destination:
            print("Error: --origin and --destination required in non-interactive mode")
            sys.exit(1)

        origin = next((z for z in zephlets if z['name'] == args.origin), None)
        dest = next((z for z in zephlets if z['name'] == args.destination), None)

        if not origin:
            print(f"Error: Origin zephlet '{args.origin}' not found")
            sys.exit(1)
        if not dest:
            print(f"Error: Destination zephlet '{args.destination}' not found")
            sys.exit(1)

        selected_fields = origin['report_fields']

    # Get destination API suggestions
    dest_api_suggestions = suggest_destination_api(dest)

    # Build context
    context = build_adapter_context(origin, dest, selected_fields, dest_api_suggestions)

    print(f"\nGenerating adapter: {context['adapter_name']}")

    # Render adapter
    adapter_path, kconfig_entry = render_adapter(context, args.output_dir, templates_dir)

    # Update Kconfig
    kconfig_path = Path(args.output_dir) / 'Kconfig'
    update_kconfig(kconfig_path, kconfig_entry)

    # Update CMakeLists.txt
    cmake_path = Path(args.output_dir) / 'CMakeLists.txt'
    update_cmakelists(cmake_path, context['adapter_name'], context['adapter_config'])

    print(f"\nAdapter generation complete!")
    print(f"\nNext steps:")
    print(f"1. Review generated file: {adapter_path}")
    print(f"2. Implement TODO comments in adapter function")
    print(f"3. Build: just c b r")
    print(f"\nNote: Adapter is enabled by default (CONFIG_{context['adapter_config']}=y)")


if __name__ == '__main__':
    main()
