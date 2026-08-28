# A plain zephyr_library that includes a generated zephlet header.
#
# Deliberately carries no add_dependencies() on the generators: the
# ordering has to come from zephyr_zephlet_generate() hooking them into
# `zephyr_generated_headers`. That is exactly what this fixture proves,
# so adding a dependency here would defeat the test.
zephyr_library()
zephyr_library_sources(consumer.c)
