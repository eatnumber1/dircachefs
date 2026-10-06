#include "dcfs/testonly/trace_recorder.h"

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
    case events::Probe::kPresent:
      return JsonStr(Key(probe.ino, probe.btime_sec, probe.btime_nsec));
    case events::Probe::kAbsent:
      return JsonStr("absent");
    case events::Probe::kRefused:
      return JsonStr("refused");
  }
  return JsonStr("?");
}

int ErrnoOf(const absl::Status &status) {
  if (status.ok()) return 0;
  return GetErrnoFromStatus(status).value_or(-1);
}

bool IsMutation(const std::string &kind) {
  return kind == "create" || kind == "unlink" || kind == "rename";
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

TraceRecorder::TraceRecorder(int fd, std::string trace)
    : fd_(fd), trace_(std::move(trace)) {}

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
    m.kind = Mapping::kRequest;
    m.req_kind = std::move(kind);
    m.n = std::move(n);
    m.m = std::move(mm);
  };
  auto unmodelled = [&](std::string why) {
    m.kind = Mapping::kUnmodelled;
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
    if (it->kind == Frame::kRequest) return &*it;
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
    if (it->kind == Frame::kRequest) break;
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

void TraceRecorder::Close(
    Context &ctx, Frame &frame,
    absl::FunctionRef<std::string(const Req &)> unmodelled_end) {
  for (auto &[dir, req] : frame.reqs) {
    if (Dir &state = dirs_[dir]; state.reading && state.read_slot == req.slot) {
      // Its population failed half-way: the trace ends where it began.
      state.reading = false;
      state.held.clear();
      Cut(ctx, dir, "failed: a listing failed half-way");
    }
    if (Traced(dir) && req.arrived) {
      const std::string why = unmodelled_end(req);
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
  if (auto it = dirs_.find(dir); it != dirs_.end() && it->second.reading) {
    it->second.held.push_back(json);
    return;
  }
  const std::string line =
      absl::StrCat("DCFS-TRACE ", trace_, " ", dir, " ", json, "\n");
  size_t done = 0;
  while (done < line.size()) {
    const ssize_t n = ::write(fd_, line.data() + done, line.size() - done);
    if (n < 0 && errno == EINTR) continue;
    PCHECK(n > 0) << "writing a trace line";
    done += static_cast<size_t>(n);
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

void TraceRecorder::Unexplained(Context &ctx, Ino dir, std::string_view why) {
  if (!Traced(dir)) return;
  Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                          ",\"ev\":\"unexplained\",\"why\":", JsonStr(why),
                          ",\"db\":", Snapshot(ctx, dir), "}"));
  dirs_[dir].dead = true;
  covered_.insert(dir);
}

void TraceRecorder::Cut(Context &ctx, Ino dir, std::string_view why) {
  if (!Traced(dir)) return;
  Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                          ",\"ev\":\"cut\",\"why\":", JsonStr(why), "}"));
  dirs_[dir].dead = true;
  covered_.insert(dir);
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
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"begin\",\"db\":", db, "}"));
    dirs_[dir].last = db;
    dirs_[dir].last_state = std::move(now);
    covered_.insert(dir);
    if (db.find("\"refused\"") != std::string::npos) {
      Cut(ctx, dir, "boundary: a refused boundary");
    }
  }
  for (auto &[dir, state] : dirs_) {
    if (state.dead || covered_.contains(dir)) continue;
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
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"unexplained\",\"db\":", db, "}"));
    state.dead = true;
  }
  forgotten_.clear();
  covered_.clear();
}

// --- Frames ----------------------------------------------------------------

void TraceRecorder::RequestBegin(Context &ctx, const events::Request &r) {
  Enter("RequestBegin");
  Frame frame;
  frame.kind = Frame::kRequest;
  frame.op = r.op;
  frame.ino = r.ino;
  frame.newparent = r.newparent;
  frame.name = std::string(r.name);
  frame.newname = std::string(r.newname);
  frame.flags = r.flags;
  frame.offset = r.offset;
  frames_.push_back(std::move(frame));
  After(ctx);
}

void TraceRecorder::RequestEnd(Context &ctx, const absl::Status &status) {
  Enter("RequestEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kRequest);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  const int err = ErrnoOf(status);
  Close(ctx, frame, [&](const Req &req) -> std::string {
    if (err == 0) {
      if (req.ended_early) {
        return "unexplained: a mutation ended before its syscall, and its "
               "request succeeded";
      }
      return "";
    }
    // The errors the model has: a create's EEXIST (or the name gone before
    // its probe), an unlink's or rename's ENOENT from its syscall, and the
    // EAGAIN of a readdir, unlink or rename that kept finding changes.
    if (req.kind == "create" &&
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
  frames_.push_back(Frame{.kind = Frame::kGetattr, .id = id});
  if (Traced(id)) {
    Frame *rf = InnermostRequest();
    const Mapping m = rf != nullptr ? Map(*rf, id) : Mapping{};
    const std::string fields = absl::StrCat(",\"valid\":", Bool(valid));
    if (m.kind == Mapping::kUnmodelled) {
      Cut(ctx, id, m.why);
    } else if (m.kind == Mapping::kRequest && m.req_kind == "readdirplus" &&
               rf->reqs.contains(id)) {
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
      Frame &owner = (m.kind == Mapping::kRequest &&
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kGetattr);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, [&](const Req &) { return FrameEnd("a getattr", status); });
  After(ctx);
}

void TraceRecorder::LookupBegin(Context &ctx, Ino parent,
                                std::string_view name) {
  Enter("LookupBegin");
  frames_.push_back(Frame{.kind = Frame::kLookup,
                          .id = parent,
                          .lookup_name = std::string(name)});
  After(ctx);
}

void TraceRecorder::LookupEnd(Context &ctx, const absl::Status &status) {
  Enter("LookupEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kLookup);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, [&](const Req &) { return FrameEnd("a lookup", status); });
  After(ctx);
}

void TraceRecorder::RefreshBegin(Context &ctx, Ino id) {
  Enter("RefreshBegin");
  frames_.push_back(Frame{.kind = Frame::kRefresh, .id = id});
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
      if (frames_[i].kind == Frame::kRequest) break;
    }
    if (!expected) {
      absl::StatusOr<cache::CachedAttr> attr = cache::GetAttr(ctx, id);
      if (attr.ok() && attr->valid) {
        // Of valid attributes: the model would serve them; refreshing
        // them changes nothing it can see (and a mutation meanwhile makes
        // the fill record nothing).
        frames_.back().silent = true;
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kRefresh);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame, [&](const Req &) { return FrameEnd("a refresh", status); });
  After(ctx);
}

void TraceRecorder::SyncBegin(Context &ctx) {
  Enter("SyncBegin");
  frames_.push_back(Frame{.kind = Frame::kSync});
  After(ctx);
}

void TraceRecorder::SyncEnd(Context &ctx, const absl::Status &status) {
  Enter("SyncEnd");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kSync);
  Frame frame = std::move(frames_.back());
  frames_.pop_back();
  Close(ctx, frame,
        [&](const Req &) { return FrameEnd("a sync point", status); });
  After(ctx);
}

// --- Reading -----------------------------------------------------------------

void TraceRecorder::LookupDecided(Context &ctx, Ino parent,
                                  std::string_view name,
                                  events::LookupOutcome outcome, Ino child) {
  Enter("LookupDecided");
  if (Traced(parent)) {
    CHECK(!frames_.empty() && frames_.back().kind == Frame::kLookup);
    Req *req = Find(parent);
    if (req == nullptr) {
      Frame *rf = InnermostRequest();
      const Mapping m = rf != nullptr ? Map(*rf, parent) : Mapping{};
      if (m.kind == Mapping::kUnmodelled) {
        Cut(ctx, parent, m.why);
      } else if (m.kind == Mapping::kRequest) {
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
  if (Traced(parent)) {
    Req *req = Find(parent);
    if (req == nullptr) {
      Unexplained(ctx, parent, "a resolve outside a request");
    } else if (probe.kind == events::Probe::kRefused) {
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
                                     bool recorded, Ino child) {
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
  if (child != 0 && child != parent && Traced(child)) {
    // The child row's attributes: a whole getattr fill of that directory.
    Emit(ctx, child, nullptr, "child_fill",
         absl::StrCat(",\"allowed\":",
                      Bool(cache::CanFill(ctx, {.seq = snapshot}, child))));
  }
  After(ctx);
}

void TraceRecorder::PopulateStarted(Context &ctx, Ino dir) {
  Enter("PopulateStarted");
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
      if (probe.kind == events::Probe::kRefused) refused = true;
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
                                      uint64_t snapshot, bool recorded,
                                      events::IdsFn children) {
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
  for (Ino child : Distinct(children)) {
    if (child == dir || !Traced(child)) continue;
    Emit(ctx, child, nullptr, "child_fill",
         absl::StrCat(",\"allowed\":",
                      Bool(cache::CanFill(ctx, {.seq = snapshot}, child))));
  }
  After(ctx);
}

void TraceRecorder::ListChecked(Context &ctx, Ino dir, bool complete) {
  Enter("ListChecked");
  if (Traced(dir)) {
    Frame *rf = InnermostRequest();
    const Mapping m = rf != nullptr ? Map(*rf, dir) : Mapping{};
    if (m.kind == Mapping::kRequest &&
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kRefresh &&
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kRefresh &&
        frames_.back().id == id);
  if (Traced(id) && !frames_.back().silent) {
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

void TraceRecorder::ParentRecorded(Context &ctx, Ino parent,
                                   uint64_t snapshot) {
  Enter("ParentRecorded");
  if (Traced(parent)) {
    Emit(ctx, parent, nullptr, "child_fill",
         absl::StrCat(",\"allowed\":",
                      Bool(cache::CanFill(ctx, {.seq = snapshot}, parent))));
  }
  After(ctx);
}

void TraceRecorder::RootRecorded(Context &ctx) {
  Enter("RootRecorded");
  if (Traced(cache::kRootInode)) {
    Emit(ctx, cache::kRootInode, nullptr, "child_fill", ",\"allowed\":true");
  }
  After(ctx);
}

// --- Mutations ---------------------------------------------------------------

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
    if (m.kind == Mapping::kUnmodelled) {
      Cut(ctx, dir, m.why);
      continue;
    }
    if (m.kind == Mapping::kNone) {
      // The directory as an object (removed, moved): the model has no
      // mutation of D itself.
      Cut(ctx, dir, "dir-itself: a mutation of the directory itself");
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
      if (m.kind == Mapping::kUnmodelled) {
        Cut(ctx, dir, m.why);
        continue;
      }
      if (m.kind == Mapping::kRequest) {
        // As for phase 1: the model judges where it may come.
        req = rf->reqs.contains(dir) ? &rf->reqs[dir]
                                     : &Open(*rf, dir, m.req_kind, m.n, m.m);
      }
    }
    if (req == nullptr) {
      Unexplained(ctx, dir, "a verification outside its request");
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
    if (m.kind == Mapping::kRequest && IsMutation(m.req_kind)) out.push_back(dir);
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
          err == 0 || (req.kind == "create" && err == EEXIST) ||
          (req.kind != "create" && err == ENOENT);
      if (!modelled) {
        Cut(ctx, dir, absl::StrCat("failed: a syscall error the model does "
                                   "not have: ",
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
      req->probe_absent = probe.kind == events::Probe::kAbsent;
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
    if (!req.syscall_seen) {
      // Before its syscall: the end of a request that failed before its
      // syscall (an OpenNode error, say), or a forbidden step. Which one,
      // its RequestEnd says: a failure ends the trace there, a success is
      // unexplained.
      req.ended_early = true;
      continue;
    }
    Emit(ctx, dir, &req, "end",
         absl::StrCat(",\"owned\":", Bool(req.owned)));
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kSync);
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kSync);
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
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kSync);
  for (auto &[dir, req] : frames_.back().reqs) {
    if (Traced(dir)) Emit(ctx, dir, &req, "syncfs");
  }
  After(ctx);
}

void TraceRecorder::SyncCleared(Context &ctx) {
  Enter("SyncCleared");
  CHECK(!frames_.empty() && frames_.back().kind == Frame::kSync);
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

void TraceRecorder::RunStarting(Context &ctx) {
  Enter("RunStarting");
  absl::StatusOr<bool> clean = GetCleanShutdown(ctx.db);
  CHECK_OK(clean.status());
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
  }
}

void TraceRecorder::Recovered(Context &ctx) {
  Enter("Recovered");
  for (Ino dir : AllDirs(ctx)) {
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"recover\",\"db\":", Snapshot(ctx, dir),
                            "}"));
  }
}

void TraceRecorder::RunStarted(Context &ctx) {
  Enter("RunStarted");
  for (Ino dir : AllDirs(ctx)) {
    Write(dir, absl::StrCat("{\"i\":", ++line_, ",\"c\":", JsonStr(cause_),
                            ",\"ev\":\"start_run\",\"db\":",
                            Snapshot(ctx, dir), "}"));
  }
  // The directories whose traces begin now (the others began in an earlier
  // run, and ignore this second "begin").
  After(ctx);
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
  Enter("InodeForgotten");
  // Its own trace ends ("gone"), and the directories that named it are cut
  // if that is all that changed (After), once its transaction committed.
  After(ctx);
}

}  // namespace dcfs::testonly
