#!/bin/bash
# Trace validation (formal/README.md, "Trace validation"): runs a QEMU guest
# whose dcfs records its protocol events, collects the traces from the
# serial log, and checks each with TLC against formal/Trace.tla.
#
#   trace_validate.sh --java JAVA --cp CLASSPATH --spec-dir DIR --lock BOOL
#       [--allow-cuts CATS] [--root TRACE --root-cuts CATS]
#       [--expect-reject TRACE ERE] [--reval-cfg CFG] [--life-cfg CFG]
#       [--ident-cfg CFG]
#       -- RUN_QEMU [RUN_QEMU_ARGS...]
#   trace_validate.sh ... --log LOG
#
# The second form checks the traces in LOG (a serial log, or any file of
# trace lines) instead of booting a guest: formal/trace_tests/ uses it to
# test Trace.tla itself on hand-written traces.
#
# DIR holds Trace.tla, Trace.cfg and dcfs.tla, RevalTrace.tla,
# RevalTrace.cfg and reval.tla, LifetimeTrace.tla, LifetimeTrace.cfg and
# lifetime.tla, and IdentTrace.tla, IdentTrace.cfg and ident.tla. --lock
# says whether the run kept the
# kernel's directory lock (the model's KernelDirLock).
#
# Files' traces (formal/reval.tla): the lines "DCFS-REVAL <trace> <file>
# <json>" are one trace per (trace, file) pair too, checked against
# RevalTrace.tla with RevalTrace.cfg, or CFG if --reval-cfg names one (a
# known-bug variant as the model); they are named "reval/<trace>@<file>"
# below, in --expect-reject and in the cuts.
#
# Nodeids' traces (formal/lifetime.tla): the lines "DCFS-LIFE <trace>
# <nodeid> <json>" are one trace per (trace, nodeid) pair, checked against
# LifetimeTrace.tla with LifetimeTrace.cfg, or CFG if --life-cfg names one;
# they are named "life/<trace>@<nodeid>".
#
# Nodeids' identity traces (formal/ident.tla): the lines "DCFS-IDENT
# <trace> <nodeid> <json>", likewise, checked against IdentTrace.tla with
# IdentTrace.cfg, or CFG if --ident-cfg names one; named
# "ident/<trace>@<nodeid>".
#
# A trace may end with a "cut": the recorder stops a directory's trace at a
# step the model does not have, giving "<category>: <detail>". Only the
# categories in CATS (comma-separated) may end a trace in this run; any
# other cut fails the test. --root names a trace (<trace>@<directory inode>)
# that must be valid, have events, and reach the end of the run (its last
# line the run's final event, clean or stop_clear), or end at a cut in its
# own, narrower --root-cuts.
#
# Without --expect-reject every trace must be valid; with it, the trace
# TRACE must be rejected, and the first event no behavior of the model
# explains must match the extended regular expression ERE (a fault build's
# test).
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
reval_cfg=""
life_cfg=""
ident_cfg=""
allow_cuts=""
root=""
root_cuts=""
log=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --java) java="$2"; shift 2 ;;
    --cp) cp="$2"; shift 2 ;;
    --spec-dir) spec_dir="$2"; shift 2 ;;
    --lock) lock="$2"; shift 2 ;;
    --expect-reject) reject_trace="$2"; reject_ere="$3"; shift 3 ;;
    --allow-cuts) allow_cuts="$2"; shift 2 ;;
    --root) root="$2"; shift 2 ;;
    --root-cuts) root_cuts="$2"; shift 2 ;;
    --log) log="$2"; shift 2 ;;
    --reval-cfg) reval_cfg="$2"; shift 2 ;;
    --life-cfg) life_cfg="$2"; shift 2 ;;
    --ident-cfg) ident_cfg="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "trace_validate.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done

work="${TEST_TMPDIR:?}/trace"
rm -rf "$work"
mkdir -p "$work/traces" "$work/reval" "$work/life" "$work/ident" "$work/tlc" \
  "$work/tmp"

# --- the guest run ----------------------------------------------------------
if [[ -n "$log" ]]; then
  serial="$log"
else
  status=0
  "$@" >"$work/qemu.out" 2>&1 || status=$?
  serial="${TEST_UNDECLARED_OUTPUTS_DIR:-$TEST_TMPDIR}/serial.log"
  if [[ "$status" -ne 0 ]]; then
    tail -n 200 "$work/qemu.out"
    echo "trace_validate.sh: FAIL: the guest run failed (exit status $status)"
    exit 1
  fi
  echo "trace_validate.sh: the guest run passed"
fi

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

# The files' traces: from a file's begin line to a cut, every line (no state
# is left out of these).
tr -d '\r' <"$serial" | grep -a '^DCFS-REVAL ' >"$work/reval_lines" || true
echo "trace_validate.sh: $(wc -l <"$work/reval_lines") file trace lines"
awk -v dir="$work/reval" -v ends="$work/traces/ends.tsv" '
  function safe(s) { gsub(/[^A-Za-z0-9._-]/, "_", s); return s }
  {
    key = $2 "@" $3
    json = $0
    sub(/^DCFS-REVAL [^ ]+ [^ ]+ /, "", json)
    if (key in done) next
    if (json ~ /"ev":"begin"/) {
      if (!(key in file)) {
        file[key] = dir "/" safe($2) "@" $3 ".jsonl"
        print json > file[key]
      }
      next
    }
    if (!(key in file)) next
    if (json ~ /"ev":"cut"/) {
      done[key] = 1
      print "reval/" key "\t" json >> ends
      next
    }
    print json > file[key]
  }' "$work/reval_lines"

# The nodeids' traces: from a nodeid's begin line on, every line.
tr -d '\r' <"$serial" | grep -a '^DCFS-LIFE ' >"$work/life_lines" || true
echo "trace_validate.sh: $(wc -l <"$work/life_lines") nodeid trace lines"
awk -v dir="$work/life" '
  function safe(s) { gsub(/[^A-Za-z0-9._-]/, "_", s); return s }
  {
    key = $2 "@" $3
    json = $0
    sub(/^DCFS-LIFE [^ ]+ [^ ]+ /, "", json)
    if (json ~ /"ev":"begin"/) {
      if (!(key in file)) {
        file[key] = dir "/" safe($2) "@" $3 ".jsonl"
        print json > file[key]
      }
      next
    }
    if (!(key in file)) next
    print json > file[key]
  }' "$work/life_lines"

# The nodeids' identity traces: likewise.
tr -d '\r' <"$serial" | grep -a '^DCFS-IDENT ' >"$work/ident_lines" || true
echo "trace_validate.sh: $(wc -l <"$work/ident_lines") identity trace lines"
awk -v dir="$work/ident" '
  function safe(s) { gsub(/[^A-Za-z0-9._-]/, "_", s); return s }
  {
    key = $2 "@" $3
    json = $0
    sub(/^DCFS-IDENT [^ ]+ [^ ]+ /, "", json)
    if (json ~ /"ev":"begin"/) {
      if (!(key in file)) {
        file[key] = dir "/" safe($2) "@" $3 ".jsonl"
        print json > file[key]
      }
      next
    }
    if (!(key in file)) next
    print json > file[key]
  }' "$work/ident_lines"

cp "$spec_dir/Trace.tla" "$spec_dir/Trace.cfg" "$spec_dir/dcfs.tla" \
  "$spec_dir/RevalTrace.tla" "$spec_dir/reval.tla" \
  "$spec_dir/LifetimeTrace.tla" "$spec_dir/lifetime.tla" \
  "$spec_dir/IdentTrace.tla" "$spec_dir/ident.tla" "$work/tlc/"
cp "${reval_cfg:-$spec_dir/RevalTrace.cfg}" "$work/tlc/RevalTrace.cfg"
cp "${life_cfg:-$spec_dir/LifetimeTrace.cfg}" "$work/tlc/LifetimeTrace.cfg"
cp "${ident_cfg:-$spec_dir/IdentTrace.cfg}" "$work/tlc/IdentTrace.cfg"

valid=0
invalid=0
errors=0
empty=0
rejected_as_expected=0
: >"$work/coverage"
# check_trace FILE NAME MODULE: runs TLC with MODULE.cfg on one trace and
# counts the outcome.
check_trace() {
  local f="$1" name="$2" module="$3"
  local events out tlc_status depth bad explained safe_name
  events=$(($(wc -l <"$f") - 1))
  if [[ "$events" -eq 0 ]]; then
    empty=$((empty + 1))
    return
  fi
  safe_name="${name//\//_}"
  out="$work/tlc/$safe_name.out"
  tlc_status=0
  (
    cd "$work/tlc"
    DCFS_TRACE="$f" DCFS_TRACE_LOCK="$lock" "$java" -XX:+UseParallelGC -XX:TieredStopAtLevel=1 -Xmx1g \
      -Xss16m -Djava.io.tmpdir="$work/tmp" -cp "$cp" tlc2.TLC \
      -deadlock -workers 1 -coverage 60 -cleanup \
      -metadir "$work/tlc/states-$safe_name" -config "$module.cfg" "$module"
  ) >"$out" 2>&1 || tlc_status=$?
  depth="$(sed -n 's/^The depth of the complete state graph search is \([0-9]*\)\..*/\1/p' "$out")"
  if [[ "$tlc_status" -ne 0 || -z "$depth" ]]; then
    errors=$((errors + 1))
    grep -v '^\(Loading\|Parsing\|Semantic\)' "$out" | tail -n 40
    echo "trace_validate.sh: ERROR: TLC failed on $name (exit status $tlc_status)"
    return
  fi
  sed -n 's/^<\(T_[A-Za-z0-9]*\) line .*>: \([0-9]*\):[0-9]*$/\1 \2/p' "$out" \
    >>"$work/coverage"
  if [[ "$depth" -eq $((events + 1)) ]]; then
    valid=$((valid + 1))
    echo "trace_validate.sh: valid: $name ($events events)"
    return
  fi
  # No initial state matches the trace's begin line (TLC still reports a
  # depth of 1 then): the begin line is the first thing not explained.
  if grep -q '^Finished computing initial states: 0 distinct states generated' "$out"; then
    depth=0
  fi
  bad="$(sed -n "$((depth + 1))p" "$f")"
  explained="the model explains $((depth - 1)) of $events events; the first it cannot (event $depth)"
  if [[ "$depth" -eq 0 ]]; then
    explained="no initial state of the model matches its begin line"
  fi
  if [[ "$name" == "$reject_trace" ]]; then
    echo "trace_validate.sh: rejected: $name: $explained:"
    echo "  $bad"
    if grep -Eq -- "$reject_ere" <<<"$bad"; then
      rejected_as_expected=1
    fi
    return
  fi
  invalid=$((invalid + 1))
  echo "trace_validate.sh: INVALID: $name: $explained:"
  echo "  $bad"
  if [[ "$depth" -ge 2 ]]; then
    echo "  after: $(sed -n "${depth}p" "$f")"
  fi
}

for f in "$work"/traces/*.jsonl; do
  [[ -e "$f" ]] || continue
  check_trace "$f" "$(basename "$f" .jsonl)" Trace
done
for f in "$work"/reval/*.jsonl; do
  [[ -e "$f" ]] || continue
  check_trace "$f" "reval/$(basename "$f" .jsonl)" RevalTrace
done
for f in "$work"/life/*.jsonl; do
  [[ -e "$f" ]] || continue
  check_trace "$f" "life/$(basename "$f" .jsonl)" LifetimeTrace
done
for f in "$work"/ident/*.jsonl; do
  [[ -e "$f" ]] || continue
  check_trace "$f" "ident/$(basename "$f" .jsonl)" IdentTrace
done

# The cuts: each must be of a category this run allows.
bad_cuts=0
in_list() {  # in_list WORD COMMA-LIST (an empty WORD is in no list)
  [[ -n "$1" && ",$2," == *",$1,"* ]]
}
root_end=""
if [[ -s "$work/traces/ends.tsv" ]]; then
  echo "trace_validate.sh: traces that end at a step the model does not have:"
  while IFS=$'\t' read -r key json; do
    echo "  $key: $json"
    [[ "$key" == "$root" ]] && root_end="$json"
    [[ "$json" == *'"ev":"gone"'* ]] && continue
    category="$(sed -n 's/.*"why":"\([a-z-]*\): .*/\1/p' <<<"$json")"
    if [[ -z "$category" ]] || ! in_list "$category" "$allow_cuts"; then
      echo "trace_validate.sh: CUT NOT ALLOWED: $key ends at a cut of category '$category' (allowed: $allow_cuts)"
      bad_cuts=$((bad_cuts + 1))
    fi
  done <"$work/traces/ends.tsv"
fi
if [[ -n "$root" ]]; then
  root_file="$work/traces/$(sed 's/[^A-Za-z0-9._@-]/_/g' <<<"$root").jsonl"
  root_category="$(sed -n 's/.*"why":"\([a-z-]*\): .*/\1/p' <<<"$root_end")"
  # Uncut, the root must reach the run's final event: its last line is a
  # clean or stop_clear (FinishRun's last step, written for every traced
  # directory at once), and nothing of the run (no line of this trace
  # name, of any directory) follows it but that same step's lines.
  root_trace="${root%@*}"
  root_dir="${root##*@}"
  root_last_nr="$(awk -v t="$root_trace" -v d="$root_dir" \
    '$2 == t && $3 == d { n = NR } END { print n + 0 }' "$work/lines")"
  root_after="$(awk -v t="$root_trace" -v n="$root_last_nr" \
    'NR > n && $2 == t && $0 !~ /"ev":"(clean|stop_clear)"/' "$work/lines" | wc -l)"
  root_last="$(tail -n 1 "$root_file" 2>/dev/null || true)"
  if [[ ! -s "$root_file" ]]; then
    echo "trace_validate.sh: ROOT MISSING: no trace $root"
    bad_cuts=$((bad_cuts + 1))
  elif [[ "$(wc -l <"$root_file")" -lt 2 ]]; then
    echo "trace_validate.sh: ROOT WITHOUT EVENTS: $root has only its begin line"
    bad_cuts=$((bad_cuts + 1))
  elif [[ -n "$root_end" ]] && ! in_list "$root_category" "$root_cuts"; then
    echo "trace_validate.sh: ROOT CUT SHORT: $root ends before the end of the run (allowed: ${root_cuts:-none}): $root_end"
    bad_cuts=$((bad_cuts + 1))
  elif [[ -z "$root_end" ]] &&
    { ! grep -Eq '"ev":"(clean|stop_clear)"' <<<"$root_last" || [[ "$root_after" -ne 0 ]]; }; then
    echo "trace_validate.sh: ROOT CUT SHORT: $root ends before the run's final event ($root_after later lines of the run): $root_last"
    bad_cuts=$((bad_cuts + 1))
  else
    echo "trace_validate.sh: $root reaches ${root_end:+a cut it may end at: }${root_end:-the end of the run}"
  fi
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
if [[ "$invalid" -ne 0 || "$errors" -ne 0 || "$valid" -eq 0 || "$bad_cuts" -ne 0 ]]; then
  echo "trace_validate.sh: FAIL"
  exit 1
fi
echo "trace_validate.sh: PASS"
