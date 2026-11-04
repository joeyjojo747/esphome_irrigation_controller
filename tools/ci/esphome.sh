#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
shopt -s nullglob

# secrets.yaml is git-ignored; CI uses the dummy example values.
cp esphome/secrets.yaml.example esphome/secrets.yaml
cp esphome/secrets.yaml.example tests/compile/secrets.yaml

for f in tests/compile/*.yaml; do
  case "$f" in
    */secrets.yaml) continue ;;
    *_bad_*.yaml)
      expect=$(sed -n 's/^# expect: *//p' "$f" | head -n1)
      if [ -z "$expect" ]; then echo "FAIL: $f has no '# expect:' comment"; exit 1; fi
      if out=$(esphome config "$f" 2>&1); then
        echo "FAIL: $f should have failed esphome config"; echo "$out"; exit 1
      fi
      if ! grep -qF -- "$expect" <<<"$out"; then
        echo "FAIL: $f failed but output lacks expected text: $expect"; echo "$out"; exit 1
      fi
      echo "OK (expected failure): $f"
      ;;
    *)
      esphome config "$f"
      esphome compile "$f"
      ;;
  esac
done

# The user-facing wrapper, without the expander (the fixture above covers the expander).
esphome config esphome/irrigation-controller.yaml
esphome compile esphome/irrigation-controller.yaml
