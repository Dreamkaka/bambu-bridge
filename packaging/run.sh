#!/bin/sh
set -eu
bundle_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec "$bundle_dir/bambu-bridge" --plugin-dir "$bundle_dir/plugins" --http 8080 "$@"
