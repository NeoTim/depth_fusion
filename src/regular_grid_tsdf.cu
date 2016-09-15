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
#include "regular_grid_tsdf.h"

#include <cassert>

#include <CUDA/Event.h>
#include <CUDA/MathUtils.h>
#include <CUDA/VecmathConversions.h>

#include "fuse.h"
#include "marching_cubes.h"
#include "raycast.h"

using libcgt::core::vecmath::SimilarityTransform;
using libcgt::core::vecmath::inverse;

RegularGridTSDF::RegularGridTSDF(const Vector3i& resolution, float voxel_size,
  const SimilarityTransform& world_from_grid) :
  device_grid_(resolution),
  voxel_size_(voxel_size),
  bounding_box_(resolution),
  max_tsdf_val_(4 * voxel_size),
  world_from_grid_(world_from_grid),
  grid_from_world_(inverse(world_from_grid)) {
  assert(voxel_size > 0);

  Reset();

  Vector3f side_lengths = SideLengths();
}

void RegularGridTSDF::Reset() {
  TSDF empty(0, 0, max_tsdf_val_);
  device_grid_.fill(empty);
}

const SimilarityTransform& RegularGridTSDF::GridFromWorld() const {
  return grid_from_world_;
}

const SimilarityTransform& RegularGridTSDF::WorldFromGrid() const {
  return world_from_grid_;
}

Box3f RegularGridTSDF::BoundingBox() const {
  return bounding_box_;
}

Vector3i RegularGridTSDF::Resolution() const {
  return device_grid_.size();
}

float RegularGridTSDF::VoxelSize() const {
  return voxel_size_;
}

Vector3f RegularGridTSDF::SideLengths() const {
  return VoxelSize() * Resolution();
}

void RegularGridTSDF::Fuse(const Vector4f& depth_camera_flpp,
  const Range1f& depth_range,
  const Matrix4f& camera_from_world,
  DeviceArray2D<float>& depth_data) {
  libcgt::cuda::Event e;

  dim3 block_dim(16, 16, 1);
  dim3 grid_dim = libcgt::cuda::math::numBins2D(
    { device_grid_.width(), device_grid_.height() },
    block_dim
  );

  e.recordStart();
  FuseKernel<<<grid_dim, block_dim>>>(
    make_float4x4(world_from_grid_.asMatrix()),
    max_tsdf_val_,
    make_float4(depth_camera_flpp),
    make_float2(depth_range.left(), depth_range.right()),
    make_float4x4(camera_from_world),
    depth_data.readView(),
    device_grid_.writeView());
  float msElapsed = e.recordStopSyncAndGetMillisecondsElapsed();

  printf("Fuse() took : %f ms\n", msElapsed);
}

void RegularGridTSDF::Raycast(const Vector4f& depth_camera_flpp,
  const Matrix4f& world_from_camera,
  DeviceArray2D<float4>& world_points_out,
  DeviceArray2D<float4>& world_normals_out) {

  libcgt::cuda::Event e;

  dim3 block_dim(16, 16, 1);
  dim3 grid_dim = libcgt::cuda::math::numBins2D(
    { world_points_out.width(), world_points_out.height() },
    block_dim
  );

  Vector4f eye = world_from_camera * Vector4f(0, 0, 0, 1);

  e.recordStart();
  RaycastKernel<<<grid_dim, block_dim>>>(
    device_grid_.readView(),
    make_float4x4(grid_from_world_.asMatrix()),
    make_float4x4(world_from_grid_.asMatrix()),
    max_tsdf_val_,
    make_float4(depth_camera_flpp),
    make_float4x4(world_from_camera),
    make_float3(eye.xyz),
    world_points_out.writeView(),
    world_normals_out.writeView()
  );
  float msElapsed = e.recordStopSyncAndGetMillisecondsElapsed();

  printf("Raycast() took: %f ms\n", msElapsed);
}

TriangleMesh RegularGridTSDF::Triangulate() const {
  Array3D<TSDF> host_grid(Resolution());
  device_grid_.copyToHost(host_grid);

  std::vector<Vector3f> positions;
  std::vector<Vector3f> normals;
  MarchingCubes(host_grid, max_tsdf_val_, world_from_grid_,
    positions, normals);

  return ConstructMarchingCubesMesh(positions, normals);
}
