#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

/**
 * @file
 * @brief The `zlet` shell root command.
 *
 * Defined exactly once here, unconditionally (compiled whenever
 * CONFIG_ZEPHLETS_SHELL=y). Each instance adds itself as a child under
 * this root via ZLET_SHELL_INSTANCE()'s `SHELL_SUBCMD_ADD((zlet), ...)`
 * call — invoked automatically by ZEPHLET_NEW(...) through the
 * `_ZLET_SHELL_HOOK_<type>` hook, from however many translation units
 * declare zephlet instances. No manifest of instance names is needed;
 * this mirrors the in-tree `kernel`/`thread` shell commands
 * (zephyr/subsys/shell/modules/kernel_service/).
 */
/* Every RPC leaf takes its text-format message as a SHELL_OPT_ARG_RAW
 * argument. Wildcard expansion rewrites `cmd_buff` before any handler
 * runs, so a `*` or `?` anywhere in that argument — including inside a
 * quoted string value, which the shell has no reason to treat as quoted —
 * silently replaces it with text the user never typed. There is no
 * handler-side defence: by the time the handler is called, the damage is
 * done. Kconfig `select` cannot force a symbol off, so it is asserted
 * here instead. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SHELL_WILDCARD),
	     "CONFIG_ZEPHLETS_SHELL requires CONFIG_SHELL_WILDCARD=n: wildcard expansion "
	     "corrupts an RPC's raw text-format argument before the handler sees it.");

SHELL_SUBCMD_SET_CREATE(zlet_shell_root_cmds, (zlet));
SHELL_CMD_REGISTER(zlet, &zlet_shell_root_cmds, "Invoke a zephlet instance's RPC.", NULL);
