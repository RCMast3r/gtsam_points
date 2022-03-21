#include <random>
#include <iostream>
#include <boost/format.hpp>

#include <gtsam_ext/ann/ivox.hpp>
#include <gtsam_ext/types/frame_cpu.hpp>

#include <gtsam_ext/util/read_points.hpp>
#include <gtsam_ext/util/covariance_estimation.hpp>
#include <gtsam_ext/util/edge_plane_extraction.hpp>

#include <gtsam_ext/factors/integrated_loam_factor.hpp>
#include <gtsam_ext/optimizers/levenberg_marquardt_ext.hpp>

#include <glk/pointcloud_buffer.hpp>
#include <glk/primitives/primitives.hpp>
#include <guik/viewer/light_viewer.hpp>

gtsam_ext::ScanLineInformation estimate_scan_lines() {
  const std::string path = "/home/koide/datasets/kitti/dataset/sequences/00/velodyne/000000.bin";
  const auto points_f = gtsam_ext::read_points4(path);
  std::vector<Eigen::Vector4d> points(points_f.size());
  std::transform(points_f.begin(), points_f.end(), points.begin(), [](const auto& p) { return Eigen::Vector4d(p[0], p[1], p[2], 1.0); });

  return gtsam_ext::estimate_scan_lines(points.data(), points.size(), 64, 0.2 * M_PI / 180.0);
}

int main(int argc, char** argv) {
  const std::string seq_path = "/home/koide/datasets/kitti/dataset/sequences/01/velodyne";

  auto viewer = guik::LightViewer::instance();

  gtsam_ext::iVox::Ptr ivox_edges(new gtsam_ext::iVox(1.0, 0.1, 20));
  gtsam_ext::iVox::Ptr ivox_planes(new gtsam_ext::iVox(1.0, 0.1, 20));

  gtsam_ext::ScanLineInformation scan_lines = estimate_scan_lines();

  gtsam::Pose3 T_world_lidar;
  gtsam::Pose3 T_last_current;

  for (int i = 0; i < 4500; i++) {
    const std::string path = (boost::format("%s/%06d.bin") % seq_path % i).str();
    const auto points_f = gtsam_ext::read_points4(path);
    std::vector<Eigen::Vector4d> points(points_f.size());
    std::transform(points_f.begin(), points_f.end(), points.begin(), [](const auto& p) { return Eigen::Vector4d(p[0], p[1], p[2], 1.0); });

    auto edges_planes = gtsam_ext::extract_edge_plane_points(scan_lines, points.data(), points.size());
    auto edges = edges_planes.first;
    auto planes = edges_planes.second;

    if (i != 0) {
      gtsam::Values values;
      values.insert(0, gtsam::Pose3());
      values.insert(1, T_world_lidar * T_last_current);

      gtsam::NonlinearFactorGraph graph;
      graph.emplace_shared<gtsam::PriorFactor<gtsam::Pose3>>(0, gtsam::Pose3(), gtsam::noiseModel::Isotropic::Precision(6, 1e6));

      auto factor = gtsam::make_shared<gtsam_ext::IntegratedLOAMFactor_<gtsam_ext::iVox, gtsam_ext::Frame>>(
        0,
        1,
        ivox_edges,
        ivox_planes,
        edges,
        planes,
        ivox_edges,
        ivox_planes);
      factor->set_num_threads(12);
      factor->set_max_corresponding_distance(1.0, 1.0);
      graph.add(factor);

      gtsam_ext::LevenbergMarquardtExtParams lm_params;
      lm_params.setMaxIterations(20);
      lm_params.set_verbose();

      gtsam_ext::LevenbergMarquardtOptimizerExt optimizer(graph, values, lm_params);
      values = optimizer.optimize();

      T_last_current = T_world_lidar.inverse() * values.at<gtsam::Pose3>(1);
      T_world_lidar = values.at<gtsam::Pose3>(1);
    }

    for (int i = 0; i < edges->size(); i++) {
      edges->points[i] = T_world_lidar.matrix() * edges->points[i];
    }
    for (int i = 0; i < planes->size(); i++) {
      planes->points[i] = T_world_lidar.matrix() * planes->points[i];
    }

    ivox_edges->insert(*edges);
    ivox_planes->insert(*planes);

    const auto plane_points = ivox_planes->voxel_points();
    const auto edge_points = ivox_edges->voxel_points();

    viewer->update_drawable("current_coord", glk::Primitives::coordinate_system(), guik::VertexColor(T_world_lidar.matrix().cast<float>()));
    viewer->update_drawable("plane_points", std::make_shared<glk::PointCloudBuffer>(plane_points), guik::Rainbow());
    viewer->update_drawable("edge_points", std::make_shared<glk::PointCloudBuffer>(edge_points), guik::FlatRed());

    viewer->spin_until_click();
  }

  return 0;
}