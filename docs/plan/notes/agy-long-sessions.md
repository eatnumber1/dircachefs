# Driving `agy` for long jobs (research, 2026-10-17)

Measured with agy 1.4.2, model gemini-3.8-flash-low, scratch dir
`$scratchpad/agyx` (never a lane). Raw runs are summarised; nothing here
was run against a repository.

## Short answer

Use stream-json in and out, keep stdin open (a FIFO the foreman owns), have
the prompt end the job with a fixed marker line, and close stdin when the
marker appears in an `agent_response`. One 31m40s background command was
seen to finish and wake the agent in that mode, past the 30-minute figure.
No bridge solves this; none is worth adopting.

## 1. Remote-control daemon: not a programmatic API

- `agy remote-control start|status|stop` registers a daemon (systemd user
  unit, or a bare background process without systemd) under the Google
  account; it is "found at https://antigravity.google.com" by instance name
  (`status` output). `--remote-control` / `/remote-control` does the same for
  one session. The changelog describes only browser/companion UIs (1.3.4:
  browser Settings modal, `remote-control serve` sessions; 1.4.2: history in
  the browser). Remote Control refuses ADC sign-in (1.3.3).
- No documented local socket, protocol or CLI to start sessions, send turns
  or get events. It tunnels via Google, not a local API. I did not start it
  (it registers this machine with the account; russ's call).
- Warning: `agy remote-control serve --help` does NOT print help, it starts
  serving (hung until I killed my own PID). Never probe subcommands with
  `--help` blindly.
- Verdict: not usable by a program; skip.

## 2. stream-json: one process, many turns

Input is NDJSON, one object per line. The shape is not in `--help`; found by
trial against the decoder's errors:
`{"event":"user","message":{"content":"<prompt>"}}`
(`"type":"user"` fails: "missing the \"event\" field"; `message` as a string
fails; `message.text` fails: "has no content"; unknown events are ignored
with a warning). `-p` must be written `-p=` here, because `-p` takes the next
token as its prompt ("took --model as its prompt").

Output events (all NDJSON): `init` (conversation_id, tools, cwd),
`step_update` (step_index, state ACTIVE|DONE, step_type user_input |
agent_response | tool | system_message, text_delta, tool_name, usage), and
`result` (status, response, num_turns, usage).

Findings:
- One process serves several turns: two prompts 75 s apart both ran in one
  process (conversation 06ae4a68..., num_turns 2); the second saw the first's
  files.
- `result` is NOT emitted at each turn's end. It was printed only when the
  next input line arrived, or at stdin EOF (single prompt, stdin held 30 s:
  agent answered at 18:29:58, `result` at 18:30:23 = EOF). At EOF the process
  exits. So `result` is "session over", not "turn over".
- There is no turn-end event. The closest signal is the last `agent_response`
  step going DONE with no further step. Do not rely on it: a background
  completion appends more steps afterwards.
- Tool steps are reported only when they finish: a `run_command` step shows
  ACTIVE and DONE at the same instant, `duration_seconds` ~ the command's
  runtime, even when the command was backgrounded.
- Background completion IS an event in the stream: a `system_message` step
  (the task-finished notice) followed by an `agent_response` in which the
  agent reacts. It happens inside the same session, no new input needed.
  Seen at 25 s and at 31m40s (see 3).

## 3. 30-minute wait, resume, configuration

Changelog (older entries): headless runs "wait for background tasks until the
`--print-timeout` deadline, up to a 30-minute cap" (before that they killed
tasks 5 s after idle); `--print-timeout` default is unlimited for turns. The
cap is not a flag, setting or env var I could find (`settings.json` here holds
only model/permissions/trusted workspaces; `agy --help` has nothing; the
binary contains the message `; waiting up to %`, no tunable). Treat as fixed.

Text-mode `-p` with `--print-timeout=15s` and a 45 s background command
(run 18:31:10):
```
root agent idle; waiting up to 15s for 1 background task(s)
STARTED.
terminating 1 background task(s) on exit
```
exit code 0 (a timeout is not an error), and the task was killed: `bg4.txt`
never appeared even 40 s after exit. So the job's end is lost and so is the
job: the wait cap kills the work, it does not merely stop watching.

`--continue` after that: the agent answered "The background command was
canceled/stopped due to a server restart". So `--conversation ID` resumes the
transcript, but a command killed at exit cannot be resumed and its output is
not recovered. Resume only helps for a task that was started with `nohup`/
`setsid` outside agy's task manager (agy does not kill that), and the agent
must then poll a file: no good (rules: no polling).

Long run, stream-json, stdin held open (18:32:42 start): backgrounded
`sleep 1900; echo > file`. The task finished at 19:04:29 (31m40s after start),
the agent got the `system_message`, replied DONE at 19:04:33, and the process
lived until I closed stdin at 19:06:52 (result: status SUCCESS, duration
1907 s, num_turns 1). So with stdin open the 30-minute cap did not end the
session. Caveat: I did not test the same with stdin at EOF or text `-p` for
>30 min; the 30m message may apply only to that path. One data point.

## 4. Bridges

All three spawn `agy -p` as a subprocess, so none can change the 30m
behaviour. Their "long run" support means "do not die on the MCP tool-call
timeout".
- Sh3rm/agy-mcp: MIT, created 2026-08-01, last push 2026-08-15, ARCHIVED,
  "personal tool". `job_id` + `agy_result(wait_seconds)` is the polling
  pattern our rules forbid. Its exec tool prompts every time.
- sshahzaiib/agy-bridge: MIT, 59 stars, pushed 2026-09-19. Per-tool
  `--print-timeout` budgets, kills on quota exhaustion; a tool-call timeout
  design, aimed at delegation not hours-long jobs.
- leologoli/agymcp: MIT, Python, one `agy` tool, session id via `--conversation`.
- arjunthilak05/antigravity-claude-mcp: code-review second opinions only.
- A Stop-hook notification design was not found in any README I could read.
Adopt none: nothing here waits on an event, and none addresses the cap.

## Recommendation (smallest change to run.sh)

Replace `agy -p "<prompt>" ...` by a stream-json session fed from a FIFO:
```
mkfifo "$lane.fifo"; exec 3<>"$lane.fifo"   # held open: no EOF, no sleep
printf '%s\n' "$(jq -nc --arg p "$prompt" '{event:"user",message:{content:$p}}')" >&3
agy --model ... --dangerously-skip-permissions \
    --input-format stream-json --output-format stream-json -p= <&3 |
  tee "$log" | { while read -r l; do
      case $l in *'JOB-COMPLETE'*) exec 3>&-; ;; esac; done; }
```
(sketch, untested as a script: the point is the shape.) The prompt gets one
added sentence: "When all work is finished and every background command has
reported back, print the line JOB-COMPLETE as the last thing you say."
The foreman closes fd 3 on that marker (an event, no timer, no polling); EOF
makes agy emit `result` and exit, giving the final status and token usage.
Tell the agent to start Bazel in the background and wait for the wake-up; the
earlier "run in the foreground" rule is no longer needed.

Add a kill-switch on the marker case: if agy exits first (crash, quota,
`status` ERROR in `result`), the pipeline ends by itself.

## Open questions

1. Does the >30 min survival depend on stdin being open? Test text `-p`
   with a 31-minute background task to learn if the cap is real there.
2. Does the model matter? I used gemini-3.8-flash-low; a model that waits
   with the `wait` tool in a loop (instead of idling) may behave differently.
3. Hard upper bound with stdin open (hours)? Only 32 min was measured.
4. Marker reliability: an agent can print JOB-COMPLETE too early (before
   the Bazel run finished) or never. Needs a prompt rule and a check that the
   last `system_message` precedes it; a failed run is reported by the lane's
   own git state, not by the marker.
5. Whether `--print-timeout` is honoured in stream mode: untested.
6. Remote Control: ask russ whether he wants the machine registered, only
   to find out whether the browser protocol is scriptable (unlikely).
