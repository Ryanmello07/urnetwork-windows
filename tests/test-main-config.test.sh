#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../test-main-config.sh
source "$here/../test-main-config.sh"
runner="$here/../test-main.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ "$(windows_acceptance_build_timeout)" = 7200 ] || fail "default build timeout is not two hours"
[ "$(UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=9000 windows_acceptance_build_timeout)" = 9000 ] || fail "override was not honored"
if UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=invalid windows_acceptance_build_timeout >/dev/null 2>&1; then
  fail "invalid build timeout was accepted"
fi
if UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=0 windows_acceptance_build_timeout >/dev/null 2>&1; then
  fail "zero build timeout was accepted"
fi

control_agent_marker='echo "[windows acceptance] building the local SDK control agent"'
control_agent_line="$(grep -nF "$control_agent_marker" "$runner" | cut -d: -f1 || true)"
[[ "$control_agent_line" =~ ^[0-9]+$ ]] || fail "could not locate the Windows control-agent build boundary"
control_agent_command="$(
  sed -n "${control_agent_line},$((control_agent_line + 3))p" "$runner"
)"
for required in \
  'CGO_ENABLED=0' \
  'GOOS=windows' \
  'GOARCH=arm64' \
  'timeout 600 go build -mod=readonly -trimpath' \
  "-o \"\$run_dir/agent.exe\" ."; do
  count="$(printf '%s\n' "$control_agent_command" | grep -cF -- "$required" || true)"
  [ "$count" -eq 1 ] || fail "Windows control-agent build must carry exactly one $required boundary (found $count)"
done

echo "PASS: Windows acceptance build timeout"
