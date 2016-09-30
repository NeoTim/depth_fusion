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
#ifndef REGULAR_GRID_TSDF_H
#define REGULAR_GRID_TSDF_H

#include <core/geometry/TriangleMesh.h>
#include <core/vecmath/Box3f.h>
#include <core/vecmath/Matrix4f.h>
#include <core/vecmath/Range1f.h>
#include <core/vecmath/SimilarityTransform.h>
#include <core/vecmath/Vector3i.h>
#include <core/vecmath/Vector4f.h>
#include <CUDA/DeviceArray2D.h>
#include <CUDA/DeviceArray3D.h>

#include "tsdf.h"

class RegularGridTSDF {

  using SimilarityTransform = libcgt::core::vecmath::SimilarityTransform;

public:

  // "resolution: number of voxels in each direction.
  // voxel_size: the physical extent of one size of each (cubical) voxel, in meters.
  RegularGridTSDF(const Vector3i& resolution, float voxel_size,
    const SimilarityTransform& world_from_grid);

  void Reset();

  // TODO(jiawen): const correct
  void Fuse(const Vector4f& depth_camera_flpp,  // Depth camera intrinsics.
    const Range1f& depth_camera_range,          // Depth camera range.
    const Matrix4f& depth_camera_from_world,    // Depth camera pose.
    DeviceArray2D<float>& depth_data);          // Depth frame, in meters.

  void Raycast(const Vector4f& depth_camera_flpp,  // Depth camera intrinsics.
    const Matrix4f& world_from_camera,             // Depth camera pose.
    DeviceArray2D<float4>& world_points_out,
    DeviceArray2D<float4>& world_normals_out);

  // The transformation that yields grid coordinates [0, resolution]^3 (in
  // samples), from world coordinates (in meters).
  const SimilarityTransform& GridFromWorld() const;

  // The transformation that yields world coordinates (in meters) from
  // grid coordinates [0, resolution]^3 (in samples).
  const SimilarityTransform& WorldFromGrid() const;

  // (0, 0, 0) --> Resolution().
  Box3f BoundingBox() const;

  // The number of samples of the grid along each axis.
  Vector3i Resolution() const;

  // The side length of a (cubical) voxel, in meters.
  float VoxelSize() const;

  // The side lengths of the entire grid, in meters.
  Vector3f SideLengths() const;

  TriangleMesh Triangulate() const;

private:

  Box3f bounding_box_; // (0, 0, 0) --> Resolution().
  SimilarityTransform grid_from_world_;
  SimilarityTransform world_from_grid_;

  DeviceArray3D<TSDF> device_grid_;
  float voxel_size_;

  // HACK: 16 mm (4 voxels).
  // TODO(jiawen): this should be dynamic, and is a function of the noise model.
  float max_tsdf_val_;
};

#endif // REGULAR_GRID_TSDF_H