# SPDX-License-Identifier: MIT
"""Review findings ledger and analysis tool.

Maintains docs/plan/review-findings.tsv and provides summary statistics
on code review findings across review sessions and models.
"""

import argparse
import collections
import os
import re
import sys

ALLOWED_SEVERITIES = {"blocker", "should", "nit", "none"}

ALLOWED_CATEGORIES = {
    "correctness",
    "test-gap",
    "model-gap",
    "dead-code",
    "comment-accuracy",
    "contract-claim",
    "status-handling",
    "naming",
    "test-shape",
    "commit-hygiene",
    "docs",
    "style-other",
}

ALLOWED_MECHANIZABLE = {"checked", "could", "no"}

TSV_HEADER = (
    "date\tstep\tround\treviewer\tseverity\tcategory\t"
    "rule\tfile\tsummary\tmechanizable\toutcome\n"
)


def find_main_repo(environ=None):
  """Finds the main repository checkout matching dashboard_lanes.py."""
  if environ is None:
    environ = os.environ
  home = environ.get("HOME", "/")
  return environ.get(
      "DCFS_MAIN_REPO",
      environ.get(
          "DCFS_MAIN_CHECKOUT",
          os.path.join(home, "Sources", "dircachefs"),
      ),
  )


def get_tsv_path(repo_dir):
  """Returns the full path to review-findings.tsv."""
  return os.path.join(repo_dir, "docs", "plan", "review-findings.tsv")


def ensure_tsv_file(tsv_path):
  """Creates the TSV file with a header row if it does not already exist."""
  if not os.path.exists(tsv_path) or os.path.getsize(tsv_path) == 0:
    os.makedirs(os.path.dirname(tsv_path), exist_ok=True)
    with open(tsv_path, "w", encoding="utf-8") as f:
      f.write(TSV_HEADER)


def parse_finding_line(line, line_num):
  """Parses and validates a single finding line separated by ' | '."""
  if "\t" in line:
    raise ValueError(f"Line {line_num}: contains forbidden tab character")

  # Support standard ' | ' separation, falling back to regex pipe split
  # so adjacent empty fields (e.g. 'none | | | | summary | no | open') work.
  parts = [p.strip() for p in line.strip().split(" | ")]
  if len(parts) != 7:
    import re
    alt_parts = [p.strip() for p in re.split(r"\s*\|\s*", line.strip())]
    if len(alt_parts) == 7:
      parts = alt_parts

  if len(parts) != 7:
    raise ValueError(
        f"Line {line_num}: expected 7 fields separated by ' | ', got"
        f" {len(parts)}: {line.strip()}"
    )

  severity, category, rule, file_loc, summary, mechanizable, outcome = parts


  if severity not in ALLOWED_SEVERITIES:
    raise ValueError(
        f"Line {line_num}: invalid severity '{severity}', must be one of"
        f" {sorted(ALLOWED_SEVERITIES)}"
    )

  if severity == "none":
    if category and category != "none" and category not in ALLOWED_CATEGORIES:
      raise ValueError(
          f"Line {line_num}: invalid category '{category}', must be empty,"
          f" 'none', or one of {sorted(ALLOWED_CATEGORIES)}"
      )
    if mechanizable and mechanizable not in ALLOWED_MECHANIZABLE:
      raise ValueError(
          f"Line {line_num}: invalid mechanizable '{mechanizable}', must be"
          f" empty or one of {sorted(ALLOWED_MECHANIZABLE)}"
      )
  else:
    if category not in ALLOWED_CATEGORIES:
      raise ValueError(
          f"Line {line_num}: invalid category '{category}', must be one of"
          f" {sorted(ALLOWED_CATEGORIES)}"
      )
    if mechanizable not in ALLOWED_MECHANIZABLE:
      raise ValueError(
          f"Line {line_num}: invalid mechanizable '{mechanizable}', must be one"
          f" of {sorted(ALLOWED_MECHANIZABLE)}"
      )

  return (severity, category, rule, file_loc, summary, mechanizable, outcome)


def cmd_add(args, stdin=sys.stdin, stderr=sys.stderr):
  """Parses a findings block from stdin and appends to review-findings.tsv."""
  for arg_name, arg_val in [
      ("step", args.step),
      ("round", args.round),
      ("reviewer", args.reviewer),
      ("date", args.date),
  ]:
    if not arg_val:
      stderr.write(f"Error: missing required argument --{arg_name}\n")
      return 1
    if "\t" in str(arg_val):
      stderr.write(f"Error: argument --{arg_name} contains tab character\n")
      return 1

  findings = []
  line_num = 0
  for raw_line in stdin:
    line_num += 1
    line = raw_line.rstrip("\r\n")
    if not line.strip():
      continue
    try:
      finding = parse_finding_line(line, line_num)
      findings.append(finding)
    except ValueError as e:
      stderr.write(f"Error: {e}\n")
      return 1

  if not findings:
    stderr.write("Warning: no findings provided in block\n")
    return 0

  repo = args.repo or find_main_repo()
  tsv_path = get_tsv_path(repo)
  ensure_tsv_file(tsv_path)

  with open(tsv_path, "a", encoding="utf-8") as f:
    for severity, category, rule, file_loc, summary, mech, outcome in findings:
      row = (
          f"{args.date}\t{args.step}\t{args.round}\t{args.reviewer}\t"
          f"{severity}\t{category}\t{rule}\t{file_loc}\t{summary}\t"
          f"{mech}\t{outcome}\n"
      )
      f.write(row)

  return 0


def cmd_summary(args, stdout=sys.stdout, stderr=sys.stderr):
  """Prints summary statistics from review-findings.tsv."""
  repo = args.repo or find_main_repo()
  tsv_path = get_tsv_path(repo)

  if not os.path.exists(tsv_path):
    stderr.write(f"Error: {tsv_path} does not exist\n")
    return 1

  counts_category = collections.Counter()
  counts_rule = collections.Counter()
  counts_severity = collections.Counter()
  counts_mechanizable = collections.Counter()

  with open(tsv_path, "r", encoding="utf-8") as f:
    lines = f.readlines()

  for line in lines[1:]:
    line = line.rstrip("\r\n")
    if not line:
      continue
    fields = line.split("\t")
    if len(fields) < 11:
      continue

    (
        date,
        step,
        round_no,
        reviewer,
        severity,
        category,
        rule,
        file_loc,
        summary,
        mechanizable,
        outcome,
    ) = fields[:11]

    counts_severity[severity] += 1
    if category:
      counts_category[category] += 1
    else:
      counts_category["(none)"] += 1

    rule_key = rule if rule else "(none)"
    counts_rule[rule_key] += 1

    mech_key = mechanizable if mechanizable else "(none)"
    counts_mechanizable[mech_key] += 1

  def print_section(title, counter):
    stdout.write(f"{title}:\n")
    # Sort by count descending, then alphabetically
    for key, count in sorted(counter.items(), key=lambda kv: (-kv[1], kv[0])):
      stdout.write(f"  {key}: {count}\n")
    stdout.write("\n")

  print_section("Category", counts_category)
  print_section("Rule", counts_rule)
  print_section("Severity", counts_severity)
  print_section("Mechanizable", counts_mechanizable)

  return 0


def main(
    argv,
    stdin=sys.stdin,
    stdout=sys.stdout,
    stderr=sys.stderr,
):
  parser = argparse.ArgumentParser(
      description="Review findings ledger and analysis tool"
  )
  subparsers = parser.add_subparsers(dest="subcommand", required=True)

  # Subcommand: add
  parser_add = subparsers.add_parser(
      "add", help="Add findings from block on stdin"
  )
  parser_add.add_argument("--step", required=True, help="Plan step number/name")
  parser_add.add_argument("--round", required=True, help="Review round number")
  parser_add.add_argument(
      "--reviewer", required=True, help="Reviewer (agent type/model/effort)"
  )
  parser_add.add_argument(
      "--date", required=True, help="Review date (YYYY-MM-DD)"
  )
  parser_add.add_argument(
      "--repo", default=None, help="Main repo path override"
  )

  # Subcommand: summary
  parser_summary = subparsers.add_parser(
      "summary", help="Print summary statistics"
  )
  parser_summary.add_argument(
      "--repo", default=None, help="Main repo path override"
  )

  args = parser.parse_args(argv)

  if args.subcommand == "add":
    return cmd_add(args, stdin=stdin, stderr=stderr)
  elif args.subcommand == "summary":
    return cmd_summary(args, stdout=stdout, stderr=stderr)
  return 0


if __name__ == "__main__":
  sys.exit(main(sys.argv[1:]))
