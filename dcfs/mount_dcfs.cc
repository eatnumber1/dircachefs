#include "dcfs/mount_dcfs.h"

#include <sys/mount.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/flags/reflection.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "dcfs/mounts_below.h"
#include "dcfs/status.h"

namespace dcfs {
namespace {

constexpr std::string_view kDcfsPrefix = "dcfs.";

// The Abseil flags a mount can set as `dcfs.<name>=value`: dcfs's own and the
// logging ones.
constexpr std::string_view kSettableFlags[] = {
    "attr_timeout_sec", "entry_timeout_sec", "sync_interval_sec",
    "stderrthreshold",  "minloglevel",       "v",
    "vmodule",
};

// Step 15.8: allow_other is always passed to the kernel, with
// default_permissions (which makes the kernel enforce the mode bits), so a
// line that names it is out of date.
absl::Status AllowOtherRefused(std::string_view option) {
  return InvalidArgumentErrorBuilder()
         << "Option " << option
         << ": dcfs always allows other users (default_permissions makes the "
            "kernel enforce the mode bits), so the option does nothing: "
            "remove it";
}

absl::Status BadOption(std::string_view option, std::string_view why) {
  return InvalidArgumentErrorBuilder()
         << "Option " << option << " " << why;
}

// A boolean option's value: bare is true.
absl::StatusOr<bool> ParseBoolOption(std::string_view option,
                                     std::optional<std::string_view> value) {
  if (!value.has_value() || *value == "1" || *value == "true") return true;
  if (*value == "0" || *value == "false") return false;
  return BadOption(option, "takes 0, 1, true or false");
}

// The mountinfo line for the topmost mount at `mountpoint`: the fields up to
// the "-" separator (index of the separator in `dash`), or nullopt.
struct MountinfoLine {
  std::vector<std::string_view> fields;
  size_t dash = 0;
};
std::optional<MountinfoLine> TopmostMountAt(std::string_view mountinfo,
                                            std::string_view mountpoint) {
  std::optional<MountinfoLine> found;
  for (std::string_view line : absl::StrSplit(mountinfo, '\n')) {
    std::vector<std::string_view> fields =
        absl::StrSplit(line, ' ', absl::SkipEmpty());
    if (fields.size() < 6) continue;
    if (UnescapeMountinfoPath(fields[4]) != mountpoint) continue;
    for (size_t i = 6; i + 1 < fields.size(); ++i) {
      if (fields[i] != "-") continue;
      found = MountinfoLine{.fields = std::move(fields), .dash = i};
      break;
    }
  }
  return found;
}

}  // namespace

bool IsMountHelperName(std::string_view name) {
  return name == kMountHelperName || name == kMountFuseHelperName;
}

bool IsUmountHelperName(std::string_view name) {
  return name == kUmountFuseDcfsHelperName || name == kUmountFuseHelperName;
}

absl::StatusOr<UmountArgs> ParseUmountArgs(
    std::span<const std::string> args) {
  UmountArgs parsed;
  std::vector<std::string> positionals;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg.size() < 2 || arg[0] != '-') {
      positionals.push_back(arg);
      continue;
    }
    if (arg[1] == '-') {
      // A long option: forwarded, with its argument if it takes one (attached
      // by `=` or as the next word).
      std::string_view name = std::string_view(arg).substr(0, arg.find('='));
      if (name == "--lazy") parsed.lazy = true;
      if (name == "--namespace") parsed.other_namespace = true;
      parsed.forwarded.push_back(arg);
      if (arg.find('=') == std::string::npos &&
          (name == "--namespace" || name == "--types" ||
           name == "--test-opts") &&
          i + 1 < args.size()) {
        parsed.forwarded.push_back(args[++i]);
      }
      continue;
    }
    // A bundle of short options (-lf), ended by one that takes an argument,
    // given attached (-tfuse.dcfs) or as the next word. -V is ours; -l and -N
    // change what the helper does; the rest are the unmount's own.
    parsed.forwarded.push_back(arg);
    for (size_t j = 1; j < arg.size(); ++j) {
      const char flag = arg[j];
      if (flag == 'V') parsed.version = true;
      if (flag == 'l') parsed.lazy = true;
      if (flag == 'N') parsed.other_namespace = true;
      if (flag == 't' || flag == 'N' || flag == 'O') {
        if (j + 1 == arg.size()) {
          if (i + 1 == args.size()) {
            return MarkUsageError(InvalidArgumentErrorBuilder()
                                  << "Option -" << flag
                                  << " needs an argument");
          }
          parsed.forwarded.push_back(args[++i]);
        }
        break;
      }
    }
  }
  if (parsed.version) return parsed;
  if (positionals.size() != 1) {
    return MarkUsageError(InvalidArgumentErrorBuilder()
                          << "Expected one mount point, got "
                          << positionals.size());
  }
  parsed.target = positionals[0];
  return parsed;
}

namespace {

absl::StatusOr<HelperArgs> ParseHelperArgsImpl(
    std::span<const std::string> args) {
  HelperArgs parsed;
  std::vector<std::string> positionals;
  auto add_options = [&parsed](std::string_view list) {
    for (std::string_view option : absl::StrSplit(list, ',')) {
      if (!option.empty()) parsed.options.emplace_back(option);
    }
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg.size() < 2 || arg[0] != '-') {
      positionals.push_back(arg);
      continue;
    }
    // A bundle of flags (-sfnv), ended by one that takes an argument, given
    // attached (-oro) or as the next word (-o ro).
    for (size_t j = 1; j < arg.size(); ++j) {
      const char flag = arg[j];
      switch (flag) {
        case 's': parsed.sloppy = true; break;
        case 'f': parsed.fake = true; break;
        case 'n': parsed.no_mtab = true; break;
        case 'v': ++parsed.verbose; break;
        case 'V': parsed.version = true; break;
        case 'o':
        case 'N':
        case 't': {
          std::string value;
          if (j + 1 < arg.size()) {
            value = arg.substr(j + 1);
          } else if (i + 1 < args.size()) {
            value = args[++i];
          } else {
            return InvalidArgumentErrorBuilder()
                   << "Option -" << flag << " needs an argument";
          }
          if (flag == 'o') add_options(value);
          if (flag == 'N') parsed.mount_namespace = std::move(value);
          // -t TYPE (the fstype's subtype) names nothing dcfs needs.
          j = arg.size();
          break;
        }
        default:
          return InvalidArgumentErrorBuilder()
                 << "Unknown option -" << flag << " in " << arg;
      }
    }
  }
  if (parsed.version) return parsed;
  if (positionals.empty()) {
    return InvalidArgumentErrorBuilder() << "Missing SOURCE and MOUNTPOINT";
  }
  if (positionals.size() < 2) {
    return InvalidArgumentErrorBuilder() << "Missing MOUNTPOINT";
  }
  if (positionals.size() > 2) {
    return InvalidArgumentErrorBuilder()
           << "Unexpected argument " << positionals[2];
  }
  parsed.spec = positionals[0];
  parsed.source = std::move(positionals[0]);
  parsed.mountpoint = std::move(positionals[1]);
  return parsed;
}

absl::StatusOr<HelperOptions> SplitHelperOptionsImpl(
    std::span<const std::string> options) {
  HelperOptions split;
  for (const std::string &option : options) {
    if (option.empty()) continue;
    if (option == "remount") {
      split.remount = true;
      continue;
    }
    if (option == "allow_other") return AllowOtherRefused(option);
    if (!absl::StartsWith(option, kDcfsPrefix)) {
      split.native_options.push_back(option);
      continue;
    }
    std::string_view rest = std::string_view(option).substr(kDcfsPrefix.size());
    std::string_view name = rest;
    std::optional<std::string_view> value;
    if (size_t eq = rest.find('='); eq != std::string_view::npos) {
      name = rest.substr(0, eq);
      value = rest.substr(eq + 1);
    }
    if (name == "fstype") {
      if (!value.has_value() || value->empty()) {
        return BadOption(option, "needs a value: none, bind or a type");
      }
      if (*value == "none") {
        split.backing = HelperOptions::Backing::kNone;
      } else if (*value == "bind") {
        split.backing = HelperOptions::Backing::kBind;
      } else {
        split.backing = HelperOptions::Backing::kNative;
        split.native_type = std::string(*value);
      }
    } else if (name == "ro") {
      ABSL_ASSIGN_OR_RETURN(split.read_only, ParseBoolOption(option, value));
    } else if (name == "foreground") {
      ABSL_ASSIGN_OR_RETURN(split.foreground, ParseBoolOption(option, value));
    } else if (name == "cache_db") {
      if (!value.has_value() || value->empty()) {
        return BadOption(option, "needs a path");
      }
      split.cache_db = std::string(*value);
    } else if (name == "cache_dir") {
      return UnimplementedErrorBuilder()
             << "Option " << option
             << " needs the instance identity (plan step 15.3); name the "
                "database with dcfs.cache_db for now";
    } else if (name == "fuse_opt") {
      if (!value.has_value() || value->empty()) {
        return BadOption(option, "needs a libfuse mount option");
      }
      split.fuse_options.emplace_back(*value);
    } else {
      if (name == "allow_other") return AllowOtherRefused(option);
      if (std::find(std::begin(kSettableFlags), std::end(kSettableFlags),
                    name) == std::end(kSettableFlags)) {
        return InvalidArgumentErrorBuilder()
               << "Unknown option " << option << " (dcfs options are "
               << "dcfs.fstype, dcfs.ro, dcfs.foreground, dcfs.cache_db, "
                  "dcfs.fuse_opt, dcfs.attr_timeout_sec, "
                  "dcfs.entry_timeout_sec, dcfs.sync_interval_sec, "
                  "dcfs.stderrthreshold, dcfs.minloglevel, dcfs.v and "
                  "dcfs.vmodule)";
      }
      if (!value.has_value()) return BadOption(option, "needs a value");
      split.flags.emplace_back(std::string(name), std::string(*value));
    }
  }
  if (split.backing == HelperOptions::Backing::kNone && !split.remount) {
    const std::vector<std::string> unhonored = UnhonoredNativeOptions(split);
    if (!unhonored.empty()) {
      return InvalidArgumentErrorBuilder()
             << "Options " << absl::StrJoin(unhonored, ", ")
             << " are for the underlying mount, which dcfs.fstype=none does "
                "not make: remove them, or remount the filesystem yourself "
                "(dcfs.ro makes the dcfs mount read-only)";
    }
  }
  return split;
}

}  // namespace

absl::StatusOr<HelperArgs> ParseHelperArgs(std::span<const std::string> args) {
  absl::StatusOr<HelperArgs> parsed = ParseHelperArgsImpl(args);
  if (!parsed.ok()) return MarkUsageError(parsed.status());
  return parsed;
}

absl::StatusOr<HelperOptions> SplitHelperOptions(
    std::span<const std::string> options) {
  absl::StatusOr<HelperOptions> split = SplitHelperOptionsImpl(options);
  if (!split.ok()) return MarkUsageError(split.status());
  return split;
}

std::vector<std::string> UnhonoredNativeOptions(const HelperOptions &options) {
  std::vector<std::string> unhonored;
  if (options.backing != HelperOptions::Backing::kNone && !options.remount) {
    return unhonored;
  }
  for (const std::string &option : options.native_options) {
    static constexpr std::string_view kMountOwn[] = {
        "rw",   "defaults", "nofail", "_netdev", "noauto", "auto",
        "user", "users",    "owner",  "group",   "nouser"};
    if (absl::StartsWith(option, "x-") ||
        std::find(std::begin(kMountOwn), std::end(kMountOwn), option) !=
            std::end(kMountOwn)) {
      continue;
    }
    unhonored.push_back(option);
  }
  return unhonored;
}

absl::Status MarkUsageError(absl::Status status) {
  if (!status.ok()) status.SetPayload(kUsageTypeUrl, absl::Cord("usage"));
  return status;
}

absl::Status ApplyFlagOption(const std::string &name,
                             const std::string &value) {
  absl::CommandLineFlag *flag = absl::FindCommandLineFlag(name);
  if (flag == nullptr) {
    return InternalErrorBuilder() << "No flag " << name << " to set";
  }
  std::string error;
  if (!flag->ParseFrom(value, &error)) {
    return MarkUsageError(InvalidArgumentErrorBuilder()
                          << "Option dcfs." << name << "=" << value << ": "
                          << error);
  }
  return absl::OkStatus();
}

absl::Status NativeMountError(int exit_status, std::string_view message) {
  absl::Status status = FailedPreconditionErrorBuilder() << message;
  status.SetPayload(kMountExitStatusTypeUrl, absl::Cord(absl::StrCat(exit_status)));
  return status;
}

int ExitStatusFor(const absl::Status &status) {
  if (std::optional<absl::Cord> payload = status.GetPayload(kMountExitStatusTypeUrl);
      payload.has_value()) {
    int exit_status = 0;
    if (absl::SimpleAtoi(std::string(*payload), &exit_status) &&
        exit_status > 0 && exit_status < 256) {
      return exit_status;
    }
  }
  return status.GetPayload(kUsageTypeUrl).has_value() ? 1 : 32;
}

std::string EncodeStartupReport(const StartupReport &report) {
  if (report.ready) return "R";
  std::string bytes = "E";
  bytes.push_back(static_cast<char>(report.exit_status));
  bytes += report.message;
  return bytes;
}

StartupReport DecodeStartupReport(std::string_view bytes) {
  if (bytes.empty()) {
    return {.ready = false,
            .exit_status = 32,
            .message = "dcfs exited before it was ready"};
  }
  if (bytes[0] == 'R') return {.ready = true};
  if (bytes[0] == 'E' && bytes.size() >= 2) {
    return {.ready = false,
            .exit_status = static_cast<unsigned char>(bytes[1]),
            .message = std::string(bytes.substr(2))};
  }
  return {.ready = false,
          .exit_status = 32,
          .message = "dcfs sent an unintelligible startup report"};
}

std::optional<MountEntry> TopmostMountEntry(std::string_view mountinfo,
                                            std::string_view mountpoint) {
  std::optional<MountinfoLine> line = TopmostMountAt(mountinfo, mountpoint);
  if (!line.has_value()) return std::nullopt;
  return MountEntry{.device = std::string(line->fields[2]),
                    .fstype = std::string(line->fields[line->dash + 1])};
}

std::optional<std::string> DcfsMountDevice(std::string_view mountinfo,
                                           std::string_view mountpoint) {
  std::optional<MountEntry> entry = TopmostMountEntry(mountinfo, mountpoint);
  if (!entry.has_value() || entry->fstype != "fuse.dcfs") return std::nullopt;
  return entry->device;
}

std::string DaemonLockPath(std::string_view device, std::string_view dir) {
  return absl::StrCat(dir, "/", absl::StrReplaceAll(device, {{":", "_"}}),
                      ".lock");
}

std::optional<unsigned long> RemountFlags(std::string_view mountinfo,
                                          std::string_view mountpoint,
                                          bool read_only) {
  std::optional<MountinfoLine> line = TopmostMountAt(mountinfo, mountpoint);
  if (!line.has_value() || line->fields[line->dash + 1] != "fuse.dcfs") {
    return std::nullopt;
  }
  unsigned long flags = MS_REMOUNT;
  for (std::string_view option : absl::StrSplit(line->fields[5], ',')) {
    if (option == "nosuid") flags |= MS_NOSUID;
    if (option == "nodev") flags |= MS_NODEV;
    if (option == "noexec") flags |= MS_NOEXEC;
    if (option == "noatime") flags |= MS_NOATIME;
    if (option == "nodiratime") flags |= MS_NODIRATIME;
  }
  if (read_only) flags |= MS_RDONLY;
  return flags;
}

}  // namespace dcfs
