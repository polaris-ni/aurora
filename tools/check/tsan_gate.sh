#!/usr/bin/env bash
# ============================================================================
# tsan_gate.sh - targeted ThreadSanitizer regression gate
# ----------------------------------------------------------------------------
# Runs the four network suites that cover the fixed data races, then fails on
# either a non-zero ctest exit code or any race report written to log_path.
#
# The report body is the only trustworthy counter:
#   - a data race is WARNING-level and does not fail a test assertion;
#   - ctest does not echo a passing suite's stderr, so --output-log sees zero;
#   - halt_on_error=1 does not guarantee a non-zero process exit.
# Never put TSAN_OPTIONS exitcode=0 here: it turns a red run green.
#
# Usage: tsan_gate.sh [build_dir]
#   AURORA_TSAN_REPORT_DIR  required; absolute report/log directory. Set it to
#                           a per-run scratch directory (CI: runner.temp).
#   PATH                    ctest is resolved from PATH; a stand-in in front of
#                           it is how the verdict is mutation-tested locally.
# ============================================================================
set -o pipefail

build_dir="${1:-build-tsan}"
report_dir="${AURORA_TSAN_REPORT_DIR:-}"

if [ -z "${report_dir}" ]; then
  echo "TSAN_GATE=FAIL AURORA_TSAN_REPORT_DIR is not set"
  exit 2
fi

report_prefix="${report_dir}/report"
ctest_log="${report_dir}/ctest.log"
case_re='^(utest_net_http_await|utest_net_http_client|utest_net_http_sse|utest_net_socket)$'

mkdir -p "${report_dir}" || exit 2
rm -f "${report_prefix}".* "${ctest_log}"

TSAN_OPTIONS="ignore_noninstrumented_modules=1:log_path=${report_prefix}:history_size=7" \
  ctest --test-dir "${build_dir}" --output-on-failure -C Release -j "$(nproc)" \
    -R "${case_re}" > "${ctest_log}" 2>&1
ctest_rc=$?

tail -30 "${ctest_log}"

race_count=$(grep -h 'WARNING: ThreadSanitizer: data race' "${report_prefix}".* 2>/dev/null | wc -l)
race_count=${race_count:-0}
echo "CTEST_EXIT=${ctest_rc} RACE_COUNT=${race_count}"

if [ "${ctest_rc}" -ne 0 ] || [ "${race_count}" -ne 0 ]; then
  echo "TSAN_GATE=FAIL"
  exit 1
fi

echo "TSAN_GATE=PASS"
