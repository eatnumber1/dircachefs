#include "dfs/ublk.h"

#include <utility>
#include <unistd.h>

#include "absl/log/log.h"
#include "absl/log/check.h"
#include "absl/cleanup/cleanup.h"

namespace dfs {

absl::Status UblkDevice::UpdateAffinity(ublksrv_ctrl_dev &ctrl) {
  return ErrnoToStatus(
      -ublksrv_ctrl_get_affinity(&ctrl), "ublksrv_ctrl_get_affinity");
}

absl::Status UblkDevice::KernelDelete(ublksrv_ctrl_dev &ctrl) {
  return ErrnoToStatus(-ublksrv_ctrl_del_dev(&ctrl), "ublksrv_ctrl_del_dev");
}

absl::Status UblkDevice::KernelAdd(ublksrv_ctrl_dev &ctrl) {
  return ErrnoToStatus(-ublksrv_ctrl_add_dev(&ctrl), "ublksrv_ctrl_add_dev");
}

absl::Status UblkDevice::KernelGetInfo() {
  return ErrnoToStatus(-ublksrv_ctrl_get_info(ctrl_), "ublksrv_ctrl_get_info");
}

absl::StatusOr<UblkDevice> UblkDevice::Create(Options opts) {
  // Unfortunately init_tgt does't have a private data argument, so we abuse
  // argv to pass in the Options. We have to make sure that the fields we're
  // going to modify aren't already present.
  if (opts.data.tgt_argv != nullptr) {
    return absl::InvalidArgumentError("opts.data.tgt_argv must be nullptr");
  }
  if (!(opts.params.types & UBLK_PARAM_TYPE_BASIC)) {
    return absl::InvalidArgumentError("Must include UBLK_PARAM_TYPE_BASIC params");
  }
  auto *tgt_ops = const_cast<ublksrv_tgt_type *>(opts.data.tgt_ops);
  if (tgt_ops == nullptr) {
    return absl::InvalidArgumentError("opts.data.tgt_ops must not be nullptr");
  }
  if (tgt_ops->init_tgt != nullptr) {
    return absl::InvalidArgumentError("opts.data.tgt_ops.init_tgt must be nullptr");
  }
  tgt_ops->init_tgt = [](ublksrv_dev *dev, int, int, char **data) -> int {
    auto &opts = *reinterpret_cast<Options*>(data);
    const struct ublksrv_ctrl_dev_info &info =
      *ublksrv_ctrl_get_dev_info(ublksrv_get_ctrl_dev(dev));
    struct ublksrv_tgt_info &tgt = dev->tgt;

    // TODO is dev_sectors always in 512-byte sectors?
    tgt.dev_size = opts.params.basic.dev_sectors << 9;
    tgt.tgt_ring_depth = info.queue_depth;
    tgt.nr_fds = 0;

    // Return negative errno on errors, 0 on success.
    return 0;
  };
  opts.data.tgt_argv = reinterpret_cast<char**>(&opts);

  // Create the control device.
  // TODO if this fails it may crash the program. Fix that.
  // https://github.com/ming1/ubdsrv/blob/075ba3922882f7537e0198360648eaeb0979b0a7/lib/ublksrv_cmd.c#L141
  ublksrv_ctrl_dev *ctrl = ublksrv_ctrl_init(&opts.data);
  if (ctrl == nullptr) {
    // Failures get printed to the screen by ublksrv_ctrl_init.
    return absl::UnknownError("ublksrv_ctrl_init");
  }
  absl::Cleanup deinit_ctrl = [ctrl]() { ublksrv_ctrl_deinit(ctrl); };

  // Create the device in-kernel.
  RETURN_IF_ERROR(UblkDevice::KernelAdd(*ctrl));
  absl::Cleanup delete_dev = [ctrl]() { 
    absl::Status st = UblkDevice::KernelDelete(*ctrl);
    LOG_IF(WARNING, !st.ok()) << st;
  };

  // Update our inmemory cpu affinity to match the in-kernel one.
  RETURN_IF_ERROR(UblkDevice::UpdateAffinity(*ctrl));

  // Create the device in-memory.
  const ublksrv_dev *dev = ublksrv_dev_init(ctrl);
  if (dev == nullptr) {
    // Demo program associates ENOMEM to null return.
    return ErrnoToStatus(ENOMEM, "ublksrv_dev_init");
  }

  std::move(delete_dev).Cancel();
  std::move(deinit_ctrl).Cancel();
  UblkDevice ret(*ctrl, *dev);

  RETURN_IF_ERROR(ret.KernelSetParams(opts.params));
  RETURN_IF_ERROR(ret.KernelGetInfo());

  return ret;
}

UblkDevice::UblkDevice(ublksrv_ctrl_dev &ctrl, const ublksrv_dev &dev)
    : ctrl_(&ctrl), dev_(&dev) {}

UblkDevice::UblkDevice(UblkDevice &&o)
    : UblkDevice() {
  *this = std::move(o);
}

UblkDevice &UblkDevice::operator=(UblkDevice &&o) {
  using std::swap;
  swap(dev_, o.dev_);
  swap(ctrl_, o.ctrl_);
  return *this;
}

const ublksrv_dev *UblkDevice::Get() const { return dev_; }
const ublksrv_dev &UblkDevice::operator*() const { return *dev_; }
const ublksrv_dev *UblkDevice::operator->() const { return dev_; }
UblkDevice::operator bool() const { return dev_ != nullptr; }

UblkDevice::~UblkDevice() {
  absl::Status st = Delete();
  LOG_IF(WARNING, !st.ok()) << "Failure destroying UblkDevice: " << st;
}

absl::Status UblkDevice::Delete() {
  if (dev_ == nullptr) return absl::OkStatus();
  CHECK_NE(ctrl_, nullptr);
  // Tear down the device in-memory
  ublksrv_dev_deinit(dev_);
  // Tear down the device in-kernel
  absl::Status st = UblkDevice::KernelDelete(*ctrl_);
  // Tear down the control device
  ublksrv_ctrl_deinit(ctrl_);
  dev_ = nullptr;
  ctrl_ = nullptr;
  return st;
}

const ublksrv_ctrl_dev_info &UblkDevice::GetInfo() const {
  auto *info = ublksrv_ctrl_get_dev_info(ctrl_);
  CHECK_NE(info, nullptr);
  return *info;
}

absl::StatusOr<ublk_params> UblkDevice::GetParams() {
  ublk_params p = {};
  RETURN_IF_ERROR(
      ErrnoToStatus(
        -ublksrv_ctrl_get_params(ctrl_, &p), "ublksrv_ctrl_get_params"));
  return p;
}

absl::Status UblkDevice::KernelStart() {
  return ErrnoToStatus(
      -ublksrv_ctrl_start_dev(ctrl_, getpid()), "ublksrv_ctrl_start_dev");
}

absl::Status UblkDevice::KernelStop() {
  return ErrnoToStatus(-ublksrv_ctrl_stop_dev(ctrl_), "ublksrv_ctrl_stop_dev");
}

absl::Status UblkDevice::KernelSetParams(ublk_params &params) {
  return ErrnoToStatus(
      -ublksrv_ctrl_set_params(ctrl_, &params), "ublksrv_ctrl_set_params");
}

absl::StatusOr<UblkDevice::Stopper> UblkDevice::Start() {
  RETURN_IF_ERROR(KernelStart());
  return Stopper(*this);
}

std::ostream &operator<<(std::ostream &os, const UblkDevice &dev) {
  return os << "/dev/ublkb" << dev.GetInfo().dev_id;
}

UblkDevice::Stopper::~Stopper() {
  if (dev_ == nullptr) return;
  absl::Status st = std::move(*this).Stop();
  LOG_IF(WARNING, !st.ok()) << "Failed to stop " << dev_ << ": " << st;
}

UblkDevice::Stopper::Stopper(Stopper &&o)
    : Stopper() {
  *this = std::move(o);
}

UblkDevice::Stopper &UblkDevice::Stopper::operator=(Stopper &&o) {
  using std::swap;
  swap(dev_, o.dev_);
  return *this;
}

UblkDevice::Stopper::Stopper(UblkDevice &dev) : dev_(&dev) {}

absl::Status UblkDevice::Stopper::Stop() && {
  absl::Status ret = dev_->KernelStop();
  dev_ = nullptr;
  return ret;
}


UblkQueue::~UblkQueue() {
  if (queue_ == nullptr) return;
  ublksrv_queue_deinit(queue_);
}

absl::StatusOr<UblkQueue> UblkQueue::Create(
    const ublksrv_dev &dev, unsigned short queue_id) {
  // TODO what do I do with queue_data?
  const ublksrv_queue *queue = ublksrv_queue_init(
      &dev, queue_id, /*queue_data=*/nullptr);
  if (queue == nullptr) return absl::UnknownError("ublksrv_queue_init");
  return UblkQueue(*queue);
}

UblkQueue::UblkQueue(const ublksrv_queue &queue) : queue_(&queue) {}

UblkQueue::UblkQueue(UblkQueue &&o)
    : UblkQueue() {
  *this = std::move(o);
}

UblkQueue &UblkQueue::operator=(UblkQueue &&o) {
  using std::swap;
  swap(queue_, o.queue_);
  return *this;
}

const ublksrv_queue *UblkQueue::Get() const { return queue_; }
const ublksrv_queue &UblkQueue::operator*() const { return *queue_; }
const ublksrv_queue *UblkQueue::operator->() const { return queue_; }
UblkQueue::operator bool() const { return queue_ != nullptr; }

absl::StatusOr<int> UblkQueue::ProcessIo() {
  int processed = ublksrv_process_io(queue_);
  if (processed < 0) return ErrnoToStatus(-processed, "ublksrv_process_io");
  return processed;
}

}  // dfs
