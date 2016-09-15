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
#include <cstdio>

#include <QApplication>

#include <camera_wrappers/StreamConfig.h>
#include <core/vecmath/EuclideanTransform.h>
#include <core/vecmath/SimilarityTransform.h>

#include "aruco_detector.h"
#include "control_widget.h"
#include "input_buffer.h"
#include "main_widget.h"
#include "main_controller.h"
#include "regular_grid_fusion_pipeline.h"
#include "rgbd_camera_parameters.h"
#include "rgbd_input.h"

using libcgt::camera_wrappers::StreamConfig;
using libcgt::core::arrayutils::flipY;
using libcgt::core::cameras::Intrinsics;
using libcgt::core::vecmath::EuclideanTransform;
using libcgt::core::vecmath::SimilarityTransform;

#define USE_REALSENSE 0
#define USE_KINECT1X 0
#define USE_OPENNI2 1

#if USE_KINECT1X
#include <camera_wrappers/Kinect1x/KinectCamera.h>
const Vector2i kColorResolution = { 640, 480 };
const int kColorFps = 30;
const Vector2i kDepthResolution = { 640, 480 };
const int kDepthFps = 30;
using libcgt::kinect1x::KinectCamera;
#endif

#if USE_REALSENSE
#include <camera_wrappers/RealSense/RealSenseCamera.h>
const Vector2i kColorResolution = { 640, 480 };
const int kColorFps = 60;
const Vector2i kDepthResolution = { 480, 360 };
const int kDepthFps = 60;
#endif

#if USE_OPENNI2
#include <camera_wrappers/OpenNI2/OpenNI2Camera.h>
using libcgt::camera_wrappers::openni2::OpenNI2Camera;
#endif

//const Vector3i kRegularGridResolution(512); ~2m^3
//const Vector3i kRegularGridResolution(640); // ~2.5m^3
const Vector3i kRegularGridResolution(768); // ~3m^3
const float kRegularGridVoxelSize = 0.004f; // 4 mm.

// TODO(jiawen): clean this up:
// When running live, they should all return a unified wrapper.
// When reading from a stream, should read from a common intrinsic+extrinsic
// calibration file.
RGBDCameraParameters DefaultCameraParameters() {
  RGBDCameraParameters camera_params;

  // TODO(jiawen): move distortion coefficients into RGBDCameraParameters and
  // also have the camera wrappers return them.
  std::vector<float> color_dist_coeffs(5, 0.0f);

#if USE_REALSENSE
  const std::vector<float>& color_dist_coeffs =
    camera.ColorDistortionCoefficients();

  auto color_from_depth_extrinsics = camera.ColorFromDepthExtrinsicsMeters();
  Range1f depth_range = Range1f::fromMinMax(0.5f, 3.5f);
#endif

#if USE_KINECT1X
  camera_params.color_resolution = kColorResolution;
  camera_params.color_intrinsics = flipY(camera.colorIntrinsics(),
    camera_params.color_resolution.y);
  camera_params.color_range = camera.depthRangeMeters();
  camera_params.color_range.origin += 0.01f;

  camera_params.depth_resolution = kDepthResolution;
  camera_params.depth_intrinsics = flipY(camera.depthIntrinsics(),
    camera_params.depth_resolution.y);
  camera_params.depth_range = camera.depthRangeMeters();

  camera_params.color_from_depth =
    KinectCamera::colorFromDepthExtrinsicsMeters();
  camera_params.depth_from_color = inverse(camera_params.color_from_depth);

#endif

#if USE_OPENNI2
  OpenNI2Camera camera;

  camera_params.color_resolution = camera.colorConfig().resolution;
  camera_params.color_intrinsics = flipY(camera.colorIntrinsics(),
    camera_params.color_resolution.y);
  camera_params.color_range = OpenNI2Camera::depthRangeMeters();
  camera_params.color_range.origin += 0.01f;

  camera_params.depth_resolution = camera.depthConfig().resolution;
  camera_params.depth_intrinsics = flipY(camera.depthIntrinsics(),
    camera_params.depth_resolution.y);
  camera_params.depth_range = camera.depthRangeMeters();

  camera_params.color_from_depth = camera.colorFromDepthExtrinsicsMeters();
  camera_params.depth_from_color = inverse(camera_params.color_from_depth);
#endif

  return camera_params;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("Usage: %s <rgbd_file>\n", argv[0]);
    return 1;
  }

  QApplication app(argc, argv);

  printf("Initializing depth camera...");

#if USE_OPENNI2
  // TODO(jiawen): move these into RgbdInput and have it load a second file
  // which is the calibration.
  StreamConfig g_color_stream_config({ 640, 480 }, 30,
    libcgt::camera_wrappers::PixelFormat::RGB_U888, false);
  StreamConfig g_depth_stream_config({ 640, 480 }, 30,
    libcgt::camera_wrappers::PixelFormat::DEPTH_MM_U16, false);
  OpenNI2Camera camera(g_color_stream_config, g_depth_stream_config);

#if 0
  camera.start();
#endif

#endif

  if (!camera.isValid()) {
    printf("failed.\n");
    return 1;
  }
  printf("succeeded.\n");


  std::string detector_params_filename("detector_params.yaml");
  ArucoDetector detector(detector_params_filename);

  const int kBoardWidthPixels = 3300;
  const int kBoardHeightPixels = 2550;
  Array2D<uint8_t> gl_board_image = detector.GLBoardImage(
    kBoardWidthPixels, kBoardHeightPixels);

  RGBDCameraParameters camera_params = DefaultCameraParameters();

  const SimilarityTransform kInitialWorldFromGrid =
    SimilarityTransform(kRegularGridVoxelSize) *
    SimilarityTransform(Vector3f(-0.5f * kRegularGridResolution.x,
      -0.5f * kRegularGridResolution.y, -kRegularGridResolution.z));

  // y up
  const EuclideanTransform kInitialDepthCameraFromWorld =
    EuclideanTransform::fromMatrix(
      Matrix4f::lookAt(
        { 0, 0, camera_params.depth_range.minimum() },
        Vector3f{ 0 },
        Vector3f{ 0, 1, 0 }.normalized()
      )
    );

  const char* rgbd_stream_filename = argv[1];
  RgbdInput rgbd_input(InputType::FILE, rgbd_stream_filename);
  RegularGridFusionPipeline pipeline(camera_params,
    kRegularGridResolution, kRegularGridVoxelSize,
    kInitialWorldFromGrid,
    true, kInitialDepthCameraFromWorld);

  ControlWidget control_widget;
  control_widget.move(0, 0);
  control_widget.show();

  QGLFormat format;
  format.setVersion(4, 5);
  format.setProfile(QGLFormat::CoreProfile);
  MainWidget main_widget(camera_params, format);

  const int window_width = 1920;
  const int window_height = 1200;
  int x = control_widget.geometry().right();
  int y = control_widget.geometry().top();
  main_widget.move(x, y);
  main_widget.resize(window_width, window_height);

  // HACK:
  // 0. pass pipeline in to constructor
  // 1. shouldn't give it input_buffer, it can read it off the pipeline
  main_widget.input_buffer_ = &pipeline.input_buffer_;
  main_widget.pipeline_ = &pipeline;
  main_widget.gl_board_image_ = gl_board_image;

	main_widget.makeCurrent();
	glewExperimental = true;
	glewInit();

  // TODO(jiawen): can move GLState initialization here, since it's actually
  // data.

	main_widget.doneCurrent();

  MainController controller(&rgbd_input, &pipeline,
    &control_widget, &main_widget);

	main_widget.show();

  bool kDoColorTracking = false;
  const int kWaitMillis = 33;

#if USE_KINECT1X
  Array2D<uint8x4> bgra_frame(kColorResolution);
  Array2D<uint16_t> input_depth_frame(kDepthResolution);
  cv::Mat_<cv::Vec3b> color_vis_ocv(kColorResolution.y, kColorResolution.x);
  Array2DView<uint8x3> color_vis_view =
    cvMatAsArray2DView<uint8x3>(color_vis_ocv);

  InputBuffer state(kColorResolution, kDepthResolution);

  libcgt::kinect1x::KinectCamera::Frame frame;
  frame.bgra = bgra_frame;
  frame.extendedDepth = input_depth_frame;
#endif

#if 0
  // OpenNI2
  Array2D<uint8x3> rgb_frame(camera.colorConfig().resolution);
  Array2D<uint16_t> input_depth_frame(camera.depthConfig().resolution);
  cv::Mat_<cv::Vec3b> color_vis_ocv(camera.colorConfig().resolution.y,
    camera.colorConfig().resolution.x);
  Array2DView<uint8x3> color_vis_view =
    cvMatAsArray2DView<uint8x3>(color_vis_ocv);

  OpenNI2Camera::Frame frame;
  frame.rgb = rgb_frame.writeView();
  frame.depth = input_depth_frame;

  InputBuffer state(camera.colorConfig().resolution,
    camera.depthConfig().resolution);
#endif

	return app.exec();
}

#if 0
// TODO: This is color based tracking code, which needs to be moved into its
// own class and integrated into RegularGridFusionPipeline.

#include <core/time/TimeUtils.h>

#include <opencv2/core.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/highgui.hpp>
#include <opencv_interop/arrayutils.h>
#include <opencv_interop/vecmathutils.h>
#include <chrono>

using libcgt::core::timeutils::dtMS;
using libcgt::opencv_interop::array2DViewAsCvMat;
using libcgt::opencv_interop::cvMatAsArray2DView;
using libcgt::opencv_interop::toCV;

cv::Mat color_intrinsics_ocv = toCV(camera.colorIntrinsics().asMatrix());

ArucoDetector::PoseEstimate pose;
pose.valid = false;
if (kDoColorTracking)
{
  cv::Mat raw_color_ocv_view = array2DViewAsCvMat(
    state->raw_color.writeView());
  auto track_t0 = std::chrono::high_resolution_clock::now();
  ArucoDetector::DetectionResult markers = detector.Detect(
    raw_color_ocv_view);
  auto track_t1 = std::chrono::high_resolution_clock::now();
  detector.Refine(raw_color_ocv_view, color_intrinsics_ocv,
    color_dist_coeffs, &markers);
  auto track_t2 = std::chrono::high_resolution_clock::now();
  pose = detector.EstimatePose(markers,
    color_intrinsics_ocv, color_dist_coeffs);
  auto track_t3 = std::chrono::high_resolution_clock::now();

  printf("detection took %lld ms\n", dtMS(track_t0, track_t1));
  printf("refine took %lld ms\n", dtMS(track_t1, track_t2));
  printf("pose estimation took %lld ms\n", dtMS(track_t2, track_t3));

  raw_color_ocv_view.copyTo(color_vis_ocv);
  ArucoDetector::VisualizeDetections(markers, true, &color_vis_ocv);
  detector.VisualizePoseEstimate(pose,
    color_intrinsics_ocv, color_dist_coeffs,
    &color_vis_ocv);
}
#endif
