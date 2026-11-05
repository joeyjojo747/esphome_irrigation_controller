#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
pytest
# HA check_config on the packages (minimal config, default_config omitted).
python -m homeassistant --script check_config -c tests/ha/config
