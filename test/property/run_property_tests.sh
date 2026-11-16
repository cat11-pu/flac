#!/usr/bin/env bash
set -u -o pipefail

usage() {
    cat <<'EOF'
usage: run_property_tests.sh [options]
  --binary PATH       flac_property executable
  --work DIR          writable work directory
  --report DIR        output report directory
  --ffmpeg PATH       ffmpeg executable
  --ffprobe PATH      ffprobe executable
  --python PATH       Python 3 executable
  --source-dir DIR    directory containing the Python generators
  --malformed-count N default: 200
  --valid-count N     default: 50
  --timeout SECONDS   per-file timeout, default: 15
EOF
}

BINARY=""
WORK=""
REPORT=""
FFMPEG=""
FFPROBE=""
PYTHON=""
SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"
MALFORMED_COUNT=200
VALID_COUNT=50
PER_FILE_TIMEOUT=15

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) BINARY=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --report) REPORT=$2; shift 2 ;;
        --ffmpeg) FFMPEG=$2; shift 2 ;;
        --ffprobe) FFPROBE=$2; shift 2 ;;
        --python) PYTHON=$2; shift 2 ;;
        --source-dir) SOURCE_DIR=$2; shift 2 ;;
        --malformed-count) MALFORMED_COUNT=$2; shift 2 ;;
        --valid-count) VALID_COUNT=$2; shift 2 ;;
        --timeout) PER_FILE_TIMEOUT=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown or incomplete argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$BINARY" ] || [ ! -x "$BINARY" ]; then
    echo "--binary must point to an executable" >&2
    exit 2
fi
for value in "$WORK" "$REPORT" "$FFMPEG" "$FFPROBE" "$PYTHON"; do
    if [ -z "$value" ]; then
        echo "missing required option" >&2
        usage >&2
        exit 2
    fi
done
for tool in "$FFMPEG" "$FFPROBE" "$PYTHON"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "tool not found: $tool" >&2
        exit 2
    fi
done

mkdir -p "$WORK" "$REPORT"
rm -rf "$WORK/malformed" "$WORK/valid" "$WORK/malformed-logs" "$WORK/valid-logs"
rm -rf "$WORK/valid-pcm" "$REPORT/counterexamples"
rm -f "$WORK/malformed_results.tsv" "$WORK/valid_results.tsv" "$REPORT/metadata.tsv"
mkdir -p "$WORK/malformed-logs" "$WORK/valid-logs" "$WORK/valid-pcm"

if strings "$BINARY" | grep -E '__asan_init|__ubsan_handle' >/dev/null 2>&1; then
    SANITIZER='ASan+UBSan+LSan'
else
    SANITIZER='none'
fi
FFMPEG_VERSION="$("$FFMPEG" -version 2>/dev/null | head -1)"
GIT_COMMIT="$(git -C "$SOURCE_DIR/../.." rev-parse HEAD 2>/dev/null || echo unknown)"
{
    printf 'sanitizer\t%s\n' "$SANITIZER"
    printf 'ffmpeg_version\t%s\n' "$FFMPEG_VERSION"
    printf 'git_commit\t%s\n' "$GIT_COMMIT"
} > "$REPORT/metadata.tsv"

ASAN_OPTIONS_VALUE="detect_leaks=1:halt_on_error=1:abort_on_error=0:exitcode=99:symbolize=1"
UBSAN_OPTIONS_VALUE="halt_on_error=1:print_stacktrace=1"

echo "generating seed and corpus..."
"$FFMPEG" -v error -nostdin -y -f lavfi \
    -i 'aevalsrc=0.32*sin(2*PI*440*t)|0.27*sin(2*PI*660*t):s=44100:d=0.5:c=stereo' \
    -c:a flac -sample_fmt s16 -fflags +bitexact -flags:a +bitexact \
    "$WORK/seed.flac" || exit 2

"$PYTHON" "$SOURCE_DIR/generate_corpus.py" \
    --seed "$WORK/seed.flac" --out "$WORK/malformed" --count "$MALFORMED_COUNT" || exit 2
"$PYTHON" "$SOURCE_DIR/generate_valid.py" \
    --out "$WORK/valid" --ffmpeg "$FFMPEG" --ffprobe "$FFPROBE" \
    --count "$VALID_COUNT" || exit 2

parse_field() {
    local line=$1
    local key=$2
    printf '%s\n' "$line" | sed -n "s/.*\\t${key}=\\([^\\t]*\\).*/\\1/p"
}

first_death_offset() {
    local log=$1
    sed -n 's/.*FLAC_PROPERTY_SANITIZER_DEATH.*offset=\([0-9][0-9]*\).*/\1/p' "$log" | head -1
}

classify_failure() {
    local log=$1
    local rc=$2
    if grep -Eq 'heap-buffer-overflow|stack-buffer-overflow|global-buffer-overflow|use-after-free|buffer-overflow' "$log"; then
        echo oob
    elif grep -Eq 'LeakSanitizer|detected memory leaks' "$log"; then
        echo leak
    elif grep -Eq 'runtime error:|UndefinedBehaviorSanitizer' "$log"; then
        echo ub
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        echo timeout
    elif [ "$rc" -eq 5 ]; then
        echo invariant
    elif grep -Eq 'AddressSanitizer|Sanitizer|FLAC_PROPERTY_SANITIZER_DEATH|Segmentation fault|Aborted' "$log"; then
        echo crash
    else
        echo harness_failure
    fi
}

printf 'id\tcategory\tvariant\tpath\tsize\tinjected_offset\tstatus\treported_offset\tsemantic_illegal\terrors\tstate\tfirst_error\trc\tfailure_kind\n' \
    > "$WORK/malformed_results.tsv"
printf 'id\tpath\tchannels\tbps\trate\tsamples\tstatus\tcompare\tlib_samples\tref_samples\tsample_index\tframe\tchannel\tbyte_offset\trc\n' \
    > "$WORK/valid_results.tsv"

echo "running $MALFORMED_COUNT malformed cases..."
malformed_index=0
malformed_failures=0
while IFS=$'\t' read -r id category variant path size injected; do
    [ "$id" = "id" ] && continue
    stdout="$WORK/malformed-logs/${id}.stdout"
    log="$WORK/malformed-logs/${id}.stderr"
    set +e
    env ASAN_OPTIONS="$ASAN_OPTIONS_VALUE" UBSAN_OPTIONS="$UBSAN_OPTIONS_VALUE" \
        timeout "${PER_FILE_TIMEOUT}s" "$BINARY" --expect malformed "$path" \
        > "$stdout" 2> "$log"
    rc=$?
    set -e
    line="$(cat "$stdout")"
    decode="$(parse_field "$line" decode)"
    offset="$(parse_field "$line" offset)"
    semantic="$(parse_field "$line" semantic_illegal)"
    errors="$(parse_field "$line" errors)"
    state="$(parse_field "$line" state_name)"
    first_error="$(parse_field "$line" first_error)"
    invariant="$(parse_field "$line" invariant)"
    failure_kind=""

    if [ "$rc" -ne 0 ] || [ -z "$line" ]; then
        failure_kind="$(classify_failure "$log" "$rc")"
        status="$failure_kind"
        if [ -z "$offset" ]; then
            offset="$(first_death_offset "$log")"
        fi
        malformed_failures=$((malformed_failures + 1))
        printf 'FAIL %s %s %s rc=%s kind=%s offset=%s\n' \
            "$id" "$category" "$variant" "$rc" "$failure_kind" "${offset:-unknown}" >&2
    elif [ "$decode" = "false" ]; then
        status=rejected
    elif [ -n "$errors" ] && [ "$errors" != "0" ]; then
        status=recovered
    else
        status=accepted
    fi
    [ -z "$offset" ] && offset=NA
    [ -z "$semantic" ] && semantic=NA
    [ -z "$errors" ] && errors=NA
    [ -z "$state" ] && state=NA
    [ -z "$first_error" ] && first_error=NA
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$id" "$category" "$variant" "$path" "$size" "$injected" "$status" \
        "$offset" "$semantic" "$errors" "$state" "$first_error" "$rc" "$failure_kind" \
        >> "$WORK/malformed_results.tsv"
    malformed_index=$((malformed_index + 1))
    if [ $((malformed_index % 25)) -eq 0 ]; then
        echo "  malformed $malformed_index/$MALFORMED_COUNT failures=$malformed_failures"
    fi
done < "$WORK/malformed/manifest.tsv"

echo "running $VALID_COUNT valid differential cases..."
valid_index=0
valid_failures=0
while IFS=$'\t' read -r id path manifest_channels manifest_bps manifest_rate manifest_samples compression chmode; do
    [ "$id" = "id" ] && continue
    lib_pcm="$WORK/valid-pcm/${id}.lib.s32"
    ref_pcm="$WORK/valid-pcm/${id}.ref.s32"
    stdout="$WORK/valid-logs/${id}.stdout"
    log="$WORK/valid-logs/${id}.stderr"
    set +e
    env ASAN_OPTIONS="$ASAN_OPTIONS_VALUE" UBSAN_OPTIONS="$UBSAN_OPTIONS_VALUE" \
        timeout "${PER_FILE_TIMEOUT}s" "$BINARY" --expect valid --dump "$lib_pcm" "$path" \
        > "$stdout" 2> "$log"
    harness_rc=$?
    set -e

    channels="$(parse_field "$(cat "$stdout")" channels)"
    bps="$(parse_field "$(cat "$stdout")" bps)"
    rate="$(parse_field "$(cat "$stdout")" rate)"
    samples="$(parse_field "$(cat "$stdout")" samples)"
    [ -z "$channels" ] && channels=$manifest_channels
    [ -z "$bps" ] && bps=$manifest_bps
    [ -z "$rate" ] && rate=$manifest_rate
    [ -z "$samples" ] && samples=$manifest_samples

    compare=not_run
    lib_samples=NA
    ref_samples=NA
    sample_index=NA
    frame=NA
    channel=NA
    byte_offset=NA
    rc=$harness_rc
    status=pass

    if [ "$harness_rc" -ne 0 ]; then
        failure_kind="$(classify_failure "$log" "$harness_rc")"
        status="$failure_kind"
        valid_failures=$((valid_failures + 1))
    else
        set +e
        "$FFMPEG" -v error -nostdin -i "$path" -map 0:a:0 \
            -f s32le -acodec pcm_s32le -y "$ref_pcm" 2> "$WORK/valid-logs/${id}.ffmpeg.stderr"
        ffmpeg_rc=$?
        set -e
        if [ "$ffmpeg_rc" -ne 0 ]; then
            status=reference_decode_failure
            valid_failures=$((valid_failures + 1))
        else
            set +e
            compare_json="$("$PYTHON" "$SOURCE_DIR/compare_pcm.py" \
                --lib "$lib_pcm" --ref "$ref_pcm" --channels "$channels" --bps "$bps")"
            compare_rc=$?
            set -e
            if [ "$compare_rc" -ne 0 ]; then
                status=sample_mismatch
                valid_failures=$((valid_failures + 1))
            fi
            compare="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; d=json.load(sys.stdin); print("match" if d.get("match") else "mismatch")')"
            lib_samples="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin).get("lib_samples","NA"))')"
            ref_samples="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin).get("ref_samples","NA"))')"
            sample_index="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin).get("sample_index","NA"))')"
            frame="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin).get("frame","NA"))')"
            channel="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin).get("channel","NA"))')"
            byte_offset="$(printf '%s\n' "$compare_json" | "$PYTHON" -c 'import json,sys; print(json.load(sys.stdin).get("byte_offset","NA"))')"
        fi
    fi

    if [ "$status" != pass ]; then
        echo "FAIL valid $id status=$status" >&2
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$id" "$path" "$channels" "$bps" "$rate" "$samples" "$status" "$compare" \
        "$lib_samples" "$ref_samples" "$sample_index" "$frame" "$channel" "$byte_offset" "$rc" \
        >> "$WORK/valid_results.tsv"
    valid_index=$((valid_index + 1))
    if [ $((valid_index % 10)) -eq 0 ]; then
        echo "  valid $valid_index/$VALID_COUNT failures=$valid_failures"
    fi
done < "$WORK/valid/manifest.tsv"

set +e
"$PYTHON" "$SOURCE_DIR/summarize.py" --work "$WORK" --report "$REPORT"
summary_rc=$?
set -e

if [ "$malformed_failures" -ne 0 ] || [ "$valid_failures" -ne 0 ]; then
    exit 1
fi
exit "$summary_rc"
