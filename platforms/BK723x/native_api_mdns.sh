#!/bin/sh
# The responder and its users must agree on the mdns_host structure layout.
# Apply to SDK sources before compiling; application-only -D flags are insufficient.
set -eu
sdk_dir="$1"
headers=$(find "$sdk_dir" -path '*/include/lwip/apps/mdns_opts.h' -type f)
if [ -z "$headers" ]; then
    echo "Native API: SDK mDNS configuration not found in $sdk_dir" >&2
    exit 1
fi
printf '%s\n' "$headers" | while IFS= read -r header; do
    sed -i 's/^#define MDNS_MAX_SERVICES[[:space:]]*1[[:space:]]*$/#define MDNS_MAX_SERVICES               2/' "$header"
    if ! grep -Eq '^#define MDNS_MAX_SERVICES[[:space:]]+2[[:space:]]*$' "$header"; then
        echo "Native API: unsupported MDNS_MAX_SERVICES configuration in $header" >&2
        exit 1
    fi
done
