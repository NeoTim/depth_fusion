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
#include "single_moving_camera_gl_state.h"

#include "libcgt/core/common/ArrayUtils.h"

#include "regular_grid_fusion_pipeline.h"

using libcgt::core::arrayutils::copy;
using libcgt::cuda::gl::Texture2D;

namespace {
  const std::string kDrawTextureVSSrc =
#include "shaders/draw_texture.vs.glsl"
    ;
  const std::string kDrawColorVSSrc =
#include "shaders/draw_color.vs.glsl"
    ;
  const std::string kPositionOnlyVSSrc =
#include "shaders/position_only.vs.glsl"
    ;
  const std::string kDrawColorFSSrc =
#include "shaders/draw_color.fs.glsl"
    ;
  const std::string kUnprojectPointCloudVSSrc =
#include "shaders/unproject_point_cloud.vs.glsl"
    ;
  const std::string kDrawTextureFSSrc =
#include "shaders/draw_texture.fs.glsl"
    ;
  const std::string kDrawColorDiscardTransparentFSSrc =
#include "shaders/draw_color_discard_transparent.fs.glsl"
    ;
  const std::string kDrawSingleColorFSSrc =
#include "shaders/draw_single_color.fs.glsl"
    ;
}  // namespace

// TODO: shaders can be shared GL state

const bool kDrawUnprojectedPointCloud = true;
const bool kDrawFullscreenRaycast = true;
const int kFullscreenRaycastDownsampleFactor = 4;

SingleMovingCameraGLState::SingleMovingCameraGLState(
  RegularGridFusionPipeline* pipeline, QOpenGLWidget* parent) :
  // HACK
  parent_(parent),
  pipeline_(pipeline),
  color_texture_(pipeline->GetCameraParameters().color.resolution,
    GLImageInternalFormat::RGB8),
  color_tracking_vis_texture_(pipeline->GetCameraParameters().color.resolution,
    GLImageInternalFormat::RGB8),
  depth_texture_(pipeline->GetCameraParameters().depth.resolution,
    GLImageInternalFormat::R32F),
  smoothed_depth_tex_(
    GLTexture2D(pipeline->GetCameraParameters().depth.resolution,
      GLImageInternalFormat::R32F),
    libcgt::cuda::gl::Texture2D::MapFlags::WRITE_DISCARD),
  smoothed_incoming_normals_tex_(
    GLTexture2D(pipeline->GetCameraParameters().depth.resolution,
      GLImageInternalFormat::RGBA32F),
    libcgt::cuda::gl::Texture2D::MapFlags::WRITE_DISCARD),
  pose_estimation_vis_tex_(
  GLTexture2D(pipeline->GetCameraParameters().depth.resolution,
    GLImageInternalFormat::RGBA8),
  libcgt::cuda::gl::Texture2D::MapFlags::WRITE_DISCARD),
  xy_coords_(2,
    pipeline->GetCameraParameters().depth.resolution.x *
    pipeline->GetCameraParameters().depth.resolution.y),
  raycasted_normals_tex_(
    GLTexture2D(pipeline->GetCameraParameters().depth.resolution,
      GLImageInternalFormat::RGBA32F),
    Texture2D::MapFlags::WRITE_DISCARD),
  free_camera_world_positions_tex_(
    GLTexture2D(pipeline->GetCameraParameters().depth.resolution,
      GLImageInternalFormat::RGBA32F),
    Texture2D::MapFlags::WRITE_DISCARD),
  free_camera_world_normals_tex_(
    GLTexture2D(pipeline->GetCameraParameters().depth.resolution,
      GLImageInternalFormat::RGBA32F),
    Texture2D::MapFlags::WRITE_DISCARD) {
  LoadShaders();

  tracked_rgb_camera_.updateColor({ 1, 0, 0, 1 });
  tracked_depth_camera_.updateColor({ 0, 0, 1, 1 });

  tsdf_bbox_.updatePositions(
    pipeline->TSDFGridBoundingBox(),
    pipeline->TSDFWorldFromGridTransform().asMatrix()
  );

  // Initialize xy_coords_.
  {
  	auto mb = xy_coords_.mapAttribute<Vector2f>(0);
  	Array2DWriteView<Vector2f> points2D(mb.view().pointer(),
      pipeline->GetCameraParameters().depth.resolution);
  	for (int y = 0; y < points2D.height(); ++y) {
  		for (int x = 0; x < points2D.width(); ++x) {
  			points2D[{x, y}] = Vector2f{ x + 0.5f, y + 0.5f };
  		}
  	}
  }

  GLTexture::SwizzleTarget swizzle_rrr1[4] =
  {
    GLTexture::SwizzleTarget::RED,
    GLTexture::SwizzleTarget::RED,
    GLTexture::SwizzleTarget::RED,
    GLTexture::SwizzleTarget::ONE
  };
  depth_texture_.setSwizzleRGBA(swizzle_rrr1);
  smoothed_depth_tex_.texture().setSwizzleRGBA(swizzle_rrr1);

  nearest_sampler_.setMinMagFilterModes(GLTextureFilterMode::NEAREST);
  nearest_sampler_.setWrapModes(GLWrapMode::CLAMP_TO_EDGE);

  linear_sampler_.setMinMagFilterModes(GLTextureFilterMode::LINEAR);
  linear_sampler_.setWrapModes(GLWrapMode::CLAMP_TO_EDGE);
}

void SingleMovingCameraGLState::OnPipelineDataChanged(PipelineDataType type) {
  // HACK: we use |= instead of = here.
  // The app is stupidly threaded and there may be multiple signals arriving
  // before a single paint. As a hack, we OR them together to bundle the
  // update.
  changed_pipeline_data_type_ |= type;
  if (changed_pipeline_data_type_ != PipelineDataType::NONE) {
    parent_->update();
  }
}

void SingleMovingCameraGLState::Resize(const Vector2i& size) {
  free_camera_world_positions_.resize(
    size / kFullscreenRaycastDownsampleFactor);
  free_camera_world_normals_.resize(
    size / kFullscreenRaycastDownsampleFactor);

  Vector2i downsampled_size = size / kFullscreenRaycastDownsampleFactor;
  free_camera_world_positions_.resize(downsampled_size);
  free_camera_world_normals_.resize(downsampled_size);

  free_camera_world_positions_tex_ =
    libcgt::cuda::gl::Texture2D(
      GLTexture2D(downsampled_size, GLImageInternalFormat::RGBA32F),
      libcgt::cuda::gl::Texture2D::MapFlags::WRITE_DISCARD
    );
  free_camera_world_normals_tex_ =
    libcgt::cuda::gl::Texture2D(
      GLTexture2D(downsampled_size, GLImageInternalFormat::RGBA32F),
      libcgt::cuda::gl::Texture2D::MapFlags::WRITE_DISCARD
    );
}

void SingleMovingCameraGLState::Render(const PerspectiveCamera& free_camera) {
  if (pipeline_ == nullptr) {
    return;
  }

  bool camera_moved = false;
  if (free_camera != free_camera_) {
    free_camera_ = free_camera;
    camera_moved = true;
  }

  // Update buffers.
  InputBuffer& input_buffer = pipeline_->GetInputBuffer();
  if (notZero(
    changed_pipeline_data_type_ & PipelineDataType::INPUT_COLOR)) {
    printf("Updating color input vis\n");
    color_texture_.set(input_buffer.color_rgb);
  }

  if (notZero(
    changed_pipeline_data_type_ & PipelineDataType::INPUT_DEPTH)) {
    depth_texture_.set(input_buffer.depth_meters);
  }

  if (notZero(
    changed_pipeline_data_type_ & PipelineDataType::SMOOTHED_DEPTH)) {
    auto mr0 = smoothed_depth_tex_.map();
    pipeline_->SmoothedDepthMeters().copyToArray(mr0.array());
    auto mr1 = smoothed_incoming_normals_tex_.map();
    pipeline_->SmoothedIncomingNormals().copyToArray(mr1.array());
  }

  if (notZero(
    changed_pipeline_data_type_ & PipelineDataType::CAMERA_POSE)) {
    {
      auto mr = pose_estimation_vis_tex_.map();
      pipeline_->PoseEstimationVisualization().copyToArray(mr.array());
    }

    tracked_rgb_camera_.updatePositions(
      pipeline_->ColorCamera());
    tracked_depth_camera_.updatePositions(
      pipeline_->DepthCamera());
  }

  if (notZero(
    changed_pipeline_data_type_ & PipelineDataType::RAYCAST_NORMALS)) {
    auto mr = raycasted_normals_tex_.map();
    pipeline_->RaycastNormals().copyToArray( mr.array() );
  }

  DrawInputsAndIntermediates();
  DrawWorldAxes();
  if (kDrawUnprojectedPointCloud) {
    DrawUnprojectedPointCloud();
  }
  DrawCameraFrustaAndTSDFGrid();
  if (kDrawFullscreenRaycast) {
    if (camera_moved ||
      notZero(changed_pipeline_data_type_ & PipelineDataType::TSDF)) {
    DrawFullscreenRaycast();
    }
  }

  changed_pipeline_data_type_ = PipelineDataType::NONE;
}

void SingleMovingCameraGLState::LoadShaders() {
  programs_.emplace("drawColorVS",
    GLSeparableProgram(GLSeparableProgram::Type::VERTEX_SHADER,
                    kDrawColorVSSrc.c_str()));
  programs_.emplace("positionOnlyVS",
    GLSeparableProgram(GLSeparableProgram::Type::VERTEX_SHADER,
                    kPositionOnlyVSSrc.c_str()));
  programs_.emplace("drawTextureVS",
    GLSeparableProgram(GLSeparableProgram::Type::VERTEX_SHADER,
                    kDrawTextureVSSrc.c_str()));
  programs_.emplace("unprojectPointCloudVS",
    GLSeparableProgram(GLSeparableProgram::Type::VERTEX_SHADER,
                    kUnprojectPointCloudVSSrc.c_str()));

  programs_.emplace("drawColorFS",
    GLSeparableProgram(GLSeparableProgram::Type::FRAGMENT_SHADER,
                    kDrawColorFSSrc.c_str()));
  programs_.emplace("drawColorDiscardTransparentFS",
    GLSeparableProgram(GLSeparableProgram::Type::FRAGMENT_SHADER,
                    kDrawColorDiscardTransparentFSSrc.c_str()));
  programs_.emplace("drawSingleColorFS",
    GLSeparableProgram(GLSeparableProgram::Type::FRAGMENT_SHADER,
                    kDrawSingleColorFSSrc.c_str()));
  programs_.emplace("drawTextureFS",
    GLSeparableProgram(GLSeparableProgram::Type::FRAGMENT_SHADER,
                    kDrawTextureFSSrc.c_str()));

  draw_color_.attachProgram(programs_["drawColorVS"]);
  draw_color_.attachProgram(programs_["drawColorFS"]);

  draw_single_color_.attachProgram(programs_["positionOnlyVS"]);
  draw_single_color_.attachProgram(programs_["drawSingleColorFS"]);

  draw_texture_.attachProgram(programs_["drawTextureVS"]);
  draw_texture_.attachProgram(programs_["drawTextureFS"]);

  unproject_point_cloud_.attachProgram(programs_["unprojectPointCloudVS"]);
  unproject_point_cloud_.attachProgram(
    programs_["drawColorDiscardTransparentFS"]);
}


void SingleMovingCameraGLState::DrawWorldAxes() {
  GLSeparableProgram& vs = programs_["drawColorVS"];
  vs.setUniformMatrix4f(0, free_camera_.viewProjectionMatrix());
  draw_color_.bind();

  world_axes_.draw();

  GLProgramPipeline::unbindAll();
}

void SingleMovingCameraGLState::DrawCameraFrustaAndTSDFGrid() {
  GLSeparableProgram& vs = programs_["drawColorVS"];
  vs.setUniformMatrix4f(0, free_camera_.viewProjectionMatrix());
  draw_color_.bind();

  tracked_rgb_camera_.draw();
  tracked_depth_camera_.draw();
  tsdf_bbox_.draw();

  GLProgramPipeline::unbindAll();
}

void SingleMovingCameraGLState::DrawUnprojectedPointCloud() {
  const int kFreeCameraFromWorldLocation = 0;
  const int kDepthCameraFLPPLocation = 1;
  const int kDepthCameraRangeMinMaxLocation = 2;
  const int kDepthWorldFromCameraLocation = 3;
  const int kDepthTextureLocation = 4;

  const int kDepthTextureUnit = 0;
  const int kColorTextureUnit = 1;

  unproject_point_cloud_.bind();
  GLSeparableProgram& vs = programs_["unprojectPointCloudVS"];
  vs.setUniformMatrix4f(kFreeCameraFromWorldLocation,
    free_camera_.viewProjectionMatrix());
  vs.setUniformVector4f(kDepthCameraFLPPLocation,
    {
      pipeline_->GetCameraParameters().depth.intrinsics.focalLength,
      pipeline_->GetCameraParameters().depth.intrinsics.principalPoint
    }
  );
  vs.setUniformVector2f(kDepthCameraRangeMinMaxLocation,
    pipeline_->GetCameraParameters().depth.depth_range.leftRight());
  vs.setUniformMatrix4f(kDepthWorldFromCameraLocation,
    pipeline_->DepthCamera().worldFromCamera().asMatrix());

  depth_texture_.bind(kDepthTextureUnit);
  nearest_sampler_.bind(kDepthTextureUnit);
  vs.setUniformInt(kDepthTextureLocation, kDepthTextureUnit);

  xy_coords_.draw();

  depth_texture_.unbind(kDepthTextureUnit);
  GLSamplerObject::unbind(kDepthTextureUnit);
  color_texture_.unbind(kColorTextureUnit);
  GLSamplerObject::unbind(kColorTextureUnit);
}

// HACK: why is the following wrong and using identity right?
namespace {
Matrix4f normalsToRGBA()
{
  Matrix4f m = Matrix4f::uniformScaling( 0.5f ) *
    Matrix4f::translation(Vector3f{1.0f});
  m.setRow(3, Vector4f{1, 0, 0, 0});

  m = Matrix4f::identity();

  return m;
}
}

void SingleMovingCameraGLState::DrawFullscreenRaycast() {
  // Update the buffer.
  pipeline_->Raycast(free_camera_,
    free_camera_world_positions_, free_camera_world_normals_);
  {
    auto mr = free_camera_world_positions_tex_.map();
    free_camera_world_positions_.copyToArray(mr.array());
  }
  {
    auto mr = free_camera_world_normals_tex_.map();
    free_camera_world_normals_.copyToArray(mr.array());
  }

  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);

  GLSeparableProgram& vs = programs_["drawTextureVS"];
  vs.setUniformMatrix4f(0, Matrix4f::identity());
  GLSeparableProgram& fs = programs_["drawTextureFS"];
  fs.setUniformInt(0, 0); // sampler
  fs.setUniformMatrix4f(1, normalsToRGBA());
  draw_texture_.bind();

  free_camera_world_normals_tex_.texture().bind(0);
  input_buffer_textured_rect_.draw();

  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
}

#include "libcgt/core/geometry/RangeUtils.h"
#include "libcgt/core/geometry/RectangleUtils.h"
#include "libcgt/core/math/Arithmetic.h"
#include "libcgt/core/vecmath/Vector4f.h"
#include "libcgt/GL/GLUtilities.h"

using libcgt::core::geometry::transformBetween;
using libcgt::core::geometry::translate;
using libcgt::core::geometry::translate;
using libcgt::core::math::floorToInt;

void SingleMovingCameraGLState::DrawRemappedTextures(
  const std::vector<RemappedTexture>& textures) {
  glDisable(GL_DEPTH_TEST);
  Rect2i vp = GLUtilities::getViewport();

  GLSeparableProgram& vs = programs_["drawTextureVS"];
  vs.setUniformMatrix4f(0, Matrix4f::identity());
  GLSeparableProgram& fs = programs_["drawTextureFS"];
  fs.setUniformInt(0, 0); // sampler
  draw_texture_.bind();

  Vector2i sz = floorToInt(
    textures[0].texture->size() * textures[0].size_scale);
  Rect2i current_rect{ { 0, 0 }, sz };
  for (const auto& tex : textures) {
    GLUtilities::setViewport(current_rect);
     fs.setUniformMatrix4f(1, tex.color_transform);
    tex.texture->bind(0);
    input_buffer_textured_rect_.draw();
    current_rect = translate(current_rect, current_rect.dx());
  }

  GLUtilities::setViewport(vp);
  glEnable(GL_DEPTH_TEST);
}

void SingleMovingCameraGLState::DrawInputsAndIntermediates() {
  Matrix4f depth_rescale_matrix =
    transformBetween(pipeline_->GetCameraParameters().depth.depth_range,
      Range1f::fromMinMax(0.2f, 1.0f));
  // Copy to the other components.
  depth_rescale_matrix(1, 1) = depth_rescale_matrix(0, 0);
  depth_rescale_matrix(1, 3) = depth_rescale_matrix(0, 3);
  depth_rescale_matrix(2, 2) = depth_rescale_matrix(0, 0);
  depth_rescale_matrix(2, 3) = depth_rescale_matrix(0, 3);
  // Set alpha to 0.
  depth_rescale_matrix(3, 0) = 1.0f;

  std::vector<RemappedTexture> textures;

  textures.push_back(
    RemappedTexture {
      &color_texture_,
      Vector2f{ 0.5f, 0.5f },
      Matrix4f::identity()
  }
  );

  textures.push_back(
    RemappedTexture {
      &color_tracking_vis_texture_,
      Vector2f{ 0.5f, 0.5f },
      Matrix4f::identity()
    }
  );

  textures.push_back(
    RemappedTexture{
      &depth_texture_,
      Vector2f{ 0.5f, 0.5f },
      depth_rescale_matrix
    }
  );

  textures.push_back(
    RemappedTexture{
      &smoothed_depth_tex_.texture(),
      Vector2f{ 0.5f, 0.5f },
      depth_rescale_matrix
    }
  );

  // TODO: normal remapping texture

  textures.push_back(
    RemappedTexture{
      &smoothed_incoming_normals_tex_.texture(),
      Vector2f{ 0.5f, 0.5f },
      normalsToRGBA()
    }
  );

  textures.push_back(
    RemappedTexture{
      &pose_estimation_vis_tex_.texture(),
      Vector2f{ 0.5f, 0.5f },
      Matrix4f::identity()
    }
  );

  textures.push_back(
    RemappedTexture{
      &raycasted_normals_tex_.texture(),
      Vector2f{ 0.5f, 0.5f },
      normalsToRGBA()
    }
  );

  DrawRemappedTextures(textures);
}
