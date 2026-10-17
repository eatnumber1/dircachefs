# SPDX-License-Identifier: MIT
# The SibylFS wrapper and the gates of its differential test (step 12.18b),
# sourced by guest/sibylfs.sh and guest/sibylfs_selftest.sh after lib.sh, and
# by test/qemu/sibylfs_lib_test.sh on the host.
#
# SibylFS (third_party/sibylfs/README.md) is a 2015 executable specification
# of POSIX file systems. fs_test_posix runs a script of system calls on a real
# file system and prints the trace of what it saw; fs_test_check says whether
# the model allows that trace. The checker always exits 0 and says its verdict
# in one line, "# trace accepted" or "# trace not accepted", so
# sibylfs_check turns that line into an exit status.
#
# A verdicts file has one "<script> <dcfs> <backing>" line per script, each
# verdict a sibylfs_check status: 0 accepted, 1 rejected, 2 no verdict.

# sibylfs_check TRACE OUT: checks TRACE against the model (Linux flags, process
# 1 a superuser, as fs_test_posix runs it) and keeps what the checker said in
# OUT. Returns 0 if the model allows the trace, 1 if it does not, and 2 if the
# checker printed neither verdict (a trace it could not parse, a crash).
sibylfs_check() {
  "${SIBYLFS_CHECK:-/sibylfs/bin/fs_test_check}" -arch linux --root "$1" \
    >"$2" 2>&1
  if grep -qx '# trace accepted' "$2"; then
    return 0
  fi
  if grep -qx '# trace not accepted' "$2"; then
    return 1
  fi
  return 2
}

# sibylfs_run SCRIPT ROOT TRACE: runs the SibylFS script SCRIPT on the file
# system ROOT is on, in the new directory ROOT (fs_test_posix refuses one that
# exists), writing the trace it saw to TRACE and what it said on stderr to
# $SIBYLFS_LOG. Returns its exit status.
sibylfs_run() {
  "${SIBYLFS_POSIX:-/sibylfs/bin/fs_test_posix}" -arch linux -r "$2" \
    -tmp "$SIBYLFS_TMP" "$1" >"$3" 2>>"$SIBYLFS_LOG"
}

# sibylfs_install_account_commands: puts useradd, userdel, groupadd and
# groupdel in /bin, and the users and groups files (as pjdfstest.sh does)
# where they are missing. fs_test_posix makes the users and groups that a
# script's User_id and Group_id name by running useradd -s /bin/false NAME and
# groupadd NAME, and undoes them with userdel and groupdel when the script
# ends; it reads the numbers back from the files. The guest has busybox's
# adduser and its relatives, not shadow-utils. A user gets a group of its own
# name, as useradd gives it, because deluser needs one.
sibylfs_install_account_commands() {
  mkdir -p /etc /bin
  [ -s /etc/passwd ] || printf '%s\n' 'root:x:0:0:root:/:/bin/sh' \
    'nobody:x:65534:65534:nobody:/:/bin/false' >/etc/passwd
  [ -s /etc/group ] || printf '%s\n' 'root:x:0:' 'nobody:x:65534:' \
    >/etc/group
  sibylfs_shim useradd \
    'addgroup "$3" && exec adduser -D -H -G "$3" -s "$2" "$3"'
  sibylfs_shim userdel 'exec deluser "$1"'
  sibylfs_shim groupadd 'exec addgroup "$1"'
  sibylfs_shim groupdel 'exec delgroup "$1"'
}

# sibylfs_shim NAME BODY: the script /bin/NAME, whose body is BODY.
sibylfs_shim() {
  printf '#!/bin/sh\n%s\n' "$2" >"/bin/$1"
  chmod +x "/bin/$1"
}

# sibylfs_forget_accounts: removes the users and groups fs_test_posix made for
# the scripts' User_id and Group_id (fs_user_N, fs_group_N) and did not
# remove: a script that dies halfway (adhoc_destroy_process does) skips its
# userdel, and the next script's useradd then fails.
sibylfs_forget_accounts() {
  for sf_user in $(sed -n 's/^\(fs_user_[0-9]*\):.*/\1/p' /etc/passwd); do
    deluser "$sf_user"
  done
  for sf_group in $(sed -n 's/^\(fs_group_[0-9]*\):.*/\1/p' /etc/group); do
    delgroup "$sf_group"
  done
}

# sibylfs_spec_valid SPEC: whether SPEC is a script selection
# sibylfs_scripts understands: a comma-separated list, none of it empty, of
# "adhoc" and "K/N" with K and N decimal numbers and K below N (so N is not
# 0). Without this a typo or a shard that does not exist selects nothing, and
# a run of the remaining part (or of nothing, once the gates are satisfied)
# passes.
sibylfs_spec_valid() {
  awk -v spec="$1" '
    BEGIN {
      n = split(spec, token, ",")
      for (i = 1; i <= n; i++) {
        if (token[i] == "adhoc") {
          continue
        }
        if (token[i] !~ /^[0-9]+\/[0-9]+$/) {
          exit 1
        }
        split(token[i], kn, "/")
        if (kn[1] + 0 >= kn[2] + 0) {
          exit 1
        }
      }
      exit (spec == "")
    }'
}

# sibylfs_scripts SPEC: reads the member names of the suite's tarball on stdin
# and prints the names of the scripts SPEC (sibylfs_spec_valid) selects, one
# per line, in a fixed order. "adhoc" selects the hand-written scripts
# (adhoc_*) and "K/N" every Nth of the generated scripts, starting at the Kth:
# shard K of N. A script the checker alone reads (-check-int.trace), and the
# .tmpl directories' copies of the hand-written ones, are not selected.
sibylfs_scripts() {
  grep -e '-int\.trace$' | grep -v -e '-check-int\.trace$' -e '\.tmpl/' |
    sort |
    awk -v specs="$1" '
      BEGIN { n_specs = split(specs, spec, ",") }
      {
        base = $0
        sub(".*/", "", base)
        if (base ~ /^adhoc_/) {
          adhoc = 1
        } else {
          adhoc = 0
          generated++
        }
        for (i = 1; i <= n_specs; i++) {
          if (spec[i] == "adhoc" && adhoc) {
            print
            break
          }
          split(spec[i], kn, "/")
          if (!adhoc && kn[2] != "" && (generated - 1) % kn[2] == kn[1]) {
            print
            break
          }
        }
      }'
}

# sibylfs_regressions VERDICTS: the scripts the model allows on the backing
# file system and rejects (or cannot judge) through dcfs, one per line: what
# dcfs does differently from the file system under it. A script rejected on
# both is the 2015 model against today's kernel, not dcfs.
sibylfs_regressions() {
  awk '$2 != 0 && $3 == 0 { print $1 }' "$1"
}

# sibylfs_in LIST FILE and sibylfs_not_in LIST FILE: the lines of FILE that are
# lines of LIST, and those that are not, in FILE's order (LIST may be empty:
# the usual awk and grep idioms for this print nothing then).
sibylfs_in() {
  sibylfs_select "$1" "$2" 1
}
sibylfs_not_in() {
  sibylfs_select "$1" "$2" 0
}
sibylfs_select() {
  awk -v list="$1" -v want="$3" '
    BEGIN { while ((getline line < list) > 0) listed[line] = 1 }
    ($0 in listed) == want' "$2"
}

# sibylfs_suite_sane FSTYPE VERDICTS: the sibylfs-suite-sane check. A run in
# which the checker rejects or cannot judge most traces of the backing file
# system proves nothing, as in pjdfstest_suite_sane: every dcfs rejection is
# then a backing rejection too. Fails if no script ran, if any trace got no
# verdict (a fs_test_posix that died before it wrote a trace leaves an empty
# file, which the checker cannot parse), or if the model allows fewer than half
# of the backing file system's traces (it allows 82% of ext4's: 901 of 1,102
# scripts, run once with dcfs_sibylfs=adhoc,0/20 on 2026-10-17; the rest are
# the 2015 model against today's kernel).
sibylfs_suite_sane() {
  ss_total=$(wc -l <"$2")
  ss_unjudged=$(awk '$2 == 2 || $3 == 2' "$2" | wc -l)
  ss_accepted=$(awk '$3 == 0' "$2" | wc -l)
  if [ "$ss_total" -gt 0 ] && [ "$ss_unjudged" -eq 0 ] &&
    [ $((ss_accepted * 2)) -ge "$ss_total" ]; then
    pass sibylfs-suite-sane
  else
    fail sibylfs-suite-sane "the model allowed $ss_accepted of $ss_total \
traces of $1 directly ($ss_unjudged without a verdict): the tooling in the \
guest is broken, so no dcfs rejection can be told apart from it (see \
\$SIBYLFS_LOG)"
    return 1
  fi
}
