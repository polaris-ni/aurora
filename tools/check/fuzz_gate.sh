#!/usr/bin/env bash
# ============================================================================
# fuzz_gate.sh - verdict carrier for the resident libFuzzer targets
# ----------------------------------------------------------------------------
# Step 1 is a health check that MUST go red. fuzz_effectiveness deliberately
# contains one UBSan signed overflow and one ASan out-of-bounds read; its fixed
# seeds must make the sanitizer runtime abort and write a crash artifact. If it
# passes, the instrumentation chain is broken and every "zero findings" from
# the five real targets is meaningless, so the whole gate fails.
#
# Step 2 runs each real target for a short bounded session. A target is green
# only when all four signals agree: zero exit code, no crash artifact, no
# sanitizer report in the log, and a normal "Done N runs" epilogue.
#
# False-green traps this script exists to close:
#   - UBSan prints but does not abort by default; halt_on_error=1 and
#     abort_on_error=1 are mandatory, and step 1 proves they are in effect.
#   - libFuzzer prints "Done N runs" only on normal termination; a crashed run
#     has no epilogue, so its absence is itself a red signal.
#   - A passing target's stdout is not echoed into the job log; logs are written
#     to disk and grepped instead of being eyeballed.
#   - Stale artifacts from a previous run must be purged first, otherwise every
#     run is red for the wrong reason and a genuinely broken chain goes unseen.
#   - libFuzzer writes evolved units back into its corpus directory, so the
#     checked-in seeds are copied into a per-run scratch corpus and stay read-only.
#
# Usage: fuzz_gate.sh [build_dir] [seconds_per_target]
#   AURORA_FUZZ_ARTIFACT_DIR  required; per-run scratch directory (CI: runner.temp)
#   AURORA_FUZZ_CORPUS_DIR    optional; defaults to <repo>/tests/fuzz/corpus
#   AURORA_FUZZ_BIN_DIR       optional; defaults to <build_dir>; a stand-in
#                             directory in front is how the verdict is
#                             mutation-tested locally.
#   AURORA_FUZZ_TARGETS       optional; overrides the five real target names.
# ============================================================================
set -o pipefail

build_dir="${1:-build-fuzz}"
seconds="${2:-90}"
artifact_dir="${AURORA_FUZZ_ARTIFACT_DIR:-}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
corpus_dir="${AURORA_FUZZ_CORPUS_DIR:-${repo_dir}/tests/fuzz/corpus}"
bin_dir="${AURORA_FUZZ_BIN_DIR:-${build_dir}}"
targets="${AURORA_FUZZ_TARGETS:-fuzz_uri_list fuzz_json fuzz_http_wire fuzz_ws_frame fuzz_bmp}"

if [ -z "${artifact_dir}" ]; then
  echo "FUZZ_GATE=FAIL AURORA_FUZZ_ARTIFACT_DIR is not set"
  exit 2
fi

mkdir -p "${artifact_dir}" || exit 2
rm -rf "${artifact_dir:?}"/*

asan_options='detect_leaks=0:abort_on_error=1:symbolize=1:allocator_may_return_null=1'
ubsan_options='print_stacktrace=1:halt_on_error=1:abort_on_error=1'
report_re='ERROR: AddressSanitizer|runtime error:|SUMMARY: .*Sanitizer|libFuzzer: deadly signal'

count_artifacts() {
  local dir="$1" count=0
  for kind in crash oom timeout leak; do
    # shellcheck disable=SC2086
    count=$((count + $(find "${dir}" -maxdepth 1 -type f -name "${kind}-*" 2>/dev/null | wc -l)))
  done
  echo "${count}"
}

# ---- Step 1: the health check must go red -----------------------------------
eff_bin="${bin_dir}/fuzz_effectiveness"
eff_out="${artifact_dir}/effectiveness"
eff_log="${artifact_dir}/effectiveness.log"
mkdir -p "${eff_out}" || exit 2

if [ ! -x "${eff_bin}" ]; then
  echo "FUZZ_GATE=FAIL missing binary ${eff_bin}"
  exit 2
fi
cp -r "${corpus_dir}/fuzz_effectiveness" "${artifact_dir}/effectiveness-corpus" 2>/dev/null || {
  echo "FUZZ_GATE=FAIL missing effectiveness seed corpus"
  exit 2
}

ASAN_OPTIONS="${asan_options}" UBSAN_OPTIONS="${ubsan_options}" \
  "${eff_bin}" -runs=200 -artifact_prefix="${eff_out}/" \
    "${artifact_dir}/effectiveness-corpus" > "${eff_log}" 2>&1
eff_rc=$?

eff_artifacts=$(count_artifacts "${eff_out}")
eff_reports=$(grep -Ec "${report_re}" "${eff_log}" 2>/dev/null || true)
eff_reports=${eff_reports:-0}
echo "EFFECTIVENESS_EXIT=${eff_rc} ARTIFACTS=${eff_artifacts} REPORTS=${eff_reports}"
tail -12 "${eff_log}"

if [ "${eff_artifacts}" -eq 0 ] || [ "${eff_reports}" -eq 0 ]; then
  echo "FUZZ_GATE=FAIL instrumentation chain is not effective; zero findings would be meaningless"
  exit 1
fi

# ---- Step 2: short bounded sessions on the five real targets ---------------
overall=0
for name in ${targets}; do
  bin="${bin_dir}/${name}"
  out="${artifact_dir}/${name}"
  log="${artifact_dir}/${name}.log"
  work_corpus="${artifact_dir}/corpus/${name}"
  mkdir -p "${out}" "${work_corpus}" || exit 2

  if [ ! -x "${bin}" ]; then
    echo "FUZZ_GATE=FAIL missing binary ${bin}"
    exit 2
  fi
  if [ ! -d "${corpus_dir}/${name}" ]; then
    echo "FUZZ_GATE=FAIL missing seed corpus ${corpus_dir}/${name}"
    exit 2
  fi
  cp "${corpus_dir}/${name}"/* "${work_corpus}/" 2>/dev/null || true

  ASAN_OPTIONS="${asan_options}" UBSAN_OPTIONS="${ubsan_options}" \
    timeout "$((seconds + 60))" "${bin}" \
      -max_total_time="${seconds}" -max_len=4096 -rss_limit_mb=2048 -timeout=10 \
      -artifact_prefix="${out}/" -print_final_stats=1 \
      "${work_corpus}" > "${log}" 2>&1
  rc=$?

  artifacts=$(count_artifacts "${out}")
  reports=$(grep -Ec "${report_re}" "${log}" 2>/dev/null || true)
  reports=${reports:-0}
  if grep -Eq 'Done [0-9]+ runs' "${log}" 2>/dev/null; then done_line=1; else done_line=0; fi

  echo "TARGET=${name} EXIT=${rc} ARTIFACTS=${artifacts} REPORTS=${reports} EPILOGUE=${done_line}"
  if [ "${rc}" -ne 0 ] || [ "${artifacts}" -ne 0 ] || [ "${reports}" -ne 0 ] || [ "${done_line}" -ne 1 ]; then
    tail -20 "${log}"
    overall=1
  fi
done

if [ "${overall}" -ne 0 ]; then
  echo "FUZZ_GATE=FAIL"
  exit 1
fi

echo "FUZZ_GATE=PASS"
