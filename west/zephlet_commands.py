"""West extension commands for zephlet development tooling."""

from west.commands import WestCommand
from west import log
import subprocess
import sys
from pathlib import Path


class Zephlet(WestCommand):
    """Command class for zephlet development tools."""

    def __init__(self):
        super().__init__(
            'zephlet',
            'zephlet development tools',
            'Tooling for creating and managing zephlets')

    def do_add_parser(self, parser_adder):
        """Add argument parser for zephlet command and subcommands."""
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description)

        subparsers = parser.add_subparsers(
            dest='command',
            help='zephlet subcommands')

        # new subcommand
        new_parser = subparsers.add_parser(
            'new',
            help='create a new zephlet',
            description=(
                'Create a new zephlet from the Copier template. '
                'Destination defaults to the current working directory; '
                'override with --output-dir. The zephlet can live anywhere '
                'in the app tree.'))
        new_parser.add_argument(
            '-o', '--output-dir',
            help='parent directory for the new zephlet (default: cwd)')
        new_parser.add_argument(
            '-n', '--name',
            help='zephlet name (non-interactive mode)')
        new_parser.add_argument(
            '-d', '--description',
            help='zephlet description (non-interactive mode)')
        new_parser.add_argument(
            '-a', '--author',
            help='author name (non-interactive mode)')

        # new-adapter subcommand
        subparsers.add_parser(
            'new-adapter',
            help='print the v0.3 adapter recipe',
            description=(
                'Adapters are plain user code in v0.3 — the framework '
                'ships only the ZEPHLET_EVENTS_LISTENER macro. This '
                'subcommand prints the recommended layout.'))

        # gen subcommand
        gen_parser = subparsers.add_parser(
            'gen',
            help='regenerate zephlet interface files',
            description=(
                'Regenerate <prefix>_interface.{h,c} for an existing '
                'zephlet, given its source directory.'))
        gen_parser.add_argument(
            'zephlet_dir',
            help='path to the zephlet directory (contains <prefix>.proto)')
        gen_parser.add_argument(
            '--type',
            help=('zephlet type name; defaults to the directory basename '
                  '(e.g. a dir named "tick" implies --type=tick)'))
        gen_parser.add_argument(
            '--prefix',
            help=('file-name prefix; defaults to "zlet_<type>" '
                  '(so --type=tick produces zlet_tick_interface.{h,c})'))
        gen_parser.add_argument(
            '--output-dir',
            help=('output directory for generated interface files; '
                  'defaults to <zephlet_dir>/build (for ad-hoc generation '
                  'outside a west-build context)'))

        return parser

    def do_run(self, args, unknown):
        """Execute the requested subcommand."""
        if not args.command:
            log.die('no subcommand specified; try "west zephlet -h"')

        if args.command == 'new':
            self._new_zephlet(args)
        elif args.command == 'new-adapter':
            self._new_adapter(args)
        elif args.command == 'gen':
            self._gen_files(args)

    def _module_paths(self):
        """Return paths internal to the zephlet module (template, codegen)."""
        shared_dir = Path(__file__).parent.parent
        return {
            'codegen_dir': shared_dir / 'codegen',
            'template_dir': shared_dir / 'codegen' / 'zephyr_zephlet_template',
        }

    def _check_dependencies(self, required_packages):
        """Check if required Python packages are installed."""
        missing = []
        for package in required_packages:
            try:
                __import__(package.replace('-', '_'))
            except ImportError:
                missing.append(package)

        if missing:
            log.die(f'missing required Python packages: {", ".join(missing)}\n'
                   f'Install with: pip install {" ".join(missing)}')

    def _new_zephlet(self, args):
        """Create a new zephlet using copier template."""
        self._check_dependencies(['copier'])

        module = self._module_paths()

        if not module['template_dir'].exists():
            log.die(f'template directory not found: {module["template_dir"]}')

        dest = Path(args.output_dir).resolve() if args.output_dir else Path.cwd()
        if not dest.exists():
            log.die(f'destination directory does not exist: {dest}\n'
                    f'Create it first (mkdir -p) or choose a different path.')

        cmd = ['copier', 'copy', str(module['template_dir']), str(dest)]

        if args.name:
            cmd.extend(['--data', f'zephlet_name={args.name}'])
            if args.description:
                cmd.extend(['--data', f'description={args.description}'])
            if args.author:
                cmd.extend(['--data', f'author_name={args.author}'])
            cmd.append('--defaults')

        log.inf(f'Creating new zephlet under {dest}...')
        result = subprocess.run(cmd)
        if result.returncode != 0:
            log.die('failed to create zephlet')

        if args.name:
            zephlet_dir = dest / args.name.lower()
            log.inf(f'Zephlet "{args.name}" created at {zephlet_dir}')
            log.inf('Next steps:')
            log.inf(f'  1. Edit {zephlet_dir}/zlet_{args.name.lower()}.proto')
            log.inf(f'  2. Add "{zephlet_dir}" to the app\'s EXTRA_ZEPHYR_MODULES')
            log.inf(f'  3. Enable CONFIG_ZEPHLET_{args.name.upper()}=y in prj.conf')
            log.inf(f'  4. Build (west build -b <board>)')

    def _new_adapter(self, args):
        """
        v0.3 adapters are not generated — they are ~8 lines of C
        written by hand, under a Kconfig+CMake guard that ties the
        adapter's build to the participating zephlets. Print the
        recipe and exit.
        """
        log.inf('v0.3 adapters are written by hand using the')
        log.inf('ZEPHLET_EVENTS_LISTENER macro. Recommended layout:')
        log.inf('')
        log.inf('  <app>/adapters/tick_to_ui/')
        log.inf('    Kconfig')
        log.inf('      config TICK_TO_UI_ADAPTER')
        log.inf('          bool "Tick -> UI adapter"')
        log.inf('          depends on ZEPHLET_TICK && ZEPHLET_UI')
        log.inf('')
        log.inf('    CMakeLists.txt')
        log.inf('      if(CONFIG_TICK_TO_UI_ADAPTER)')
        log.inf('          zephyr_library()')
        log.inf('          zephyr_library_sources(tick_to_ui.c)')
        log.inf('      endif()')
        log.inf('')
        log.inf('    tick_to_ui.c')
        log.inf('      #include "zlet_tick.h"')
        log.inf('      #include "zlet_ui.h"')
        log.inf('      static void on_tick(const struct tick_events *ev) { /* ... */ }')
        log.inf('      ZEPHLET_EVENTS_LISTENER(tick_instance, tick, on_tick);')
        log.inf('')
        log.inf('The Kconfig guard ensures the adapter only compiles when both')
        log.inf('zephlets are enabled. The adapter directory can live anywhere')
        log.inf('in the app tree — the framework takes no position.')

    def _gen_files(self, args):
        """Regenerate <prefix>_interface.{h,c} for an existing zephlet."""
        self._check_dependencies(['proto-schema-parser', 'jinja2'])

        module = self._module_paths()
        script = module['codegen_dir'] / 'generate_zephlet.py'
        if not script.exists():
            log.die(f'generate_zephlet.py not found: {script}')

        zephlet_dir = Path(args.zephlet_dir).resolve()
        if not zephlet_dir.is_dir():
            log.die(f'not a directory: {zephlet_dir}')

        type_ = args.type or zephlet_dir.name
        prefix = args.prefix or f'zlet_{type_}'
        proto = zephlet_dir / f'{prefix}.proto'
        if not proto.exists():
            log.die(f'proto file not found: {proto}')

        output_dir = (
            Path(args.output_dir).resolve() if args.output_dir
            else zephlet_dir / 'build')

        cmd = [
            sys.executable, str(script),
            '--proto', str(proto),
            '--output-dir', str(output_dir),
            '--type', type_,
            '--prefix', prefix,
        ]

        log.inf(f'Regenerating interface files for "{type_}"...')
        result = subprocess.run(cmd)
        if result.returncode != 0:
            log.die('failed to regenerate files')

        log.inf(f'Interface files written to: {output_dir}')
