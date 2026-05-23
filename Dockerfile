FROM ghcr.io/zephyrproject-rtos/zephyr-build:main

USER root

# Codegen runtime deps + pytest deps for the zephlet test suites.
# Keeping these in the image avoids a pip install on every twister
# invocation. The image's venv lives at /opt/python/venv and is
# owned by root, hence the explicit USER root above.
RUN pip install --quiet \
        aiocoap pytest pytest-asyncio \
        proto-schema-parser jinja2 pkl-python copier \
        tree-sitter tree-sitter-c
