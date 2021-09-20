#include <fstream>
#include <iostream>
#include <boost/format.hpp>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>

#include <gtsam_ext/util/covariance_estimation.hpp>
#include <gtsam_ext/types/voxelized_frame_gpu.hpp>
#include <gtsam_ext/types/gaussian_voxelmap_cpu.hpp>
#include <gtsam_ext/types/gaussian_voxelmap_gpu.hpp>
#include <gtsam_ext/factors/integrated_icp_factor.hpp>
#include <gtsam_ext/factors/integrated_gicp_factor.hpp>
#include <gtsam_ext/factors/integrated_vgicp_factor.hpp>
#include <gtsam_ext/factors/integrated_vgicp_factor_gpu.hpp>
#include <gtsam_ext/optimizers/levenberg_marquardt_ext.hpp>
#include <gtsam_ext/optimizers/isam2_ext.hpp>
#include <gtsam_ext/cuda/stream_temp_buffer_roundrobin.hpp>

#include <glk/primitives/primitives.hpp>
#include <glk/thin_lines.hpp>
#include <glk/pointcloud_buffer.hpp>
#include <glk/normal_distributions.hpp>
#include <glk/effects/naive_screen_space_ambient_occlusion.hpp>
#include <guik/viewer/light_viewer.hpp>

struct Submap {
public:
  using Ptr = std::shared_ptr<Submap>;

  Submap(const std::string& submap_path) {
    std::ifstream points_ifs(submap_path + "/points.bin", std::ios::binary | std::ios::ate);
    if (!points_ifs) {
      std::cerr << "error: failed to open " << submap_path + "/(points|covs).bin" << std::endl;
      abort();
    }

    std::streamsize points_bytes = points_ifs.tellg();
    size_t num_points = points_bytes / (sizeof(Eigen::Vector3f));

    points_ifs.seekg(0, std::ios::beg);

    std::vector<Eigen::Vector3f, Eigen::aligned_allocator<Eigen::Vector3f>> points_f;
    points_f.resize(num_points);
    points_ifs.read(reinterpret_cast<char*>(points_f.data()), sizeof(Eigen::Vector3f) * num_points);

    points.resize(num_points);
    std::transform(points_f.begin(), points_f.end(), points.begin(), [](const Eigen::Vector3f& p) { return Eigen::Vector4d(p[0], p[1], p[2], 1.0); });
    covs = gtsam_ext::estimate_covariances(points, 10);
  }

public:
  std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d>> points;
  std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d>> covs;
};

struct GlobalMap {
public:
  GlobalMap(const std::string& dump_path) {
    std::ifstream ifs(dump_path + "/graph.txt");
    if (!ifs) {
      std::cerr << "error: failed to open " << dump_path << "/graph.txt" << std::endl;
      abort();
    }

    std::string token;
    int num_frames, num_all_frames, num_factors;

    ifs >> token >> num_frames;
    ifs >> token >> num_all_frames;
    ifs >> token >> num_factors;

    for (int i = 0; i < num_frames; i++) {
      Eigen::Vector3d trans;
      Eigen::Quaterniond quat;
      ifs >> token >> trans.x() >> trans.y() >> trans.z() >> quat.x() >> quat.y() >> quat.z() >> quat.w();

      Eigen::Isometry3d pose = Eigen::Isometry3d::Identity();
      pose.linear() = quat.toRotationMatrix();
      pose.translation() = trans;

      submap_poses.push_back(pose);
      submaps.push_back(std::make_shared<Submap>((boost::format("%s/%06d") % dump_path % i).str()));
    }

    for (int i = 0; i < num_factors; i++) {
      int first, second;
      ifs >> token >> first >> second;
      factors.push_back(std::make_pair(first, second));
    }
  }

public:
  std::vector<Submap::Ptr> submaps;
  std::vector<std::pair<int, int>> factors;
  std::vector<Eigen::Isometry3d, Eigen::aligned_allocator<Eigen::Isometry3d>> submap_poses;
};

int main(int argc, char** argv) {
  GlobalMap globalmap("/home/koide/datasets/kitti_07_dump");

  auto viewer = guik::LightViewer::instance();
  viewer->enable_vsync();
  viewer->show_info_window();
  viewer->use_topdown_camera_control(600.0);
  viewer->lookat(Eigen::Vector3f(30.0f, 180.0f, 0.0f));
  viewer->set_screen_effect(std::make_shared<glk::NaiveScreenSpaceAmbientOcclusion>());

  std::vector<gtsam_ext::VoxelizedFrameGPU::Ptr> frames;
  for (int i = 0; i < globalmap.submaps.size(); i++) {
    const auto& submap = globalmap.submaps[i];
    const auto& submap_pose = globalmap.submap_poses[i];

    gtsam_ext::VoxelizedFrameGPU::Ptr frame(new gtsam_ext::VoxelizedFrameGPU(2.0, submap->points, submap->covs));
    frames.push_back(frame);

    viewer->update_drawable("submap_" + std::to_string(i), std::make_shared<glk::PointCloudBuffer>(submap->points), guik::Rainbow(submap_pose.cast<float>()));
  }

  gtsam::Values values;
  gtsam::NonlinearFactorGraph graph;

  graph.add(gtsam::PriorFactor<gtsam::Pose3>(0, gtsam::Pose3::identity(), gtsam::noiseModel::Isotropic::Precision(6, 1e6)));
  std::vector<std::pair<int, int>> matching_cost_factors;

  std::unique_ptr<gtsam_ext::StreamTempBufferRoundRobin> stream_buffer_round_robin(new gtsam_ext::StreamTempBufferRoundRobin());
  for (int i = 0; i < globalmap.submaps.size(); i++) {
    gtsam::Pose3 noise = gtsam::Pose3::Expmap(gtsam::Vector6::Random() * 0.1);
    values.insert(i, gtsam::Pose3(globalmap.submap_poses[i].matrix()) * noise);
  }

  for (int i = 0; i < globalmap.submaps.size(); i++) {
    int target = (i + 1) % globalmap.submaps.size();

    auto stream_buffer = stream_buffer_round_robin->get_stream_buffer();
    const auto& stream = stream_buffer.first;
    const auto& buffer = stream_buffer.second;

    // gtsam_ext::IntegratedGICPFactor::shared_ptr factor(new gtsam_ext::IntegratedGICPFactor(target, i, frames[target], frames[i]));
    // gtsam_ext::IntegratedVGICPFactor::shared_ptr factor(new gtsam_ext::IntegratedVGICPFactor(target, i, frames[target], frames[i]));
    // factor->set_num_threads(12);

    // gtsam_ext::IntegratedVGICPFactorGPU::shared_ptr factor(new gtsam_ext::IntegratedVGICPFactorGPU(target, i, frames[target], frames[i], stream, buffer));

    // graph.add(factor);
    // matching_cost_factors.push_back(std::make_pair(target, i));
  }

  for (const auto& factor : globalmap.factors) {
    auto stream_buffer = stream_buffer_round_robin->get_stream_buffer();
    const auto& stream = stream_buffer.first;
    const auto& buffer = stream_buffer.second;

    // gtsam_ext::IntegratedVGICPFactor::shared_ptr f(new gtsam_ext::IntegratedVGICPFactor(factor.first, factor.second, frames[factor.first], frames[factor.second]));
    // f->set_num_threads(12);

    gtsam_ext::IntegratedVGICPFactorGPU::shared_ptr f(
      new gtsam_ext::IntegratedVGICPFactorGPU(factor.first, factor.second, frames[factor.first], frames[factor.second], stream, buffer));

    graph.add(f);
    matching_cost_factors.push_back(factor);
  }

  std::cout << "step4" << std::endl;

  /*
  gtsam_ext::ISAM2Ext isam2;
  auto result = isam2.update(graph, values);
  values = isam2.calculateEstimate();
  viewer->append_text(result.to_string());
  */

  //*
  gtsam_ext::LevenbergMarquardtExtParams lm_params;
  lm_params.setDiagonalDamping(true);
  lm_params.callback = [&](const gtsam_ext::LevenbergMarquardtOptimizationStatus& status, const gtsam::Values& values) {
    viewer->append_text(status.to_string());
    for (int i = 0; i < globalmap.submaps.size(); i++) {
      Eigen::Isometry3f matrix(values.at<gtsam::Pose3>(i).matrix().cast<float>());

      auto drawable = viewer->find_drawable("submap_" + std::to_string(i));
      drawable.first->add("model_matrix", matrix);
      viewer->update_drawable(
        "coord_" + std::to_string(i),
        glk::Primitives::coordinate_system(),
        guik::VertexColor(matrix * Eigen::UniformScaling<float>(15.0f)).make_transparent());
    }

    std::vector<Eigen::Vector3f, Eigen::aligned_allocator<Eigen::Vector3f>> lines;
    for (const auto& factor : matching_cost_factors) {
      lines.push_back(values.at<gtsam::Pose3>(factor.first).translation().cast<float>());
      lines.push_back(values.at<gtsam::Pose3>(factor.second).translation().cast<float>());
    }
    viewer->update_drawable("factors", std::make_shared<glk::ThinLines>(lines), guik::FlatColor(0.0f, 1.0f, 0.0f, 1.0f).make_transparent());

    viewer->spin_once();
  };
  values = gtsam_ext::LevenbergMarquardtOptimizerExt(graph, values, lm_params).optimize();
  //*/

  viewer->spin();

  return 0;
}