#ifndef DFS_UBLK_H_
#define DFS_UBLK_H_

#include "dfs/status.h"
#include "absl/status/statusor.h"
#include "ublksrv/ublksrv.h"

namespace dfs {

class UblkDevice {
 public:
  struct Options {
    // Options for ublksrv_ctrl_init
    ublksrv_dev_data data;
    // Params for ublksrv_ctrl_set_params
    ublk_params params;
  };

  class Stopper {
   public:
    Stopper() = default;
    ~Stopper();

    absl::Status Stop() &&;

    Stopper(Stopper &&);
    Stopper(const Stopper &) = delete;
    Stopper &operator=(Stopper &&);
    Stopper &operator=(const Stopper &) = delete;

   private:
    friend class ::dfs::UblkDevice;

    Stopper(UblkDevice &dev);

    UblkDevice *dev_ = nullptr;
  };

  UblkDevice() = default;
  ~UblkDevice();

  // Stopper captures *this, so must be destroyed before this UblkDevice.
  absl::StatusOr<Stopper> Start();

  const ublksrv_ctrl_dev_info &GetInfo() const;
  absl::StatusOr<ublk_params> GetParams();

  // TODO remove from API
  ublksrv_ctrl_dev &GetControlDevice();

  static absl::StatusOr<UblkDevice> Create(Options opts);
  absl::Status Delete();

  UblkDevice(UblkDevice &&);
  UblkDevice(const UblkDevice &) = delete;
  UblkDevice &operator=(UblkDevice &&);
  UblkDevice &operator=(const UblkDevice &) = delete;

  const ublksrv_dev *Get() const;
  const ublksrv_dev &operator*() const;
  const ublksrv_dev *operator->() const;
  operator bool() const;

  friend std::ostream &operator<<(std::ostream &os, const UblkDevice &dev);

 private:
  UblkDevice(ublksrv_ctrl_dev &ctrl, const ublksrv_dev &dev);

  absl::Status KernelSetParams(ublk_params &params);

  absl::Status KernelStart();
  absl::Status KernelStop();

  // Updates inmemory cache of device info.
  absl::Status KernelGetInfo();

  static absl::Status UpdateAffinity(ublksrv_ctrl_dev &ctrl);
  static absl::Status KernelDelete(ublksrv_ctrl_dev &ctrl);
  static absl::Status KernelAdd(ublksrv_ctrl_dev &ctrl);

  ublksrv_ctrl_dev *ctrl_ = nullptr;
  const ublksrv_dev *dev_ = nullptr;
};

class UblkQueue {
 public:
  UblkQueue() = default;
  ~UblkQueue();

  // dev is retained but not owned. Must outlive this object.
  static absl::StatusOr<UblkQueue> Create(
      const ublksrv_dev &dev, unsigned short queue_id);

  absl::StatusOr<int> ProcessIo();

  UblkQueue(UblkQueue &&);
  UblkQueue(const UblkQueue &) = delete;
  UblkQueue &operator=(UblkQueue &&);
  UblkQueue &operator=(const UblkQueue &) = delete;

  const ublksrv_queue *Get() const;
  const ublksrv_queue &operator*() const;
  const ublksrv_queue *operator->() const;
  operator bool() const;

 private:
  UblkQueue(const ublksrv_queue &queue);

  const ublksrv_queue *queue_ = nullptr;
};

}  // namespace dfs

#endif  // DFS_UBLK_H_
