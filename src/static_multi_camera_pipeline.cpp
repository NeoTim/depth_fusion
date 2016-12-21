// Copyright 2016 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http ://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "static_multi_camera_pipeline.h"

#include <core/common/ArrayUtils.h>
#include <gflags/gflags.h>

using libcgt::core::arrayutils::cast;
using libcgt::core::cameras::Intrinsics;
using libcgt::core::vecmath::SimilarityTransform;

DEFINE_int32(only_process_mesh, -1, "Only process i-th mesh.");

StaticMultiCameraPipeline::StaticMultiCameraPipeline(
  const std::vector<RGBDCameraParameters>& camera_params,
  const std::vector<EuclideanTransform>& depth_camera_poses_cfw,
  const Vector3i& grid_resolution,
  const SimilarityTransform& world_from_grid,
  float max_tsdf_value) :
  regular_grid_(grid_resolution, world_from_grid, max_tsdf_value),

  camera_params_(camera_params),
  depth_camera_poses_cfw_(depth_camera_poses_cfw),

  depth_processor_(camera_params[0].depth.intrinsics,
                   camera_params[0].depth.depth_range),
  depth_meters_(camera_params.size()),
  depth_camera_undistort_maps_(camera_params.size()),
  undistorted_depth_meters_(camera_params.size()) {


  for (size_t i = 0; i < camera_params.size(); ++i) {
    depth_meters_[i] = DeviceArray2D<float>(camera_params[i].depth.resolution);
    depth_camera_undistort_maps_[i] = DeviceArray2D<float2>(camera_params[i].depth.resolution);
    undistorted_depth_meters_[i] = DeviceArray2D<float>(camera_params[i].depth.resolution);
    input_buffers_.emplace_back(camera_params[i].color.resolution,
                                camera_params[i].depth.resolution);

    depth_camera_undistort_maps_[i].copyFromHost(
      cast<float2>(camera_params[i].depth.undistortion_map.readView()));
  }
}

int StaticMultiCameraPipeline::NumCameras() const {
  return static_cast<int>(camera_params_.size());
}

const RGBDCameraParameters& StaticMultiCameraPipeline::GetCameraParameters(
  int camera_index ) const {
  return camera_params_[camera_index];
}

Box3f StaticMultiCameraPipeline::TSDFGridBoundingBox() const {
  return regular_grid_.BoundingBox();
}

const SimilarityTransform&
StaticMultiCameraPipeline::TSDFWorldFromGridTransform() const {
  return regular_grid_.WorldFromGrid();
}

void StaticMultiCameraPipeline::Reset() {
  regular_grid_.Reset();
}

void StaticMultiCameraPipeline::NotifyInputUpdated(int camera_index,
                                                   bool color_updated,
                                                   bool depth_updated) {
  depth_meters_[camera_index].copyFromHost(
    input_buffers_[camera_index].depth_meters);

  depth_processor_.Undistort(
    depth_meters_[camera_index], depth_camera_undistort_maps_[camera_index],
    undistorted_depth_meters_[camera_index]);
}

InputBuffer& StaticMultiCameraPipeline::GetInputBuffer(int camera_index) {
  return input_buffers_[camera_index];
}

const DeviceArray2D<float>& StaticMultiCameraPipeline::GetUndistortedDepthMap(
  int camera_index) {
  return undistorted_depth_meters_[camera_index];
}

PerspectiveCamera StaticMultiCameraPipeline::GetDepthCamera(
  int camera_index) const {
  return PerspectiveCamera(
    depth_camera_poses_cfw_[camera_index],
    camera_params_[camera_index].depth.intrinsics,
    camera_params_[camera_index].depth.resolution,
    camera_params_[camera_index].depth.depth_range.left(),
    camera_params_[camera_index].depth.depth_range.right()
  );
}

void StaticMultiCameraPipeline::Fuse() {
  // Right now, fuse them all, time aligned.
  // sweep over all the ones that are ready.

  // TODO: instead of N sweeps over the volume, for each voxel, can sweep
  // over cameras instead.
  for (size_t i = 0; i < depth_meters_.size(); ++i) {
    if (i != FLAGS_only_process_mesh && FLAGS_only_process_mesh != -1) continue;
    Vector4f flpp = {
      camera_params_[i].depth.intrinsics.focalLength,
      camera_params_[i].depth.intrinsics.principalPoint
    };
    regular_grid_.Fuse(
      flpp, camera_params_[i].depth.depth_range,
      depth_camera_poses_cfw_[i].asMatrix(),
      undistorted_depth_meters_[i]
    );
  }
}

void StaticMultiCameraPipeline::Raycast(const PerspectiveCamera& camera,
                                        DeviceArray2D<float4>& world_points,
                                        DeviceArray2D<float4>& world_normals)
{
  Intrinsics intrinsics = camera.intrinsics(world_points.size());
  Vector4f flpp{ intrinsics.focalLength, intrinsics.principalPoint };

  regular_grid_.Raycast(
    flpp,
    camera.worldFromCamera().asMatrix(),
    world_points, world_normals
  );
}

TriangleMesh StaticMultiCameraPipeline::Triangulate(
  const Matrix4f& output_from_world) const {
  TriangleMesh mesh = regular_grid_.Triangulate();

  printf("Output mesh: %lu vertices and %lu normals.\n", mesh.positions().size(),
         mesh.normals().size());
  for (Vector3f& v : mesh.positions()) {
    v = output_from_world.transformPoint(v);
  }

  for (Vector3f& n : mesh.normals()) {
    n = output_from_world.transformNormal(n);
  }

  return mesh;
}
