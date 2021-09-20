#pragma once

#include <gtsam_ext/types/voxelized_frame.hpp>
#include <gtsam_ext/types/gaussian_voxelmap_cpu.hpp>
#include <gtsam_ext/types/gaussian_voxelmap_gpu.hpp>

namespace gtsam_ext {

struct VoxelizedFrameCPU : public VoxelizedFrame {
public:
  using Ptr = std::shared_ptr<VoxelizedFrameCPU>;
  using ConstPtr = std::shared_ptr<const VoxelizedFrameCPU>;

  VoxelizedFrameCPU(
    double voxel_resolution,
    const std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d>>& points,
    const std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d>>& covs);

  std::unique_ptr<GaussianVoxelMapCPU> voxels_storage;
  std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d>> points_storage;
  std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d>> covs_storage;
};

}  // namespace gtsam_ext
