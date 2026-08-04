# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# script_args.sh -- `--flag value` parsing for the shell scripts under tests/.
#
# Sourced, not run. It exists so that every script here takes options the way the binaries
# it drives take them, and so that one place does the checks. An environment variable
# cannot be wrong: REPEAT=5 where the script reads REPEATS is silently the default, and the
# run then reports a number nobody asked for. A flag can be, and is.
#
# usage:
#   . "$(dirname "${BASH_SOURCE[0]}")/../harness/script_args.sh"

# script_parse_args USAGE "valued names" "toggle names" "$@"
#
#   --flag value   sets FLAG=value, for a name in the valued list
#   --flag         sets FLAG=1, for a name in the toggle list
#   --no-flag      sets FLAG=0, for the same
#   -h, --help     prints USAGE and exits 0
#
# A name becomes its variable by upper-casing and turning '-' into '_', so --cs-sleep sets
# CS_SLEEP. The script declares the default before calling this; nothing here invents one.
script_parse_args()
{
    local usage="$1" valued="$2" toggles="$3" name variable candidate
    shift 3
    while [ $# -gt 0 ]; do
        case "$1" in
        -h | --help)
            printf '%s\n' "$usage"
            exit 0
            ;;
        --no-*)
            name="${1#--no-}"
            if ! script_names_include "$name" "$toggles"; then
                script_reject "$usage" "unknown option $1"
            fi
            declare -g "$(script_variable_name "$name")=0"
            shift
            ;;
        --*)
            name="${1#--}"
            variable="$(script_variable_name "$name")"
            if script_names_include "$name" "$toggles"; then
                declare -g "$variable=1"
                shift
                continue
            fi
            if ! script_names_include "$name" "$valued"; then
                script_reject "$usage" "unknown option $1"
            fi
            if [ $# -lt 2 ]; then
                script_reject "$usage" "$1 needs a value"
            fi
            declare -g "$variable=$2"
            shift 2
            ;;
        *)
            script_reject "$usage" "unexpected argument $1"
            ;;
        esac
    done
}

script_variable_name()
{
    printf '%s' "$1" | tr 'a-z-' 'A-Z_'
}

script_names_include()
{
    local wanted="$1" candidate
    for candidate in $2; do
        if [ "$wanted" = "$candidate" ]; then
            return 0
        fi
    done
    return 1
}

# script_reject USAGE MESSAGE -- the caller writes the sentence, so word order stays right
# whether the flag comes first or last in it.
script_reject()
{
    printf '%s: %s\n%s\n' "$0" "$2" "$1" >&2
    exit 2
}

# One of a fixed set, or exit naming what was allowed. @flag is for the message only.
script_require_choice()
{
    local flag="$1" value="$2" allowed="$3"
    if script_names_include "$value" "$allowed"; then
        return 0
    fi
    echo "$0: $flag must be one of: $allowed (got '$value')" >&2
    exit 2
}

# Every value in @list must be in @allowed, for a flag that takes a set rather than one
# name. A typo in one entry of a sweep set would otherwise run a shorter sweep in silence.
script_require_choices()
{
    local flag="$1" values="$2" allowed="$3" value
    for value in $values; do
        script_require_choice "$flag" "$value" "$allowed"
    done
}
