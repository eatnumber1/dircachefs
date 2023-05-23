#ifndef DFS_UBLK_H_
#define DFS_UBLK_H_

#include "dfs/status.h"
#include "absl/status/statusor.h"
#include "ublksrv/ublksrv.h"

namespace dfs {

class UblkDevice {
 public:
  using Options = ublksrv_dev_data;

  UblkDevice() = default;
  ~UblkDevice();

  const ublksrv_ctrl_dev_info &GetInfo() const;

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

 private:
  UblkDevice(ublksrv_ctrl_dev &ctrl, const ublksrv_dev &dev);

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
