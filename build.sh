#!/bin/sh
# Builds the firmware, always from the project root.
#
# ESP-IDF silently misbehaves when idf.py runs from a component directory, so
# this wrapper removes the chance of that happening.
set -e
cd "$(dirname "$0")"
. "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1

# SRC_DIRS globs at configure time, so a newly added source is silently left out
# until CMake reruns. Reconfiguring first costs a couple of seconds and removes
# a confusing class of "undefined reference" failures.
if [ "${1:-build}" = "build" ]; then
    idf.py reconfigure > /dev/null
fi
exec idf.py "${@:-build}"
