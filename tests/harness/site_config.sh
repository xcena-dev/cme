# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# site_config.sh -- read config.yaml from a shell script.
#
# The third reader of the same file, after cmake/SiteConfig.cmake and the
# config_reader.hpp beside this. config.yaml is restricted to flat `key: value` lines
# precisely so that each of the three can read it with what it already has, and
# none of them needs a YAML parser.
#
# A missing file or a missing key yields an empty string, which every caller
# treats as "this machine does not have that". Override the location with
# CME_SITE_CONFIG.
#
# usage:
#   . "$(dirname "${BASH_SOURCE[0]}")/site_config.sh"
#   dev="$(site_get dax_device)"

CME_SITE_CONFIG="${CME_SITE_CONFIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/config.yaml}"

site_get() {
    local key="$1"
    [ -f "$CME_SITE_CONFIG" ] || return 0
    sed -n "s/^[[:space:]]*${key}[[:space:]]*:[[:space:]]*//p" "$CME_SITE_CONFIG" \
        | sed -e 's/[[:space:]]\{1,\}#.*$//' \
              -e 's/[[:space:]]*$//' \
              -e 's/^"\(.*\)"$/\1/' \
              -e "s/^'\(.*\)'\$/\1/" \
        | head -n1
}

# True when $1 is a mount point, by the same rule as harness::isMountPoint: a
# mount point's device differs from its parent's. A configured directory that is
# not mounted is an ordinary directory on the root filesystem, so writing to it
# would measure the wrong medium rather than fail.
site_is_mounted() {
    local path="$1" here parent
    [ -n "$path" ] && [ -d "$path" ] || return 1
    here="$(stat -c %d "$path" 2>/dev/null)" || return 1
    parent="$(stat -c %d "$path/.." 2>/dev/null)" || return 1
    [ "$here" != "$parent" ]
}

# Same, but fail loudly: for a value the caller cannot proceed without.
site_require() {
    local key="$1" value
    value="$(site_get "$key")"
    if [ -z "$value" ]; then
        echo "$0: '$key' is empty in $CME_SITE_CONFIG" >&2
        echo "  copy config.example.yaml to config.yaml and fill it in" >&2
        exit 2
    fi
    printf '%s' "$value"
}
