# Zephlet infra — dev tasks. Run from this directory (modules/lib/zephlet/).

docker_image := "zephlet-tester:latest"

# Workspace root is three levels up from this justfile (the west topdir
# that contains `.west/`, `zephyr/`, `ports_adapters_zbus/`, and the
# `modules/lib/zephlet/` infra checkout).
workspace_root := justfile_directory() + "/../../.."

default:
    @just --list

# Build the local image baked with codegen + pytest Python deps.
# Re-run whenever the upstream base image or the pinned deps change.
# Recent Docker Desktop prints a legacy-builder deprecation notice;
# install the `buildx` plugin (Docker Desktop → Settings → Builders,
# or `brew install docker-buildx`) to silence it. The legacy builder
# still works otherwise.
docker-build:
    docker build -t {{ docker_image }} {{ justfile_directory() }}

# Run the full test suite inside the local image. `platform` defaults
# to native_sim/native/64 (the variant aarch64 hosts require); pass
# `native_sim` explicitly for x86_64 hosts.
docker-test platform="native_sim/native/64":
    docker run --rm -u root \
        --cap-add=NET_ADMIN --device=/dev/net/tun \
        -v {{ workspace_root }}:/workdir -w /workdir \
        {{ docker_image }} \
        west twister \
            --testsuite-root modules/lib/zephlet/tests \
            -p {{ platform }} \
            -O /tmp/twister-out \
            --inline-logs

# Drop into an interactive shell inside the image with the workspace
# mounted — useful for debugging a single failing case.
docker-shell:
    docker run --rm -ti -u root \
        --cap-add=NET_ADMIN --device=/dev/net/tun \
        -v {{ workspace_root }}:/workdir -w /workdir \
        {{ docker_image }} \
        bash
