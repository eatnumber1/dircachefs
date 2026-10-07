#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#include "dcfs/testonly/trace_recorder.h"

#include <fcntl.h>     // O_ACCMODE, O_APPEND
#include <linux/fs.h>  // FS_IOC_*, FS_*_FL
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dcfs/context.h"
#include "dcfs/escape.h"
#include "dcfs/metadata_cache.h"
#include "dcfs/migrate.h"
#include "dcfs/protocol_events.h"
#include "dcfs/sqlite.h"
#include "dcfs/status.h"
#include "dcfs/syscalls_backing.h"
#include "fuse_lowlevel.h"  // FUSE_SET_ATTR_*
#include "sqlite3.h"

namespace dcfs::testonly {
namespace {

using events::Ino;

// `s` as a JSON string literal.
std::string JsonStr(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (u < 0x20 || u == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", u);
      out.append(buf);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

// A name as the traces spell it: EscapeBytes (names are bytes), as a JSON
// string.
std::string Name(std::string_view name) { return JsonStr(EscapeBytes(name)); }

const char *Bool(bool b) { return b ? "true" : "false"; }

// The identity of a backing object, as both a probe (statx) and a cached
// row (its backing_ino and birth time) spell it.
std::string Key(uint64_t ino, int64_t btime_sec, int64_t btime_nsec) {
  return absl::StrCat("k:", ino, ".", btime_sec, ".", btime_nsec);
}

std::string ProbeValue(const events::Probe &probe) {
  switch (probe.kind) {
    case events::Probe::Kind::kPresent:
      return JsonStr(Key(probe.ino, probe.btime_sec, probe.btime_nsec));
    case events::Probe::Kind::kAbsent:
      return JsonStr("absent");
    case events::Probe::Kind::kRefused:
      return JsonStr("refused");
  }
  return JsonStr("?");
}

int ErrnoOf(const absl::Status &status) {
  if (status.ok()) return 0;
  return GetErrnoFromStatus(status).value_or(-1);
}

// A create, or a link of an unnamed O_TMPFILE file, which to its new
// parent is a create without a probe (the model's "linkcreate").
bool IsCreate(const std::string &kind) {
  return kind == "create" || kind == "linkcreate";
}

bool IsMutation(const std::string &kind) {
  return IsCreate(kind) || kind == "unlink" || kind == "rename";
}

// Distinct ids, in order of first appearance.
std::vector<Ino> Distinct(events::IdsFn ids) {
  std::vector<Ino> out;
  ids([&](Ino id) {
    for (Ino seen : out) {
      if (seen == id) return;
    }
    out.push_back(id);
  });
  return out;
}

}  // namespace

TraceRecorder::TraceRecorder(int fd, std::string trace, bool files,
                             bool lifetimes, bool identities, bool directories)
    : fd_(fd),
      trace_(std::move(trace)),
      files_enabled_(files),
      lifetimes_enabled_(lifetimes),
      identities_enabled_(identities),
      directories_enabled_(directories) {}

void TraceRecorder::Enter(const char *cause) {
  cause_ = cause;
  ++callbacks_;
}

void TraceRecorder::BeginAll(Context &ctx) {
  Enter("BeginAll");
  After(ctx);
}

// --- Helpers ---------------------------------------------------------------

TraceRecorder::Mapping TraceRecorder::Map(const Frame &r, Ino dir) {
  using events::Op;
  Mapping m;
  auto request = [&](std::string kind, std::string n = "",
                     std::string mm = "") {
    m.kind = Mapping::Kind::kRequest;
    m.req_kind = std::move(kind);
    m.n = std::move(n);
    m.m = std::move(mm);
  };
  auto unmodelled = [&](std::string why) {
    m.kind = Mapping::Kind::kUnmodelled;
    m.why = std::move(why);
  };
  switch (r.op) {
    case Op::kLookup:
      if (r.ino != dir || r.name == "..") break;
      if (r.name == ".") {
        request("getattr");
      } else {
        request("lookup", EscapeBytes(r.name));
      }
      break;
    case Op::kGetattr:
    case Op::kOpendir:
      if (r.ino == dir) request("getattr");
      break;
    case Op::kReaddir:
      if (r.ino == dir) request("readdir");
      break;
    case Op::kReaddirplus:
      // A continuation (offset >= 1) has no "." entry, so it never
      // refreshes the directory's attributes: to the model, a readdir.
      if (r.ino == dir) request(r.offset < 1 ? "readdirplus" : "readdir");
      break;
    case Op::kMknod:
    case Op::kMkdir:
    case Op::kSymlink:
    case Op::kCreate:
      if (r.ino == dir) request("create", EscapeBytes(r.name));
      break;
    case Op::kUnlink:
    case Op::kRmdir:
      if (r.ino == dir) request("unlink", EscapeBytes(r.name));
      break;
    case Op::kRename:
      if (r.ino != dir && r.newparent != dir) break;
      if (r.ino != r.newparent) {
        unmodelled("cross-directory-rename: a rename across directories");
      } else if (r.flags != 0) {
        unmodelled("rename-flags: a rename with flags");
      } else if (r.name == r.newname) {
        unmodelled("rename-flags: a rename of a name onto itself");
      } else {
        request("rename", EscapeBytes(r.name), EscapeBytes(r.newname));
      }
      break;
    case Op::kLink:
      if (r.newparent == dir) unmodelled("link: a link into the directory");
      break;
    case Op::kLinkTmpfile:
      if (r.newparent == dir) request("linkcreate", EscapeBytes(r.newname));
      break;
    case Op::kIoctl:
      // Only a set changes D (its flags and ctime); a read (lsattr's
      // FS_IOC_GETFLAGS) is nothing to D, and its getattr a getattr.
      if (r.ino == dir &&
          (r.flags == FS_IOC_SETFLAGS || r.flags == FS_IOC_FSSETXATTR)) {
        unmodelled("dir-attrs: an ioctl changing the directory's flags");
      }
      break;
    case Op::kSetattr:
      if (r.ino == dir) unmodelled("dir-attrs: a setattr of the directory");
      break;
    case Op::kSetxattr:
    case Op::kRemovexattr:
      if (r.ino == dir) unmodelled("dir-attrs: an xattr change of the directory");
      break;
    default:
      break;
  }
  return m;
}

TraceRecorder::Frame *TraceRecorder::InnermostRequest() {
  for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
    if (it->kind == Frame::Kind::kRequest) return &*it;
  }
  return nullptr;
}

TraceRecorder::Req *TraceRecorder::Find(Ino dir, Frame **owner) {
  for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
    auto found = it->reqs.find(dir);
    if (found != it->reqs.end()) {
      if (owner != nullptr) *owner = &*it;
      return &found->second;
    }
    if (it->kind == Frame::Kind::kRequest) break;
  }
  return nullptr;
}

TraceRecorder::Req *TraceRecorder::RequestReq(Ino dir) {
  Frame *rf = InnermostRequest();
  if (rf == nullptr) return nullptr;
  auto it = rf->reqs.find(dir);
  return it == rf->reqs.end() ? nullptr : &it->second;
}

TraceRecorder::Req &TraceRecorder::Open(Frame &frame, Ino dir,
                                        std::string kind, std::string n,
                                        std::string m) {
  std::set<int> &slots = dirs_[dir].slots;
  int slot = 1;
  while (slots.contains(slot)) ++slot;
  slots.insert(slot);
  Req &req = frame.reqs[dir];
  req = Req{.slot = slot,
            .kind = std::move(kind),
            .n = std::move(n),
            .m = std::move(m)};
  return req;
}

void TraceRecorder::Defer(Ino dir, Req &req, std::string why) {
  // The trace ends here, as a cut if the request fails, or "unexplained"
  // if it replies OK (Close decides); nothing more is written for it.
  if (!req.pending.empty()) return;
  EndHeld(dir);
  req.pending = std::move(why);
  dirs_[dir].pending = true;
}

void TraceRecorder::Close(
    Context &ctx, Frame &frame, const absl::Status &status,
    absl::FunctionRef<std::string(const Req &)> unmodelled_end) {
  for (auto &[dir, req] : frame.reqs) {
    if (Dir &state = dirs_[dir]; state.reading && state.read_slot == req.slot) {
      // Its population never reported its reads: a failure ends the trace
      // where the population began; a frame that returned OK anyway is
      // unexplained.
      if (status.ok()) {
        Unexplained(ctx, dir,
                    "a listing's reads ended without their line, and its "
                    "request replied OK");
      } else {
        Cut(ctx, dir, absl::StrCat("failed: a listing failed half-way: ",
                                   status.ToString()));
      }
    }
    if (!req.pending.empty()) {
      dirs_[dir].pending = false;
      if (status.ok()) {
        Unexplained(ctx, dir,
                    absl::StrCat(req.pending, ", and its request replied OK"));
      } else {
        Cut(ctx, dir, absl::StrCat("failed: ", req.pending, ": ",
                                   status.ToString()));
      }
    } else if (Traced(dir) && req.arrived) {
      // An interrupted request's EINTR is the model's (its interrupt line
      // made the request idle); anything else is the frame's to judge.
      const std::string why = req.interrupted && ErrnoOf(status) == EINTR
                                  ? ""
                                  : unmodelled_end(req);
      if (why.empty()) {
        Emit(ctx, dir, &req, "reply");
      } else if (why.starts_with("unexplained: ")) {
        Unexplained(ctx, dir, why.substr(13));
      } else {
        Cut(ctx, dir, why);
      }
    }
    dirs_[dir].slots.erase(req.slot);
  }
  frame.reqs.clear();
}

bool TraceRecorder::Traced(Ino dir) const {
  auto it = dirs_.find(dir);
  return it != dirs_.end() && !it->second.dead;
}

std::vector<Ino> TraceRecorder::AllDirs(Context &ctx) {
  std::vector<Ino> dirs;
  absl::StatusOr<sqlite3::Statement *> stmt =
      ctx.db.Prepared("SELECT inode FROM directories ORDER BY inode");
  CHECK_OK(stmt.status());
  CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &row) {
    dirs.push_back(row.Column<int64_t>(0));
    return absl::OkStatus();
  }));
  return dirs;
}

std::string TraceRecorder::State::Json() const {
  std::string out = "{\"dent\":[";
  bool first = true;
  for (const auto &[name, value] : dent) {
    absl::StrAppend(&out, first ? "" : ",", "[", Name(name), ",",
                    JsonStr(value), "]");
    first = false;
  }
  absl::StrAppend(&out, "]", rest, "}");
  return out;
}

std::string TraceRecorder::Snapshot(Context &ctx, Ino dir) {
  return SnapshotState(ctx, dir).Json();
}

TraceRecorder::State TraceRecorder::SnapshotState(Context &ctx, Ino dir) {
  State out;
  {
    absl::StatusOr<sqlite3::Statement *> stmt = ctx.db.Prepared(
        "SELECT d.name, d.state, i.backing_ino, i.btime_s, i.btime_ns "
        "FROM dentries d LEFT JOIN inodes i ON i.id = d.inode "
        "WHERE d.parent = ? ORDER BY d.name");
    CHECK_OK(stmt.status());
    CHECK_OK((*stmt)->Bind(1, dir));
    CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &row) {
      std::string name = row.Column<std::string>(0);
      const std::string state = row.Column<std::string>(1);
      out.dent.emplace_back(
          std::move(name),
          state == "present" ? Key(row.Column<uint64_t>(2),
                                   row.Column<int64_t>(3),
                                   row.Column<int64_t>(4))
                             : state);
      return absl::OkStatus();
    }));
  }
  bool complete = false;
  int64_t epoch = 0;
  {
    absl::StatusOr<sqlite3::Statement *> stmt = ctx.db.Prepared(
        "SELECT children_complete, epoch FROM directories WHERE inode = ?");
    CHECK_OK(stmt.status());
    CHECK_OK((*stmt)->Bind(1, dir));
    CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &row) {
      complete = row.Column<bool>(0);
      epoch = row.Column<int64_t>(1);
      return absl::OkStatus();
    }));
  }
  bool valid = false;
  {
    absl::StatusOr<sqlite3::Statement *> stmt =
        ctx.db.Prepared("SELECT attrs_valid FROM inodes WHERE id = ?");
    CHECK_OK(stmt.status());
    CHECK_OK((*stmt)->Bind(1, dir));
    CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &row) {
      valid = row.Column<bool>(0);
      return absl::OkStatus();
    }));
  }
  bool dirty = false;
  {
    absl::StatusOr<sqlite3::Statement *> stmt =
        ctx.db.Prepared("SELECT 1 FROM dirty WHERE inode = ?");
    CHECK_OK(stmt.status());
    CHECK_OK((*stmt)->Bind(1, dir));
    CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &) {
      dirty = true;
      return absl::OkStatus();
    }));
  }
  absl::StatusOr<bool> clean = GetCleanShutdown(ctx.db);
  CHECK_OK(clean.status());
  auto inflight = ctx.fills.inflight.find(dir);
  out.rest = absl::StrCat(
      ",\"complete\":", Bool(complete), ",\"epoch\":", epoch,
      ",\"valid\":", Bool(valid), ",\"dirty\":", Bool(dirty),
      ",\"clean\":", Bool(*clean),
      ",\"durable\":", Bool(ctx.dirty.durable.contains(dir)),
      ",\"inflight\":",
      inflight == ctx.fills.inflight.end() ? 0 : inflight->second);
  return out;
}

void TraceRecorder::Write(Ino dir, const std::string &json) {
  if (!directories_enabled_) return;
  if (auto it = dirs_.find(dir); it != dirs_.end()) {
    if (it->second.pending) return;  // Its end is decided later (Defer).
    if (it->second.reading) {
      it->second.held.push_back(json);
      return;
    }
  }
  WriteLine(absl::StrCat("DCFS-TRACE ", trace_, " ", dir, " ", json, "\n"));
}

void TraceRecorder::WriteLine(const std::string &line) {
  size_t done = 0;
  while (done < line.size()) {
    absl::StatusOr<size_t> n =
        syscalls::write(fd_, line.data() + done, line.size() - done);
    if (!n.ok() && StatusToErrno(n.status()) == EINTR) continue;
    CHECK_OK(n) << "writing a trace line";
    CHECK_GT(*n, 0u) << "writing a trace line";
    done += *n;
  }
}

void TraceRecorder::Emit(Context &ctx, Ino dir, Req *req, std::string_view ev,
                         std::string_view fields) {
  if (!Traced(dir)) return;
  std::string json =
      absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                   ",\"ev\":", JsonStr(ev));
  if (req != nullptr) {
    absl::StrAppend(&json, ",\"p\":\"p", req->slot, "\"");
    if (!req->arrived) {
      // The request's first line: its Arrive, if its slot is idle.
      absl::StrAppend(&json, ",\"req\":{\"k\":", JsonStr(req->kind),
                      ",\"n\":", JsonStr(req->n), ",\"m\":", JsonStr(req->m),
                      "}");
      req->arrived = true;
    }
  }
  State now = SnapshotState(ctx, dir);
  const std::string db = now.Json();
  Dir &state = dirs_[dir];
  // An unchanged state is left out (the serial console is slow, and slows
  // the guest): formal/trace_validate.sh puts the previous line's back.
  // Not while a population's lines are held, which are reordered.
  if (db == state.last && !state.reading) {
    absl::StrAppend(&json, fields, "}");
  } else {
    absl::StrAppend(&json, fields, ",\"db\":", db, "}");
  }
  Write(dir, json);
  state.last = db;
  state.last_state = std::move(now);
  covered_.insert(dir);
}

std::string TraceRecorder::FrameEnd(std::string_view what,
                                    const absl::Status &status) {
  // A frame that failed ends the trace (its request did not finish, which
  // the model's requests always do). One that succeeded replies, and the
  // model checks that its request had (T_Reply): one that skipped a step
  // it must take is rejected there.
  if (status.ok()) return "";
  return absl::StrCat("failed: ", what, " failed: ", status.ToString());
}

void TraceRecorder::EndHeld(Ino dir) {
  // A trace that ends while a listing's lines are held ends where the
  // listing began: the held lines (after that point) are dropped, and the
  // end line is written at once (the listing's own line would have come
  // first, and never will).
  Dir &state = dirs_[dir];
  state.reading = false;
  state.held.clear();
}

void TraceRecorder::Unexplained(Context &ctx, Ino dir, std::string_view why) {
  if (!Traced(dir)) return;
  EndHeld(dir);
  Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                          ",\"ev\":\"unexplained\",\"why\":", JsonStr(why),
                          ",\"db\":", Snapshot(ctx, dir), "}"));
  dirs_[dir].dead = true;
  covered_.insert(dir);
}

void TraceRecorder::Cut(Context &ctx, Ino dir, std::string_view why) {
  if (!Traced(dir)) return;
  EndHeld(dir);
  Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                          ",\"ev\":\"cut\",\"why\":", JsonStr(why), "}"));
  dirs_[dir].dead = true;
  covered_.insert(dir);
}

std::string TraceRecorder::Origin() {
  // What made a directory row appear, from the callback that saw it first:
  // the trace's start, a mkdir's phase 3 (RecordNewChild, whose End
  // follows at once), a listing's or resolve's commit (RecordChild),
  // ParentOf. Trace.tla checks the row's first state against it.
  const std::string_view cause = cause_;
  if (cause == "BeginAll" || cause == "RunStarted") return "existing";
  if (cause == "MutationEnded") {
    const Frame *rf = InnermostRequest();
    if (rf != nullptr && rf->op == events::Op::kMkdir) return "mkdir";
  }
  if (cause == "PopulateCommitted" || cause == "ResolveCommitted") {
    return "listing";
  }
  if (cause == "ParentRecorded") return "parent";
  return "other";
}

void TraceRecorder::After(Context &ctx) {
  // Nothing is looked at inside a transaction (an invalidation inside an
  // upsert, say): the state there is half-written. The next callback
  // outside one checks what the transaction did.
  if (ctx.db.InTransaction()) return;
  const std::vector<Ino> all = AllDirs(ctx);
  const std::set<Ino> present(all.begin(), all.end());
  for (auto &[dir, state] : dirs_) {
    if (!state.dead && !present.contains(dir)) {
      EndHeld(dir);
      Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                              ",\"ev\":\"gone\"}"));
      state.dead = true;
    }
  }
  for (Ino dir : all) {
    if (dirs_.contains(dir)) continue;
    // A new directory row (or the first look at an existing one): its
    // trace begins in the state it is in now.
    dirs_[dir] = Dir{};
    State now = SnapshotState(ctx, dir);
    const std::string db = now.Json();
    const std::string origin = Origin();
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"begin\",\"origin\":",
                            JsonStr(origin), ",\"db\":", db, "}"));
    dirs_[dir].last = db;
    dirs_[dir].last_state = std::move(now);
    covered_.insert(dir);
    if (db.find("\"refused\"") != std::string::npos) {
      Cut(ctx, dir, "boundary: a refused boundary");
    } else if (origin == "other") {
      Unexplained(ctx, dir, "a directory row appeared at a step that does "
                            "not create one");
    }
  }
  for (auto &[dir, state] : dirs_) {
    if (state.dead || state.pending || covered_.contains(dir)) continue;
    const std::string db = Snapshot(ctx, dir);
    if (db == state.last) continue;
    // A forgotten inode (InodeForgetting) made the names that pointed at
    // it unknown, which no model step does: a cut, if that is the whole
    // change.
    if (auto it = forgotten_.find(dir); it != forgotten_.end()) {
      State expected = state.last_state;
      for (auto &[name, value] : expected.dent) {
        if (it->second.contains(name) &&
            value.starts_with("k:")) {
          value = "unknown";
        }
      }
      if (expected.Json() == db) {
        Cut(ctx, dir, "invalidated: a forgotten inode's dentries became unknown");
        continue;
      }
    }
    // Changed by something that is not one of its events: no model action
    // matches this line, so validation stops here.
    EndHeld(dir);
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"unexplained\",\"db\":", db, "}"));
    state.dead = true;
  }
  forgotten_.clear();
  // Child rows whose transaction never reported its commit rolled back.
  child_rows_.clear();
  covered_.clear();
}

// --- Frames ----------------------------------------------------------------

void TraceRecorder::RequestBegin(Context &ctx, const events::Request &r) {
  Enter("RequestBegin");
  Frame frame;
  frame.kind = Frame::Kind::kRequest;
  frame.op = r.op;
  frame.ino = r.ino;
  frame.newparent = r.newparent;
  frame.name = std::string(r.name);
  frame.newname = std::string(r.newname);
  frame.flags = r.flags;
  frame.offset = r.offset;
  frame.ioctl_arg = r.ioctl_arg;
  frames_.push_back(std::move(frame));
  After(ctx);
}

void TraceRecorder::RequestEnd(Context &ctx, const absl::Status &status) {
  Enter("RequestEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kRequest);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  const int err = ErrnoOf(status);
  FileRequestEnd(frame, err);
  Close(ctx, frame, status, [&](const Req &req) -> std::string {
    if (err == 0) return "";
    // The errors the model has: a create's EEXIST (or the name gone before
    // its probe), an unlink's or rename's ENOENT from its syscall, and the
    // EAGAIN of a readdir, unlink or rename that kept finding changes.
    if (IsCreate(req.kind) &&
        (err == EEXIST || (err == ENOENT && req.probe_absent))) {
      return "";
    }
    if ((req.kind == "unlink" || req.kind == "rename") &&
        ((err == ENOENT && req.syscall_seen && !req.syscall_ok) ||
         err == EAGAIN)) {
      return "";
    }
    if ((req.kind == "readdir" || req.kind == "readdirplus") &&
        err == EAGAIN) {
      return "";
    }
    return absl::StrCat("failed: the request failed: ", status.ToString());
  });
  After(ctx);
}

void TraceRecorder::GetattrBegin(Context &ctx, Ino id, bool valid) {
  Enter("GetattrBegin");
  frames_.push_back(Frame{.kind = Frame::Kind::kGetattr, .id = id});
  if (Traced(id)) {
    Frame *rf = InnermostRequest();
    const Mapping m = rf != nullptr ? Map(*rf, id) : Mapping{};
    const std::string fields = absl::StrCat(",\"valid\":", Bool(valid));
    if (m.kind == Mapping::Kind::kUnmodelled) {
      Cut(ctx, id, m.why);
    } else if (m.kind == Mapping::Kind::kRequest &&
               m.req_kind == "readdirplus" && rf->reqs.contains(id)) {
      // "."'s attributes, part of the readdirplus's first step (RDFrom).
      Req &req = rf->reqs[id];
      Emit(ctx, id, &req, "rdp_attr_check", fields);
      if (valid) {
        req.terminal = true;
      } else {
        req.expects_refresh = true;
      }
    } else {
      // A getattr request of its own: the FUSE request's (GETATTR,
      // LOOKUP of ".", OPENDIR), or one inside another request.
      Frame &owner = (m.kind == Mapping::Kind::kRequest &&
                      m.req_kind == "getattr" && !rf->reqs.contains(id))
                         ? *rf
                         : frames_.back();
      Req &req = Open(owner, id, "getattr");
      Emit(ctx, id, &req, "attr_check", fields);
      if (valid) {
        req.terminal = true;
      } else {
        req.expects_refresh = true;
      }
    }
  }
  After(ctx);
}

void TraceRecorder::GetattrEnd(Context &ctx, const absl::Status &status) {
  Enter("GetattrEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kGetattr);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, status,
        [&](const Req &) { return FrameEnd("a getattr", status); });
  After(ctx);
}

void TraceRecorder::LookupBegin(Context &ctx, Ino parent,
                                std::string_view name) {
  Enter("LookupBegin");
  frames_.push_back(Frame{.kind = Frame::Kind::kLookup,
                          .id = parent,
                          .lookup_name = std::string(name)});
  After(ctx);
}

void TraceRecorder::LookupEnd(Context &ctx, const absl::Status &status) {
  Enter("LookupEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kLookup);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, status,
        [&](const Req &) { return FrameEnd("a lookup", status); });
  After(ctx);
}

void TraceRecorder::RefreshBegin(Context &ctx, Ino id) {
  Enter("RefreshBegin");
  frames_.push_back(Frame{.kind = Frame::Kind::kRefresh, .id = id});
  if (Traced(id)) {
    // A refresh a request of the directory expects (a getattr's, a
    // readdirplus's or a mutation's last steps) is that request's.
    bool expected = false;
    for (size_t i = frames_.size() - 1; i-- > 0;) {
      auto it = frames_[i].reqs.find(id);
      if (it != frames_[i].reqs.end() && it->second.expects_refresh) {
        expected = true;
        break;
      }
      if (frames_[i].kind == Frame::Kind::kRequest) break;
    }
    if (!expected) {
      absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx, id);
      if (attr.ok() && attr->valid) {
        // Of valid attributes: the model would serve them, and refreshing
        // a correct value changes nothing it can see. Its fill must record
        // nothing if a mutation of the directory begins or ends meanwhile
        // (checked in AttrsFilled).
        frames_.back().silent = true;
        frames_.back().mark = dirs_[id].mutation_lines;
      } else {
        // A getattr of its own, whose check found them unknown.
        Req &req = Open(frames_.back(), id, "getattr");
        Emit(ctx, id, &req, "attr_check", ",\"valid\":false");
        req.expects_refresh = true;
      }
    }
  }
  After(ctx);
}

void TraceRecorder::RefreshEnd(Context &ctx, const absl::Status &status) {
  Enter("RefreshEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kRefresh);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, status,
        [&](const Req &) { return FrameEnd("a refresh", status); });
  After(ctx);
}

void TraceRecorder::SyncBegin(Context &ctx) {
  Enter("SyncBegin");
  frames_.push_back(Frame{.kind = Frame::Kind::kSync});
  After(ctx);
}

void TraceRecorder::SyncEnd(Context &ctx, const absl::Status &status) {
  Enter("SyncEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kSync);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, status,
        [&](const Req &) { return FrameEnd("a sync point", status); });
  After(ctx);
}

// --- Reading -----------------------------------------------------------------

void TraceRecorder::LookupDecided(Context &ctx, Ino parent,
                                  std::string_view name,
                                  events::LookupOutcome outcome, Ino child) {
  Enter("LookupDecided");
  if (outcome == events::LookupOutcome::kFound) {
    Resolved(ctx, parent, name, KeyOf(ctx, child));
  }
  // ResolveName takes its fill snapshot after this (with nothing that
  // emits a line between): marking here is the earlier, stricter point.
  if (outcome == events::LookupOutcome::kResolve) {
    listing_marks_[parent] = mutation_seq_;
  }
  if (Traced(parent)) {
    CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kLookup);
    Req *req = Find(parent);
    if (req == nullptr) {
      Frame *rf = InnermostRequest();
      const Mapping m = rf != nullptr ? Map(*rf, parent) : Mapping{};
      if (m.kind == Mapping::Kind::kUnmodelled) {
        Cut(ctx, parent, m.why);
      } else if (m.kind == Mapping::Kind::kRequest) {
        if (m.req_kind == "lookup" || m.req_kind == "unlink" ||
            m.req_kind == "rename") {
          req = &Open(*rf, parent, m.req_kind, m.n, m.m);
        } else {
          // Not a step of this kind of request: the model rejects it.
          req = &Open(*rf, parent, m.req_kind, m.n, m.m);
        }
      } else {
        // A lookup of its own (LookupOrPopulate outside any request of
        // this directory, e.g. a test resolving a name directly).
        req = &Open(frames_.back(), parent, "lookup", EscapeBytes(name));
      }
    }
    if (req != nullptr) {
      std::string fields = absl::StrCat(",\"n\":", Name(name));
      switch (outcome) {
        case events::LookupOutcome::kFound: {
          absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx, child);
          CHECK_OK(attr.status());
          absl::StrAppend(&fields, ",\"out\":\"found\",\"key\":",
                          JsonStr(Key(attr->backing_ino, attr->btime.tv_sec,
                                      attr->btime.tv_nsec)));
          break;
        }
        case events::LookupOutcome::kNegative:
          absl::StrAppend(&fields, ",\"out\":\"neg\"");
          break;
        case events::LookupOutcome::kResolve:
          absl::StrAppend(&fields, ",\"out\":\"resolve\"");
          break;
        case events::LookupOutcome::kPopulate:
          absl::StrAppend(&fields, ",\"out\":\"populate\"");
          break;
        case events::LookupOutcome::kRefused:
          break;
      }
      if (outcome == events::LookupOutcome::kRefused) {
        Cut(ctx, parent, "boundary: a refused boundary");
      } else {
        Emit(ctx, parent, req, "lookup", fields);
        if (outcome == events::LookupOutcome::kFound ||
            outcome == events::LookupOutcome::kNegative) {
          req->terminal = true;
        }
      }
    }
  }
  After(ctx);
}

void TraceRecorder::ResolveProbed(Context &ctx, Ino parent,
                                  std::string_view name,
                                  const events::Probe &probe) {
  Enter("ResolveProbed");
  if (probe.kind == events::Probe::Kind::kPresent) {
    Resolved(ctx, parent, name, Key(probe.ino, probe.btime_sec, probe.btime_nsec));
  }
  if (Traced(parent)) {
    Req *req = Find(parent);
    if (req == nullptr) {
      Unexplained(ctx, parent, "a resolve outside a request");
    } else if (probe.kind == events::Probe::Kind::kRefused) {
      Cut(ctx, parent, "boundary: a refused boundary");
    } else {
      Emit(ctx, parent, req, "probe",
           absl::StrCat(",\"n\":", Name(name), ",\"what\":",
                        ProbeValue(probe)));
    }
  }
  After(ctx);
}

void TraceRecorder::ResolveCommitted(Context &ctx, Ino parent,
                                     std::string_view name, uint64_t snapshot,
                                     bool recorded) {
  Enter("ResolveCommitted");
  if (Traced(parent)) {
    Req *req = Find(parent);
    if (req == nullptr) {
      Unexplained(ctx, parent, "a resolve outside a request");
    } else {
      Emit(ctx, parent, req, "resolve_commit",
           absl::StrCat(",\"recorded\":", Bool(recorded)));
      req->terminal = true;
    }
  }
  ChildFills(ctx, parent);
  After(ctx);
}

void TraceRecorder::ChildRowRecorded(Context &ctx, Ino dir, Ino child,
                                     bool filled) {
  Enter("ChildRowRecorded");
  // Inside the caller's transaction: its line once that committed.
  child_rows_[dir].emplace_back(child, filled);
}

void TraceRecorder::ChildFills(Context &ctx, Ino dir) {
  auto it = child_rows_.find(dir);
  if (it == child_rows_.end()) return;
  auto mark = listing_marks_.find(dir);
  for (const auto &[child, filled] : it->second) {
    if (child == dir || !Traced(child)) continue;
    Fill(ctx, child, mark == listing_marks_.end() ? -1 : mark->second, filled);
  }
  child_rows_.erase(it);
}

void TraceRecorder::MutationLine(Ino dir, bool begun) {
  Dir &state = dirs_[dir];
  ++state.mutation_lines;
  state.last_mutation = ++mutation_seq_;
  // A trace that began with a mutation of the directory in flight sees
  // its end without its phase 1: never below zero.
  if (begun) {
    ++state.open_mutations;
  } else if (state.open_mutations > 0) {
    --state.open_mutations;
  }
}

void TraceRecorder::Fill(Context &ctx, Ino dir, int64_t mark, bool filled) {
  // The code's own decision, checked against what the trace saw: recording
  // although a mutation of the directory began or ended since the fill's
  // snapshot event, or is in flight, is unexplained (the model's whole
  // getattr would not record, and over valid attributes it would not even
  // run). The recorder's own record, not cache::CanFill, so that a fault in
  // the guard or a snapshot taken late is not judged by itself.
  const Dir &state = dirs_[dir];
  if (filled && mark < 0) {
    Unexplained(ctx, dir,
                "attributes recorded by a fill whose snapshot event was not "
                "seen");
    return;
  }
  if (filled && (state.last_mutation > mark || state.open_mutations > 0)) {
    Unexplained(ctx, dir,
                "attributes recorded although a mutation of the directory "
                "began or ended since the fill's snapshot");
    return;
  }
  Emit(ctx, dir, nullptr, "child_fill",
       absl::StrCat(",\"filled\":", Bool(filled)));
}

void TraceRecorder::PopulateStarted(Context &ctx, Ino dir) {
  Enter("PopulateStarted");
  // PopulateDirectory took its fill snapshot just before.
  listing_marks_[dir] = mutation_seq_;
  if (Traced(dir)) {
    Req *req = Find(dir);
    Dir &state = dirs_[dir];
    if (req == nullptr) {
      Unexplained(ctx, dir, "a listing outside a request");
    } else if (state.reading) {
      Cut(ctx, dir,
          "overlapping-listings: a listing during another listing's reads");
    } else {
      state.read_db = Snapshot(ctx, dir);
      state.read_slot = req->slot;
      state.reading = true;
    }
  }
  After(ctx);
}

void TraceRecorder::PopulateRead(Context &ctx, Ino dir,
                                 events::ListingFn listing) {
  Enter("PopulateRead");
  if (Traced(dir)) {
    Dir &state = dirs_[dir];
    std::string list;
    bool refused = false;
    listing([&](std::string_view name, const events::Probe &probe) {
      if (probe.kind == events::Probe::Kind::kRefused) refused = true;
      if (probe.kind == events::Probe::Kind::kPresent) {
        Resolved(ctx, dir, name,
                 Key(probe.ino, probe.btime_sec, probe.btime_nsec));
      }
      absl::StrAppend(&list, list.empty() ? "" : ",", "[", Name(name), ",",
                      ProbeValue(probe), "]");
    });
    if (!state.reading) {
      Unexplained(ctx, dir, "a listing read without its start");
    } else {
      // The line goes where the population started, before the lines
      // held since: the model's PopulateRead is one step at that point.
      // (If a request that ran during the reads changed a name the reads
      // saw, no behavior matches and validation fails: README.)
      std::vector<std::string> held = std::move(state.held);
      state.held.clear();
      state.reading = false;
      if (refused) {
        Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":",
                                JsonStr(cause_),
                                ",\"ev\":\"cut\",\"why\":\"boundary: a refused boundary\"}"));
        state.dead = true;
      } else {
        Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":",
                                JsonStr(cause_),
                                ",\"ev\":\"populate_read\",\"p\":\"p",
                                state.read_slot, "\",\"listing\":[", list,
                                "],\"db\":", state.read_db, "}"));
        for (const std::string &json : held) Write(dir, json);
      }
      covered_.insert(dir);
    }
  }
  After(ctx);
}

void TraceRecorder::PopulateCommitted(Context &ctx, Ino dir,
                                      uint64_t snapshot, bool recorded) {
  Enter("PopulateCommitted");
  if (Traced(dir)) {
    Req *req = Find(dir);
    if (req == nullptr) {
      Unexplained(ctx, dir, "a listing outside a request");
    } else {
      Emit(ctx, dir, req, "populate_commit",
           absl::StrCat(",\"recorded\":", Bool(recorded)));
      req->terminal = true;
    }
  }
  ChildFills(ctx, dir);
  After(ctx);
}

void TraceRecorder::ListChecked(Context &ctx, Ino dir, bool complete) {
  Enter("ListChecked");
  if (Traced(dir)) {
    Frame *rf = InnermostRequest();
    const Mapping m = rf != nullptr ? Map(*rf, dir) : Mapping{};
    if (m.kind == Mapping::Kind::kRequest &&
        (m.req_kind == "readdir" || m.req_kind == "readdirplus")) {
      Req &req = rf->reqs.contains(dir) ? rf->reqs[dir]
                                        : Open(*rf, dir, m.req_kind);
      Emit(ctx, dir, &req, "list_check",
           absl::StrCat(",\"complete\":", Bool(complete)));
      if (complete && req.kind == "readdir") req.terminal = true;
    } else {
      Unexplained(ctx, dir, "a listing check outside a readdir");
    }
  }
  After(ctx);
}

void TraceRecorder::AttrsStatted(Context &ctx, Ino id) {
  Enter("AttrsStatted");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kRefresh &&
        frames_.back().id == id);
  if (Traced(id) && !frames_.back().silent) {
    Req *req = Find(id);
    if (req == nullptr) {
      Unexplained(ctx, id, "a refresh outside a request");
    } else {
      Emit(ctx, id, req, "stat");
    }
  }
  After(ctx);
}

void TraceRecorder::AttrsFilled(Context &ctx, Ino id, bool recorded) {
  Enter("AttrsFilled");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kRefresh &&
        frames_.back().id == id);
  if (Traced(id) && frames_.back().silent) {
    auto inflight = ctx.fills.inflight.find(id);
    if (recorded && (dirs_[id].mutation_lines != frames_.back().mark ||
                     inflight != ctx.fills.inflight.end())) {
      Unexplained(ctx, id,
                  "a refresh of valid attributes recorded although a "
                  "mutation of the directory began or ended since it began");
    }
  } else if (Traced(id)) {
    Req *req = Find(id);
    if (req == nullptr) {
      Unexplained(ctx, id, "a refresh outside a request");
    } else {
      Emit(ctx, id, req, "fill",
           absl::StrCat(",\"recorded\":", Bool(recorded)));
      req->expects_refresh = false;
      req->terminal = true;
    }
  }
  After(ctx);
}

void TraceRecorder::ParentLookupStarted(Context &ctx, Ino dir) {
  Enter("ParentLookupStarted");
  parent_marks_[dir] = mutation_seq_;
  After(ctx);
}

void TraceRecorder::ParentRecorded(Context &ctx, Ino dir, Ino parent,
                                   uint64_t snapshot, bool filled) {
  Enter("ParentRecorded");
  auto mark = parent_marks_.find(dir);
  if (Traced(parent)) {
    Fill(ctx, parent, mark == parent_marks_.end() ? -1 : mark->second,
         filled);
  }
  if (mark != parent_marks_.end()) parent_marks_.erase(mark);
  After(ctx);
}

void TraceRecorder::RootRecorded(Context &ctx) {
  Enter("RootRecorded");
  // UpsertRoot records the root's attributes unconditionally, at startup,
  // with nothing in flight.
  if (Traced(cache::kRootInode)) {
    Emit(ctx, cache::kRootInode, nullptr, "child_fill", ",\"filled\":true");
  }
  After(ctx);
}

// --- Mutations ---------------------------------------------------------------

std::string TraceRecorder::KeyOf(Context &ctx, Ino id) {
  absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx, id);
  if (!attr.ok()) return "";
  return Key(attr->backing_ino, attr->btime.tv_sec, attr->btime.tv_nsec);
}

void TraceRecorder::Resolved(Context &ctx, Ino dir, std::string_view name,
                             std::string key) {
  if (Frame *rf = InnermostRequest(); rf != nullptr) {
    rf->resolved[{dir, std::string(name)}] = std::move(key);
  }
}

void TraceRecorder::ItselfOrUnexplained(Context &ctx, const Frame &rf,
                                        Ino dir) {
  // The directory as an object (removed, moved): the model has no mutation
  // of D itself, so its trace ends -- if this request resolved one of its
  // names to D. A request naming D otherwise (a wrong id, say) is
  // unexplained.
  const std::string key = KeyOf(ctx, dir);
  auto resolved_to = [&](Ino parent, const std::string &name) {
    auto it = rf.resolved.find({parent, name});
    return it != rf.resolved.end() && !key.empty() && it->second == key;
  };
  if (resolved_to(rf.ino, rf.name) ||
      (rf.newparent != 0 && resolved_to(rf.newparent, rf.newname))) {
    Cut(ctx, dir, "dir-itself: a mutation of the directory itself");
  } else {
    Unexplained(ctx, dir,
                "a mutation names the directory in a request that did not "
                "resolve it");
  }
}

void TraceRecorder::MutationBegun(Context &ctx, events::IdsFn ids,
                                  bool synced) {
  Enter("MutationBegun");
  Frame *rf = InnermostRequest();
  for (Ino dir : Distinct(ids)) {
    if (!Traced(dir)) continue;
    if (rf == nullptr) {
      Unexplained(ctx, dir, "a mutation outside a request");
      continue;
    }
    const Mapping m = Map(*rf, dir);
    if (m.kind == Mapping::Kind::kUnmodelled) {
      Cut(ctx, dir, m.why);
      continue;
    }
    if (m.kind == Mapping::Kind::kNone) {
      ItselfOrUnexplained(ctx, *rf, dir);
      continue;
    }
    // Any other phase 1 is the request's line, for the model to judge: a
    // second phase 1 in one create, one in a request that is no mutation,
    // or an unlink's or rename's before its resolve all are rejected.
    Req *req = rf->reqs.contains(dir)
                   ? &rf->reqs[dir]
                   : &Open(*rf, dir, m.req_kind, m.n, m.m);
    req->begun = true;
    Emit(ctx, dir, req, "phase1",
         absl::StrCat(",\"outcome\":\"begun\",\"synced\":", Bool(synced)));
    MutationLine(dir, /*begun=*/true);
  }
  After(ctx);
}

void TraceRecorder::MutationAborted(Context &ctx, events::IdsFn ids) {
  Enter("MutationAborted");
  Frame *rf = InnermostRequest();
  for (Ino dir : Distinct(ids)) {
    if (!Traced(dir)) continue;
    Req *req = nullptr;
    if (rf != nullptr) {
      const Mapping m = Map(*rf, dir);
      if (m.kind == Mapping::Kind::kUnmodelled) {
        Cut(ctx, dir, m.why);
        continue;
      }
      if (m.kind == Mapping::Kind::kRequest) {
        // As for phase 1: the model judges where it may come.
        req = rf->reqs.contains(dir) ? &rf->reqs[dir]
                                     : &Open(*rf, dir, m.req_kind, m.n, m.m);
      }
    }
    if (req == nullptr) {
      if (rf != nullptr && Map(*rf, dir).kind == Mapping::Kind::kNone) {
        ItselfOrUnexplained(ctx, *rf, dir);
      } else {
        Unexplained(ctx, dir, "a verification outside its request");
      }
      continue;
    }
    Emit(ctx, dir, req, "phase1", ",\"outcome\":\"aborted\"");
  }
  After(ctx);
}

std::vector<TraceRecorder::Ino> TraceRecorder::MutatedDirs(const Frame &rf) {
  std::vector<Ino> dirs = {rf.ino};
  if (rf.newparent != 0 && rf.newparent != rf.ino) dirs.push_back(rf.newparent);
  std::vector<Ino> out;
  for (Ino dir : dirs) {
    if (!Traced(dir)) continue;
    const Mapping m = Map(rf, dir);
    if (m.kind == Mapping::Kind::kRequest && IsMutation(m.req_kind)) {
      out.push_back(dir);
    }
  }
  return out;
}

void TraceRecorder::MutationSyscallStarting(Context &ctx) {
  Enter("MutationSyscallStarting");
  if (Frame *rf = InnermostRequest(); rf != nullptr) {
    for (Ino dir : MutatedDirs(*rf)) {
      auto it = rf->reqs.find(dir);
      if (it == rf->reqs.end() || !it->second.begun) {
        Unexplained(ctx, dir, "a mutation's syscall started before its phase 1");
      }
    }
  }
  After(ctx);
}

void TraceRecorder::MutationSyscall(Context &ctx, const absl::Status &status) {
  Enter("MutationSyscall");
  Frame *rf = InnermostRequest();
  if (rf != nullptr) {
    const int err = ErrnoOf(status);
    // The syscall's line goes to every directory the request mutates as a
    // modelled request, whether or not its phase 1 has begun (or its
    // syscall has run before): where the model has no syscall step for it,
    // it rejects the line.
    for (Ino dir : MutatedDirs(*rf)) {
      const Mapping m = Map(*rf, dir);
      Req &req = rf->reqs.contains(dir)
                     ? rf->reqs[dir]
                     : Open(*rf, dir, m.req_kind, m.n, m.m);
      const bool modelled =
          err == 0 || (IsCreate(req.kind) && err == EEXIST) ||
          (!IsCreate(req.kind) && err == ENOENT);
      if (!modelled) {
        // Whether the request then failed (the model's requests always
        // finish: a cut) or replied OK regardless (unexplained), its
        // RequestEnd says.
        Defer(dir, req, absl::StrCat("a syscall error the model does not "
                                     "have: ",
                                     status.ToString()));
        continue;
      }
      req.syscall_seen = true;
      req.syscall_ok = err == 0;
      // The names whose backing state the syscall may have changed.
      std::string names = JsonStr(req.n);
      if (!req.m.empty()) absl::StrAppend(&names, ",", JsonStr(req.m));
      Emit(ctx, dir, &req, "syscall",
           absl::StrCat(",\"errno\":", err, ",\"names\":[", names, "]"));
    }
  }
  After(ctx);
}

void TraceRecorder::NewChildProbed(Context &ctx, Ino parent,
                                   std::string_view name,
                                   const events::Probe &probe) {
  Enter("NewChildProbed");
  if (Traced(parent)) {
    Req *req = RequestReq(parent);
    if (req == nullptr || req->kind != "create") {
      Unexplained(ctx, parent, "a create's probe outside its request");
    } else {
      req->probe_absent = probe.kind == events::Probe::Kind::kAbsent;
      Emit(ctx, parent, req, "probe",
           absl::StrCat(",\"n\":", Name(name), ",\"what\":",
                        ProbeValue(probe)));
    }
  }
  After(ctx);
}

void TraceRecorder::MutationEnding(Context &ctx, events::IdsFn ids,
                                   absl::FunctionRef<bool(Ino)> owns) {
  Enter("MutationEnding");
  for (Ino dir : Distinct(ids)) {
    if (Req *req = RequestReq(dir); req != nullptr) req->owned = owns(dir);
  }
  // No check of the other directories here: the caller's phase 3 has just
  // committed, and its line is MutationEnded's (one model step).
}

void TraceRecorder::MutationEnded(Context &ctx, events::IdsFn ids) {
  Enter("MutationEnded");
  for (Ino dir : Distinct(ids)) {
    if (!Traced(dir)) continue;
    Req *found = RequestReq(dir);
    if (found == nullptr || !IsMutation(found->kind)) {
      Unexplained(ctx, dir, "a mutation's end outside its request");
      continue;
    }
    Req &req = *found;
    if (!req.syscall_seen && req.interrupted) {
      // Interrupted between its phase 1 and its syscall: the model's
      // Interrupt, which Ends it (Interrupted left its line to here).
      Emit(ctx, dir, &req, "interrupt");
      MutationLine(dir, /*begun=*/false);
      continue;
    }
    if (!req.syscall_seen) {
      // Before its syscall: the end of a request that failed before its
      // syscall (an OpenNode error, say), or a forbidden step. Which one,
      // its RequestEnd says (Defer).
      Defer(dir, req, "a mutation ended before its syscall");
      MutationLine(dir, /*begun=*/false);
      continue;
    }
    Emit(ctx, dir, &req, "end",
         absl::StrCat(",\"owned\":", Bool(req.owned)));
    MutationLine(dir, /*begun=*/false);
    if (req.syscall_ok && !req.probe_absent) req.expects_refresh = true;
  }
  After(ctx);
}

void TraceRecorder::NameResolved(Context &ctx, Ino parent,
                                 std::string_view name, bool found) {
  Enter("NameResolved");
  if (Traced(parent)) {
    Req *req = RequestReq(parent);
    if (req == nullptr || (req->kind != "unlink" && req->kind != "rename")) {
      Unexplained(ctx, parent, "a resolve outside an unlink or rename");
    } else {
      Emit(ctx, parent, req, "resolved",
           absl::StrCat(",\"n\":", Name(name), ",\"found\":", Bool(found)));
    }
  }
  After(ctx);
}

void TraceRecorder::Reresolve(Context &ctx, Ino parent,
                              std::string_view name) {
  Enter("Reresolve");
  if (Traced(parent)) {
    Req *req = RequestReq(parent);
    if (req == nullptr || !IsMutation(req->kind)) {
      Unexplained(ctx, parent, "a re-resolve outside its mutation");
    } else {
      Emit(ctx, parent, req, "reresolve",
           absl::StrCat(",\"n\":", Name(name)));
    }
  }
  After(ctx);
}

void TraceRecorder::WritesEnded(Context &ctx, Ino id) {
  Enter("WritesEnded");
  // A file's: the model has no writable opens (and so no file).
  if (Traced(id)) Unexplained(ctx, id, "writes ended on a directory");
  After(ctx);
}

// --- Sync points -------------------------------------------------------------

void TraceRecorder::SyncSnapshotTaken(Context &ctx) {
  Enter("SyncSnapshotTaken");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kSync);
  frames_.back().snapshot_at = callbacks_;
  for (auto &[dir, state] : dirs_) {
    if (state.dead) continue;
    if (shutdown_) {
      Emit(ctx, dir, nullptr, "stop_sync");
    } else {
      Req &req = Open(frames_.back(), dir, "sync");
      Emit(ctx, dir, &req, "sync_begin");
    }
  }
  After(ctx);
}

void TraceRecorder::SyncfsStarting(Context &ctx) {
  Enter("SyncfsStarting");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kSync);
  // The syncfs calls must follow the snapshot with nothing in between (the
  // model's S1 is both): a snapshot taken later, or anything run between,
  // is unexplained.
  if (frames_.back().snapshot_at != callbacks_ - 1) {
    for (auto &[dir, state] : dirs_) {
      if (!state.dead) {
        Unexplained(ctx, dir,
                    "a sync point's syncfs started without its snapshot "
                    "right before");
      }
    }
  }
  After(ctx);
}

void TraceRecorder::SyncfsDone(Context &ctx) {
  Enter("SyncfsDone");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kSync);
  for (auto &[dir, req] : frames_.back().reqs) {
    if (Traced(dir)) Emit(ctx, dir, &req, "syncfs");
  }
  After(ctx);
}

void TraceRecorder::SyncCleared(Context &ctx) {
  Enter("SyncCleared");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::Kind::kSync);
  if (shutdown_) {
    for (auto &[dir, state] : dirs_) {
      if (!state.dead) Emit(ctx, dir, nullptr, "stop_clear");
    }
  } else {
    // Only the directories traced when the sync point began: a directory
    // created since is not in its snapshot of the dirty set, and its row
    // stays.
    for (auto &[dir, req] : frames_.back().reqs) {
      if (!Traced(dir)) continue;
      Emit(ctx, dir, &req, "sync_clear");
      req.terminal = true;
    }
  }
  After(ctx);
}

// --- Startup and shutdown ----------------------------------------------------

void TraceRecorder::RunLineWritten(Context &ctx, Ino dir) {
  auto it = dirs_.find(dir);
  if (it == dirs_.end() || it->second.dead) return;
  it->second.last_state = SnapshotState(ctx, dir);
  it->second.last = it->second.last_state.Json();
  covered_.insert(dir);
}

void TraceRecorder::RunStarting(Context &ctx) {
  Enter("RunStarting");
  absl::StatusOr<bool> clean = GetCleanShutdown(ctx.db);
  CHECK_OK(clean.status());
  dirty_keys_.clear();
  {
    absl::StatusOr<sqlite3::Statement *> stmt = ctx.db.Prepared(
        "SELECT i.backing_ino, i.btime_s, i.btime_ns FROM dirty d "
        "JOIN inodes i ON i.id = d.inode");
    CHECK_OK(stmt.status());
    CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &row) {
      dirty_keys_.push_back(Key(row.Column<uint64_t>(0), row.Column<int64_t>(1),
                                row.Column<int64_t>(2)));
      return absl::OkStatus();
    }));
  }
  // A new process knows nothing of the traces of the last one: every
  // directory gets these lines, and the traces that have begun take them.
  for (Ino dir : AllDirs(ctx)) {
    const std::string db = Snapshot(ctx, dir);
    if (!*clean) {
      Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                              ",\"ev\":\"crash\",\"db\":", db, "}"));
    }
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"restart\",\"db\":", db, "}"));
    RunLineWritten(ctx, dir);
  }
  // The nodeids' traces: a new mount holds no nodeid (lifetime.tla's Crash,
  // or the Restart after a DESTROY).
  LifeRunLines(ctx, "RunStarting", *clean ? "restart" : "crash");
  IdentRunLines(ctx, "RunStarting", *clean ? "restart" : "crash");
}

void TraceRecorder::Recovered(Context &ctx) {
  Enter("Recovered");
  // The keys of the inodes that were dirty (RunStarting read them): only a
  // dentry pointing at one of them may recovery make unknown in a clean
  // directory (Trace.tla's T_Recover).
  std::string keys;
  for (const std::string &key : dirty_keys_) {
    absl::StrAppend(&keys, keys.empty() ? "" : ",", JsonStr(key));
  }
  for (Ino dir : AllDirs(ctx)) {
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"recover\",\"dirty_keys\":[", keys,
                            "],\"db\":", Snapshot(ctx, dir), "}"));
    RunLineWritten(ctx, dir);
  }
  dirty_keys_.clear();
}

void TraceRecorder::RunStarted(Context &ctx) {
  Enter("RunStarted");
  for (Ino dir : AllDirs(ctx)) {
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"start_run\",\"db\":",
                            Snapshot(ctx, dir), "}"));
    RunLineWritten(ctx, dir);
  }
  // After the sweep of unnamed rows (lifetime.tla's Restart).
  LifeRunLines(ctx, "RunStarted", "start");
  IdentRunLines(ctx, "RunStarted", "start");
  // The directories whose traces begin now (the others began in an earlier
  // run, and ignore this second "begin").
  After(ctx);
}

void TraceRecorder::RecoveryDone(Context &ctx) {
  Enter("RecoveryDone");
  // Every directory, as the start's other lines: the probed rows left the
  // dirty set (dcfs.tla's ClearRecovered).
  for (Ino dir : AllDirs(ctx)) {
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"recovery_done\",\"db\":",
                            Snapshot(ctx, dir), "}"));
    RunLineWritten(ctx, dir);
  }
}

void TraceRecorder::ShutdownBegin(Context &ctx) {
  Enter("ShutdownBegin");
  shutdown_ = true;
  for (auto &[dir, state] : dirs_) {
    if (!state.dead) Emit(ctx, dir, nullptr, "shutdown");
  }
  After(ctx);
}

void TraceRecorder::Checkpointed(Context &ctx) {
  Enter("Checkpointed");
  for (auto &[dir, state] : dirs_) {
    if (!state.dead) Emit(ctx, dir, nullptr, "checkpoint");
  }
  After(ctx);
}

void TraceRecorder::CleanShutdownRecorded(Context &ctx) {
  Enter("CleanShutdownRecorded");
  for (auto &[dir, state] : dirs_) {
    if (!state.dead) Emit(ctx, dir, nullptr, "clean");
  }
  After(ctx);
}

// --- Steps the model does not have -------------------------------------------

void TraceRecorder::OutOfBandChange(Context &ctx, Ino id) {
  Enter("OutOfBandChange");
  if (Traced(id)) Cut(ctx, id, "out-of-band: an out-of-band change");
  After(ctx);
}

void TraceRecorder::InodeForgetting(Context &ctx, Ino id) {
  Enter("InodeForgetting");
  // The present rows pointing at it, which its deletion makes unknown
  // (schema.sql's inodes_delete_unknowns).
  absl::StatusOr<sqlite3::Statement *> stmt = ctx.db.Prepared(
      "SELECT parent, name FROM dentries WHERE inode = ? AND state = "
      "'present'");
  CHECK_OK(stmt.status());
  CHECK_OK((*stmt)->Bind(1, id));
  CHECK_OK((*stmt)->ForEachRow([&](sqlite3::Statement &row) {
    forgotten_[row.Column<int64_t>(0)].insert(row.Column<std::string>(1));
    return absl::OkStatus();
  }));
}

void TraceRecorder::InodeForgotten(Context &ctx, Ino id) {
  // Its identity trace's row is gone (ident.tla: an unlink's phase 3, an
  // invalidation by a probe of a recycled inode number, ForgetStale).
  if (idents_.contains(id)) IdentLine(ctx, id, "InodeForgotten", "gone");
  Enter("InodeForgotten");
  // Its own trace ends ("gone"), and the directories that named it are cut
  // if that is all that changed (After), once its transaction committed.
  After(ctx);
}

// --- Files (formal/reval.tla) ---------------------------------------------

namespace {

// An open's access mode, as reval.tla's Modes spell it.
const char *ModeOf(int flags) {
  if ((flags & O_ACCMODE) == O_RDONLY) return "r";
  return (flags & O_APPEND) != 0 ? "wa" : "w";
}

std::string FdJson(const events::SharedFd &fd) {
  using WriteFd = events::SharedFd::WriteFd;
  const char *sfd = !fd.held ? "none" : fd.writable ? "rw" : "ro";
  const char *wfd = fd.write_fd == WriteFd::kNone    ? "none"
                    : fd.write_fd == WriteFd::kPlain ? "plain"
                                                     : "append";
  return absl::StrCat("{\"sfd\":", JsonStr(sfd), ",\"wfd\":", JsonStr(wfd),
                      ",\"refs\":", fd.refs, ",\"wrefs\":", fd.writable_refs,
                      "}");
}

}  // namespace

bool TraceRecorder::FileTraced(Ino id) const {
  auto it = files_.find(id);
  return it != files_.end() && !it->second.dead;
}

void TraceRecorder::FileLine(Ino id, std::string_view ev,
                             std::string_view fields) {
  WriteLine(absl::StrCat("DCFS-REVAL ", trace_, " ", id, " {\"i\":",
                         ++file_line_, ",\"c\":", JsonStr(cause_),
                         ",\"ev\":", JsonStr(ev), fields, "}\n"));
}

void TraceRecorder::FileCut(Ino id, std::string_view why) {
  FileLine(id, "cut", absl::StrCat(",\"why\":", JsonStr(why)));
  files_[id].dead = true;
}

void TraceRecorder::FileOpened(Context &ctx, Ino id, int flags, bool shared,
                               const absl::Status &status,
                               const events::SharedFd &after) {
  Enter("FileOpened");
  // An open stopped at its checkpoint (EINTR) opened nothing.
  if (ErrnoOf(status) == EINTR) {
    After(ctx);
    return;
  }
  if (files_enabled_ && !files_.contains(id) && !shared) {
    // A file's trace begins with no open of it outstanding (the model's
    // Init: no shared fd, no handle); one first seen shared is not traced.
    files_.emplace(id, FileTrace{});
    FileLine(id, "begin");
  }
  if (FileTraced(id)) {
    const int err = ErrnoOf(status);
    // The model's opens fail only with the backing file's flags (EPERM);
    // the kernel's own refusal (EACCES, default_permissions) never reaches
    // dcfs.
    if (err != 0 && err != EPERM) {
      FileCut(id, absl::StrCat("failed: the open failed: ", status.ToString()));
    } else {
      FileLine(id, "open",
               absl::StrCat(",\"mode\":", JsonStr(ModeOf(flags)),
                            ",\"shared\":", Bool(shared), ",\"errno\":", err,
                            ",\"fd\":", FdJson(after)));
    }
  }
  After(ctx);
}

void TraceRecorder::FileReleased(Context &ctx, Ino id, bool writable,
                                 const events::SharedFd &after) {
  Enter("FileReleased");
  if (FileTraced(id)) {
    FileLine(id, "release",
             absl::StrCat(",\"writable\":", Bool(writable),
                          ",\"fd\":", FdJson(after)));
  }
  After(ctx);
}

// --- Interrupts -------------------------------------------------------------

void TraceRecorder::Interrupted(Context &ctx) {
  Enter("Interrupted");
  // Every directory request the innermost FUSE request (and the frames
  // inside it) is: the model's Interrupt of each.
  for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
    for (auto &[dir, req] : it->reqs) {
      if (!Traced(dir) || !req.arrived || req.interrupted) continue;
      req.interrupted = true;
      Dir &state = dirs_[dir];
      if (state.reading && state.read_slot == req.slot) {
        // Between a population's probe batches: its reads are abandoned.
        // The line goes where the population started (as PopulateRead's
        // would have), before the lines held since.
        std::vector<std::string> held = std::move(state.held);
        state.held.clear();
        state.reading = false;
        Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":",
                                JsonStr(cause_),
                                ",\"ev\":\"interrupt\",\"p\":\"p", req.slot,
                                "\",\"db\":", state.read_db, "}"));
        for (const std::string &json : held) Write(dir, json);
        // The next line carries its state (the held lines changed it).
        state.last.clear();
        covered_.insert(dir);
      } else if (req.begun && !req.syscall_seen) {
        // Between its phase 1 and its syscall: its End comes next, and the
        // line with it (MutationEnded).
      } else {
        Emit(ctx, dir, &req, "interrupt");
      }
    }
    if (it->kind == Frame::Kind::kRequest) break;
  }
  After(ctx);
}

void TraceRecorder::NoteOutOfBand(Ino id) {
  cause_ = "NoteOutOfBand";
  if (FileTraced(id)) FileLine(id, "oob");
}

void TraceRecorder::FileRequestEnd(const Frame &request, int err) {
  if (!FileTraced(request.ino)) return;
  const Ino id = request.ino;
  const std::string errno_field = absl::StrCat(",\"errno\":", err);
  switch (request.op) {
    case events::Op::kIoctl:
      if (request.flags == FS_IOC_SETFLAGS) {
        FileLine(id, "setflags",
                 absl::StrCat(errno_field, ",\"imm\":",
                              Bool((request.ioctl_arg & FS_IMMUTABLE_FL) != 0),
                              ",\"app\":",
                              Bool((request.ioctl_arg & FS_APPEND_FL) != 0)));
      } else if (request.flags == FS_IOC_FSSETXATTR) {
        // Its xflags can set the immutable and append-only flags too; the
        // values are left free.
        FileLine(id, "setflags", errno_field);
      } else if (request.flags == FS_IOC_GETFLAGS) {
        FileLine(id, "getflags", errno_field);
      }
      break;
    case events::Op::kSetattr:
      // A change of the mode or owner: what the kernel's permission check
      // reads (the model's "mode", left free: the trace does not know the
      // caller).
      if ((request.flags & (FUSE_SET_ATTR_MODE | FUSE_SET_ATTR_UID |
                            FUSE_SET_ATTR_GID)) != 0) {
        FileLine(id, "chmod", errno_field);
      }
      break;
    case events::Op::kWrite:
    case events::Op::kFallocate:
    case events::Op::kCopyFileRange:
      // What dcfs writes itself, through the file's write fd (WriteFd):
      // EBADF if it has none that can write. Other errors are not the
      // model's.
      if (err != 0 && err != EBADF) {
        FileCut(id, absl::StrCat("failed: a write failed: errno ", err));
      } else {
        FileLine(id, "write", errno_field);
      }
      break;
    default:
      break;
  }
}

// --- Nodeids' lifetime traces (formal/lifetime.tla) -------------------------

namespace {

const char *WrittenName(events::Lifetime::Written written) {
  switch (written) {
    case events::Lifetime::Written::kNo:
      return "no";
    case events::Lifetime::Written::kNoFd:
      return "nofd";
    case events::Lifetime::Written::kHeld:
      return "held";
  }
  return "?";
}

// What DirCacheFS keeps for a nodeid, as a line's "st" spells it (the row
// follows: RowJson).
std::string KeptJson(const events::Lifetime &kept) {
  return absl::StrCat("\"lk\":", kept.lookups, ",\"rec\":", Bool(kept.removed),
                      ",\"wr\":", JsonStr(WrittenName(kept.written)),
                      ",\"refs\":", kept.refs);
}

}  // namespace

std::string TraceRecorder::RowJson(Context &ctx, Ino id) {
  absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx, id);
  if (!attr.ok()) {
    CHECK(absl::IsNotFound(attr.status())) << attr.status();
    return "\"row\":false,\"nl0\":false";
  }
  return absl::StrCat("\"row\":true,\"nl0\":", Bool(attr->st.st_nlink == 0));
}

void TraceRecorder::LifeLine(Ino id, std::string_view cause,
                             std::string_view ev, std::string_view fields) {
  WriteLine(absl::StrCat("DCFS-LIFE ", trace_, " ", id, " {\"i\":",
                         ++life_line_, ",\"c\":", JsonStr(cause),
                         ",\"ev\":", JsonStr(ev), fields, "}\n"));
}

void TraceRecorder::LifeRunLines(Context &ctx, std::string_view cause,
                                 std::string_view ev) {
  for (Ino id : lives_) {
    LifeLine(id, cause, ev, absl::StrCat(",\"st\":{", RowJson(ctx, id), "}"));
  }
}

void TraceRecorder::LifetimeChanged(Context &ctx, Ino id,
                                    events::LifetimeStep step, uint64_t arg,
                                    events::LifetimeFn after) {
  // Not Enter(): no directory's trace sees these, and the callback count
  // is the sync points' (SyncfsStarting).
  if (cache::IsStub(id) || (!lifetimes_enabled_ && !identities_enabled_)) {
    return;
  }
  using events::LifetimeStep;
  const events::Lifetime kept = after();
  if (identities_enabled_) IdentStep(ctx, id, step, kept);
  if (!lifetimes_enabled_) return;
  const bool creates =
      step == LifetimeStep::kCreated || step == LifetimeStep::kTmpfile;
  if (!lives_.contains(id)) {
    // A trace begins with no lookup of the nodeid counted (the model's
    // initial state); one first seen with lookups counted is not traced.
    if ((step != LifetimeStep::kLookup && !creates) || kept.lookups != 1) {
      return;
    }
    lives_.insert(id);
    // The state before the step: a created nodeid had nothing (and no
    // row); a looked-up one what it has now, less the lookup.
    events::Lifetime before;
    if (!creates) {
      before = kept;
      before.lookups = 0;
    }
    LifeLine(id, "LifetimeChanged", "begin",
             absl::StrCat(",\"st\":{", KeptJson(before), ",",
                          creates ? "\"row\":false,\"nl0\":false"
                                  : RowJson(ctx, id),
                          "}"));
  }
  std::string ev;
  std::string fields;
  switch (step) {
    case LifetimeStep::kLookup: {
      // Which request handed it out: a LINK (the model's Link), a LOOKUP
      // of "." or ".." (by no name of the object), or by a name.
      std::string via = "lookup";
      if (const Frame *request = InnermostRequest(); request != nullptr) {
        if (request->op == events::Op::kLink ||
            request->op == events::Op::kLinkTmpfile) {
          via = "link";
        } else if (request->op == events::Op::kLookup &&
                   (request->name == "." || request->name == "..")) {
          via = "dot";
        }
      }
      ev = "lookup";
      fields = absl::StrCat(",\"via\":", JsonStr(via));
      break;
    }
    case LifetimeStep::kCreated:
      ev = "create";
      fields = absl::StrCat(",\"w\":", Bool(arg != 0));
      break;
    case LifetimeStep::kTmpfile:
      ev = "tmpfile";
      break;
    case LifetimeStep::kOpened:
      ev = "open";
      fields = absl::StrCat(",\"w\":", Bool(arg != 0));
      break;
    case LifetimeStep::kReleased:
      ev = "release";
      fields = absl::StrCat(",\"w\":", Bool(arg != 0));
      break;
    case LifetimeStep::kForgot:
    case LifetimeStep::kForgotInBatch:
      ev = "forget";
      fields = absl::StrCat(",\"n\":", arg, ",\"batch\":",
                            Bool(step == LifetimeStep::kForgotInBatch));
      break;
    case LifetimeStep::kRemoved:
      ev = "removed";
      fields = absl::StrCat(",\"held\":", Bool(arg != 0));
      break;
    case LifetimeStep::kProbed:
      ev = "probe";
      fields = absl::StrCat(",\"gone\":", Bool(arg != 0));
      break;
  }
  LifeLine(id, "LifetimeChanged", ev,
           absl::StrCat(fields, ",\"st\":{", KeptJson(kept), ",",
                        RowJson(ctx, id), "}"));
}

void TraceRecorder::Destroyed(Context &ctx) {
  LifeRunLines(ctx, "Destroyed", "destroy");
  IdentRunLines(ctx, "Destroyed", "destroy");
}

// --- Nodeids' identity traces (formal/ident.tla) ---------------------------

namespace {

// How a field of what a reopen reached compares with the row's: the same,
// another, or unknown (0 on either side: not reported, or not read).
const char *Compare(bool known, bool equal) {
  if (!known) return "unknown";
  return equal ? "same" : "other";
}

}  // namespace

bool TraceRecorder::HasRow(Context &ctx, Ino id) {
  absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx, id);
  CHECK(attr.ok() || absl::IsNotFound(attr.status())) << attr.status();
  return attr.ok();
}

void TraceRecorder::IdentLine(Context &ctx, Ino id, std::string_view cause,
                              std::string_view ev, std::string_view fields,
                              std::string_view more) {
  WriteLine(absl::StrCat("DCFS-IDENT ", trace_, " ", id, " {\"i\":",
                         ++ident_line_, ",\"c\":", JsonStr(cause),
                         ",\"ev\":", JsonStr(ev), fields, ",\"st\":{\"row\":",
                         Bool(HasRow(ctx, id)), more, "}}\n"));
}

void TraceRecorder::IdentRunLines(Context &ctx, std::string_view cause,
                                  std::string_view ev) {
  for (Ino id : idents_) IdentLine(ctx, id, cause, ev);
}

void TraceRecorder::IdentStep(Context &ctx, Ino id, events::LifetimeStep step,
                              const events::Lifetime &kept) {
  using events::LifetimeStep;
  const bool creates =
      step == LifetimeStep::kCreated || step == LifetimeStep::kTmpfile;
  const bool reply = step == LifetimeStep::kLookup || creates;
  const bool forget =
      step == LifetimeStep::kForgot || step == LifetimeStep::kForgotInBatch;
  if (!reply && !forget) return;
  if (!idents_.contains(id)) {
    // As the lifetime traces: at the reply dcfs counts first (the kernel
    // held no inode for it before), the row as it was (a created nodeid
    // had none).
    if (!reply || kept.lookups != 1) return;
    idents_.insert(id);
    WriteLine(absl::StrCat("DCFS-IDENT ", trace_, " ", id, " {\"i\":",
                           ++ident_line_,
                           ",\"c\":\"LifetimeChanged\",\"ev\":\"begin\","
                           "\"st\":{\"row\":",
                           Bool(!creates && HasRow(ctx, id)), "}}\n"));
  }
  const std::string lk = absl::StrCat(",\"lk\":", kept.lookups);
  if (forget) {
    IdentLine(ctx, id, "LifetimeChanged", "forget", "", lk);
    return;
  }
  // Which request handed it out (as the lifetime traces' "via").
  std::string via = step == LifetimeStep::kCreated   ? "create"
                    : step == LifetimeStep::kTmpfile ? "tmpfile"
                                                     : "lookup";
  if (const Frame *request = InnermostRequest();
      request != nullptr && step == LifetimeStep::kLookup) {
    if (request->op == events::Op::kLink ||
        request->op == events::Op::kLinkTmpfile) {
      via = "link";
    } else if (request->op == events::Op::kLookup &&
               (request->name == "." || request->name == "..")) {
      via = "dot";
    }
  }
  // The generation the reply carried: the row's (EntryFor read it, and
  // nothing wrote it since). None if the row is gone (the model rejects a
  // reply without one).
  absl::StatusOr<uint32_t> gen = cache::GetGeneration(ctx, id);
  CHECK(gen.ok() || absl::IsNotFound(gen.status())) << gen.status();
  IdentLine(ctx, id, "LifetimeChanged", "reply",
            absl::StrCat(",\"via\":", JsonStr(via), ",\"fgen\":\"",
                         gen.ok() ? absl::StrCat(*gen) : "", "\""),
            lk);
}

void TraceRecorder::IdentityResolved(Context &ctx, Ino id,
                                     const events::IdentityCheck &check) {
  if (!identities_enabled_ || !idents_.contains(id)) return;
  using Outcome = events::IdentityCheck::Outcome;
  if (check.outcome == Outcome::kStaleHandle) {
    IdentLine(ctx, id, "IdentityResolved", "resolve",
              ",\"outcome\":\"stale_handle\"");
    return;
  }
  const bool row_btime = check.row_btime_sec != 0 || check.row_btime_nsec != 0;
  const bool found_btime =
      check.found_btime_known &&
      (check.found_btime_sec != 0 || check.found_btime_nsec != 0);
  const bool same_btime = check.found_btime_sec == check.row_btime_sec &&
                          check.found_btime_nsec == check.row_btime_nsec;
  IdentLine(
      ctx, id, "IdentityResolved", "resolve",
      absl::StrCat(
          ",\"outcome\":",
          JsonStr(check.outcome == Outcome::kServed ? "served" : "mismatch"),
          ",\"ino\":",
          JsonStr(Compare(true, check.found_ino == check.row_ino)),
          ",\"gen\":",
          JsonStr(Compare(check.row_gen != 0 && check.found_gen != 0,
                          check.found_gen == check.row_gen)),
          ",\"bt\":",
          JsonStr(Compare(row_btime && found_btime, same_btime))));
}

}  // namespace dcfs::testonly
