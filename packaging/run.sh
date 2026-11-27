#!/bin/sh
set -eu
bundle_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# The HTTP listener serves Streamable HTTP MCP at /mcp on the same port.
exec "$bundle_dir/bambu-bridge" --plugin-dir "$bundle_dir/plugins" --http 8080 --no-stdio "$@"
