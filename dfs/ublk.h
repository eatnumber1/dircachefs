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

  // ctrl is retained but not owned. Must outlive this object.
  static absl::StatusOr<UblkDevice> Create(Options opts);
  absl::Status Delete();

  absl::Status Start();
  absl::Status Stop();

  UblkDevice(UblkDevice &&);
  UblkDevice(const UblkDevice &) = delete;
  UblkDevice &operator=(UblkDevice &&);
  UblkDevice &operator=(const UblkDevice &) = delete;

  const ublksrv_dev *Get() const;
  const ublksrv_dev &operator*() const;
  operator bool() const;

 private:
  UblkDevice(ublksrv_ctrl_dev &ctrl, const ublksrv_dev &dev);

  static absl::Status UpdateAffinity(ublksrv_ctrl_dev &ctrl);
  static absl::Status KernelDelete(ublksrv_ctrl_dev &ctrl);
  static absl::Status KernelAdd(ublksrv_ctrl_dev &ctrl);

  ublksrv_ctrl_dev *ctrl_ = nullptr;
  const ublksrv_dev *dev_ = nullptr;
};

}  // namespace dfs

#endif  // DFS_UBLK_H_
