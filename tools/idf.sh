#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_CHECKOUT="${KORVO_IDF_PATH:-/home/lucinhu/esp/esp-idf/esp-idf-main}"
unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_IDF_VERSION IDF_TOOLS_EXPORT_CMD
source "$IDF_CHECKOUT/export.sh" >/dev/null
cd "$ROOT"
export ESPPORT="${ESPPORT:-/dev/ttyUSB0}"
# IDF custom partition flash targets read ESPPORT/ESPBAUD instead of CLI -p/-b.
KORVO_ARGS=("$@")
for ((i=0; i<${#KORVO_ARGS[@]}; ++i)); do
    if [[ "${KORVO_ARGS[i]}" == "-p" && $((i+1)) -lt ${#KORVO_ARGS[@]} ]]; then
        export ESPPORT="${KORVO_ARGS[i+1]}"
    elif [[ "${KORVO_ARGS[i]}" == "-b" && $((i+1)) -lt ${#KORVO_ARGS[@]} ]]; then
        export ESPBAUD="${KORVO_ARGS[i+1]}"
    fi
done
exec idf.py "$@"
