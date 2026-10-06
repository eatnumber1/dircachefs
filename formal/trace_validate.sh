#!/bin/bash
# Trace validation (formal/README.md, "Trace validation"): runs a QEMU guest
# whose dcfs records its protocol events, collects the traces from the
# serial log, and checks each with TLC against formal/Trace.tla.
#
#   trace_validate.sh --java JAVA --cp CLASSPATH --spec-dir DIR --lock BOOL
#       [--expect-reject TRACE ERE] -- RUN_QEMU [RUN_QEMU_ARGS...]
#
# DIR holds Trace.tla, Trace.cfg and dcfs.tla. --lock says whether the run
# kept the kernel's directory lock (the model's KernelDirLock). Without
# --expect-reject every trace must be valid (or end in a "cut" the recorder
# made at a step the model does not have, which is reported); with it, the
# trace TRACE (<trace name>@<directory inode>) must be rejected, and the
# first event no behavior of the model explains must match the extended
# regular expression ERE (the fault-injection test).
#
# Each line the recorder writes is "DCFS-TRACE <trace> <dir> <json>"; the
# lines of one (trace, dir) pair, from its first "begin" line to a "cut" or
# "gone" line, are one trace file; a line without a "db" (the recorder
# leaves out a state equal to the previous line's) gets the previous one's. TLC reports the depth of its search,
# which is one more than the number of events it could match: all of them
# if the trace is valid, else the index of the first one it could not.
set -euo pipefail

java=""
cp=""
spec_dir=""
lock=""
reject_trace=""
reject_ere=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --java) java="$2"; shift 2 ;;
    --cp) cp="$2"; shift 2 ;;
    --spec-dir) spec_dir="$2"; shift 2 ;;
    --lock) lock="$2"; shift 2 ;;
    --expect-reject) reject_trace="$2"; reject_ere="$3"; shift 3 ;;
    --) shift; break ;;
    *) echo "trace_validate.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done

work="${TEST_TMPDIR:?}/trace"
rm -rf "$work"
mkdir -p "$work/traces" "$work/tlc" "$work/tmp"

# --- the guest run ----------------------------------------------------------
status=0
"$@" >"$work/qemu.out" 2>&1 || status=$?
serial="${TEST_UNDECLARED_OUTPUTS_DIR:-$TEST_TMPDIR}/serial.log"
if [[ "$status" -ne 0 ]]; then
  tail -n 200 "$work/qemu.out"
  echo "trace_validate.sh: FAIL: the guest run failed (exit status $status)"
  exit 1
fi
echo "trace_validate.sh: the guest run passed"

# --- the traces ---------------------------------------------------------------
tr -d '\r' <"$serial" | grep -a '^DCFS-TRACE ' >"$work/lines" || true
echo "trace_validate.sh: $(wc -l <"$work/lines") trace lines"
awk -v dir="$work/traces" '
  function safe(s) { gsub(/[^A-Za-z0-9._-]/, "_", s); return s }
  {
    key = $2 "@" $3
    json = $0
    sub(/^DCFS-TRACE [^ ]+ [^ ]+ /, "", json)
    if (key in done) next
    if (json ~ /"ev":"begin"/) {
      if (!(key in file)) {
        file[key] = dir "/" safe($2) "@" $3 ".jsonl"
        print json > file[key]
        db[key] = json
        sub(/^.*,"db":/, "", db[key])
        sub(/\}$/, "", db[key])
      }
      next
    }
    if (!(key in file)) next
    if (json ~ /"ev":"(cut|gone)"/) {
      done[key] = 1
      print key "\t" json >> (dir "/ends.tsv")
      next
    }
    # The recorder leaves out a state that equals the previous line'"'"'s.
    if (json ~ /,"db":\{/) {
      db[key] = json
      sub(/^.*,"db":/, "", db[key])
      sub(/\}$/, "", db[key])
    } else {
      # (Concatenation, not sub(): "&" in a name would mean the match.)
      json = substr(json, 1, length(json) - 1) ",\"db\":" db[key] "}"
    }
    print json > file[key]
  }' "$work/lines"

cp "$spec_dir/Trace.tla" "$spec_dir/Trace.cfg" "$spec_dir/dcfs.tla" "$work/tlc/"

valid=0
invalid=0
errors=0
empty=0
rejected_as_expected=0
: >"$work/coverage"
for f in "$work"/traces/*.jsonl; do
  [[ -e "$f" ]] || continue
  name="$(basename "$f" .jsonl)"
  events=$(($(wc -l <"$f") - 1))
  if [[ "$events" -eq 0 ]]; then
    empty=$((empty + 1))
    continue
  fi
  out="$work/tlc/$name.out"
  tlc_status=0
  (
    cd "$work/tlc"
    DCFS_TRACE="$f" DCFS_TRACE_LOCK="$lock" "$java" -XX:+UseParallelGC -XX:TieredStopAtLevel=1 -Xmx1g \
      -Xss16m -Djava.io.tmpdir="$work/tmp" -cp "$cp" tlc2.TLC \
      -deadlock -workers 1 -coverage 60 -cleanup \
      -metadir "$work/tlc/states-$name" -config Trace.cfg Trace
  ) >"$out" 2>&1 || tlc_status=$?
  depth="$(sed -n 's/^The depth of the complete state graph search is \([0-9]*\)\..*/\1/p' "$out")"
  if [[ "$tlc_status" -ne 0 || -z "$depth" ]]; then
    errors=$((errors + 1))
    grep -v '^\(Loading\|Parsing\|Semantic\)' "$out" | tail -n 40
    echo "trace_validate.sh: ERROR: TLC failed on $name (exit status $tlc_status)"
    continue
  fi
  sed -n 's/^<\(T_[A-Za-z0-9]*\) line .*>: \([0-9]*\):[0-9]*$/\1 \2/p' "$out" \
    >>"$work/coverage"
  if [[ "$depth" -eq $((events + 1)) ]]; then
    valid=$((valid + 1))
    echo "trace_validate.sh: valid: $name ($events events)"
    continue
  fi
  bad="$(sed -n "$((depth + 1))p" "$f")"
  if [[ "$name" == "$reject_trace" ]]; then
    echo "trace_validate.sh: rejected: $name: the model explains $((depth - 1)) of $events events; the first it cannot (event $depth):"
    echo "  $bad"
    if grep -Eq -- "$reject_ere" <<<"$bad"; then
      rejected_as_expected=1
    fi
    continue
  fi
  invalid=$((invalid + 1))
  echo "trace_validate.sh: INVALID: $name: the model explains $((depth - 1)) of $events events; the first it cannot (event $depth):"
  echo "  $bad"
  if [[ "$depth" -ge 2 ]]; then
    echo "  after: $(sed -n "${depth}p" "$f")"
  fi
done

if [[ -s "$work/traces/ends.tsv" ]]; then
  echo "trace_validate.sh: traces that end at a step the model does not have:"
  awk -F '\t' '{ print "  " $1 ": " $2 }' "$work/traces/ends.tsv"
fi
echo "trace_validate.sh: action coverage (states that matched an event, over the valid traces):"
awk '{ n[$1] += $2 } END { for (a in n) printf "  %-24s %d\n", a, n[a] }' \
  "$work/coverage" | sort
echo "trace_validate.sh: $valid valid, $invalid invalid, $errors TLC errors, $empty without events"

if [[ -n "$reject_trace" ]]; then
  if [[ "$rejected_as_expected" -eq 1 && "$errors" -eq 0 ]]; then
    echo "trace_validate.sh: PASS: $reject_trace was rejected at the expected event"
    exit 0
  fi
  echo "trace_validate.sh: FAIL: expected $reject_trace to be rejected at an event matching: $reject_ere"
  exit 1
fi
if [[ "$invalid" -ne 0 || "$errors" -ne 0 || "$valid" -eq 0 ]]; then
  echo "trace_validate.sh: FAIL"
  exit 1
fi
echo "trace_validate.sh: PASS"
