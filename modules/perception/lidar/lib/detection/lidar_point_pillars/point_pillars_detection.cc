/******************************************************************************
 * Copyright 2020 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "modules/perception/lidar/lib/detection/lidar_point_pillars/point_pillars_detection.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "cyber/common/log.h"

#include "modules/perception/base/object_pool_types.h"
#include "modules/perception/common/perception_gflags.h"
#include "modules/perception/lidar/common/lidar_timer.h"

namespace apollo {
namespace perception {
namespace lidar {

using base::Object;
using base::PointD;
using base::PointF;

// TODO(chenjiahao):
//  specify score threshold and nms over lap threshold for each class.
bool PointPillarsDetection::Init(const DetectionInitOptions& options) {
  point_pillars_ptr_.reset(new PointPillars(
      FLAGS_reproduce_result_mode, FLAGS_score_threshold,
      FLAGS_nms_overlap_threshold, FLAGS_pfe_onnx_file, FLAGS_rpn_onnx_file));
  return true;
}

bool PointPillarsDetection::Detect(const DetectionOptions& options,
                                   LidarFrame* frame) {
  // check input
  if (frame == nullptr) {
    AERROR << "Input null frame ptr.";
    return false;
  }
  if (frame->cloud == nullptr) {
    AERROR << "Input null frame cloud.";
    return false;
  }
  if (frame->cloud->size() == 0) {
    AERROR << "Input none points.";
    return false;
  }

  // record input cloud and lidar frame
  original_cloud_ = frame->cloud;
  original_world_cloud_ = frame->world_cloud;
  lidar_frame_ref_ = frame;

  // check output
  frame->segmented_objects.clear();

  Timer timer;

  if (cudaSetDevice(FLAGS_gpu_id) != cudaSuccess) {
    AERROR << "Failed to set device to gpu " << FLAGS_gpu_id;
    return false;
  }

  // transform point cloud into an array
  float* points_array;
  int num_points = original_cloud_->size();
  int num_point_indexes;
  std::vector<int> point_indexes;
  if (FLAGS_enable_fuse_frames && FLAGS_num_fuse_frames > 1) {
    // before fusing
    while (!prev_point_clouds_.empty() &&
           frame->timestamp - prev_point_clouds_.front().second >
               FLAGS_fuse_time_interval) {
      prev_point_clouds_.pop_front();
    }

    // fusing
    for (auto& pc_timestamp : prev_point_clouds_) {
      num_points += pc_timestamp.first->size();
    }
    num_point_indexes = num_points;
    point_indexes =
        GenerateIndexes(0, num_point_indexes, FLAGS_enable_shuffle_points);
    num_points = std::min(num_points, FLAGS_max_num_points);
    points_array = new float[num_points * FLAGS_num_point_feature]();
    FusePointCloudToArray(original_cloud_, points_array, point_indexes,
                          FLAGS_normalizing_factor);

    // after fusing
    while (static_cast<int>(prev_point_clouds_.size()) >=
           FLAGS_num_fuse_frames - 1) {
      prev_point_clouds_.pop_front();
    }
    prev_point_clouds_.emplace_back(
        std::make_pair(original_world_cloud_, frame->timestamp));
  } else {
    num_point_indexes = num_points;
    point_indexes =
        GenerateIndexes(0, num_point_indexes, FLAGS_enable_shuffle_points);
    num_points = std::min(num_points, FLAGS_max_num_points);
    points_array = new float[num_points * FLAGS_num_point_feature]();
    PclToArray(original_cloud_, points_array, point_indexes,
               FLAGS_normalizing_factor);
  }
  pcl_to_array_time_ = timer.toc(true);

  // inference
  std::vector<float> out_detections;
  std::vector<int> out_labels;
  point_pillars_ptr_->DoInference(points_array, num_points, &out_detections,
                                  &out_labels);
  inference_time_ = timer.toc(true);

  // transfer output bounding boxes to objects
  GetObjects(&frame->segmented_objects, frame->lidar2world_pose,
             &out_detections, &out_labels);
  collect_time_ = timer.toc(true);

  AINFO << "PointPillars: "
        << "pcl_to_array: " << pcl_to_array_time_ << "\t"
        << "inference: " << inference_time_ << "\t"
        << "collect: " << collect_time_;

  delete[] points_array;
  return true;
}

void PointPillarsDetection::PclToArray(const base::PointFCloudPtr& pc_ptr,
                                       float* out_points_array,
                                       const std::vector<int>& point_indexes,
                                       const float normalizing_factor) {
  for (size_t i = 0; i < pc_ptr->size(); ++i) {
    int point_pos = point_indexes.at(i);
    if (point_pos >= FLAGS_max_num_points) continue;
    const auto& point = pc_ptr->at(i);
    out_points_array[point_pos * FLAGS_num_point_feature + 0] = point.x;
    out_points_array[point_pos * FLAGS_num_point_feature + 1] = point.y;
    out_points_array[point_pos * FLAGS_num_point_feature + 2] = point.z;
    out_points_array[point_pos * FLAGS_num_point_feature + 3] =
        point.intensity / normalizing_factor;
    // delta of timestamp between prev and cur frames
    out_points_array[point_pos * FLAGS_num_point_feature + 4] = 0;
  }
}

// TODO(chenjiahao): write a cuda version to accelerate
void PointPillarsDetection::FusePointCloudToArray(
    const base::PointFCloudPtr& pc_ptr, float* out_points_array,
    const std::vector<int>& point_indexes, const float normalizing_factor) {
  PclToArray(pc_ptr, out_points_array, point_indexes, normalizing_factor);

  int point_counter = pc_ptr->size();
  for (auto iter = prev_point_clouds_.rbegin();
       iter != prev_point_clouds_.rend(); ++iter) {
    base::PointDCloudPtr& prev_pc_ptr = iter->first;
    // transform prev world point cloud to current sensor's coordinates
    for (size_t i = 0; i < prev_pc_ptr->size(); ++i) {
      int point_pos = point_indexes.at(point_counter);
      if (point_pos >= FLAGS_max_num_points) continue;
      const auto& point = prev_pc_ptr->at(i);
      Eigen::Vector3d trans_point(point.x, point.y, point.z);
      trans_point = lidar_frame_ref_->lidar2world_pose.inverse() * trans_point;
      out_points_array[point_pos * FLAGS_num_point_feature + 0] =
          static_cast<float>(trans_point(0));
      out_points_array[point_pos * FLAGS_num_point_feature + 1] =
          static_cast<float>(trans_point(1));
      out_points_array[point_pos * FLAGS_num_point_feature + 2] =
          static_cast<float>(trans_point(2));
      out_points_array[point_pos * FLAGS_num_point_feature + 3] =
          static_cast<float>(point.intensity / normalizing_factor);
      out_points_array[point_pos * FLAGS_num_point_feature + 4] =
          static_cast<float>(lidar_frame_ref_->timestamp - iter->second);
      point_counter++;
    }
  }
}

std::vector<int> PointPillarsDetection::GenerateIndexes(int start_index,
                                                        int size,
                                                        bool shuffle) {
  // create a range number array
  std::vector<int> indexes(size);
  std::iota(indexes.begin(), indexes.end(), start_index);

  // shuffle the index array
  if (shuffle) {
    unsigned seed = 0;
    std::shuffle(indexes.begin(), indexes.end(),
                 std::default_random_engine(seed));
  }
  return indexes;
}

void PointPillarsDetection::GetObjects(
    std::vector<std::shared_ptr<Object>>* objects, const Eigen::Affine3d& pose,
    std::vector<float>* detections, std::vector<int>* labels) {
  int num_objects = detections->size() / FLAGS_num_output_box_feature;

  objects->clear();
  base::ObjectPool::Instance().BatchGet(num_objects, objects);

  for (int i = 0; i < num_objects; ++i) {
    auto& object = objects->at(i);
    object->id = i;

    // read params of bounding box
    float x = detections->at(i * FLAGS_num_output_box_feature + 0);
    float y = detections->at(i * FLAGS_num_output_box_feature + 1);
    float z = detections->at(i * FLAGS_num_output_box_feature + 2);
    float dx = detections->at(i * FLAGS_num_output_box_feature + 4);
    float dy = detections->at(i * FLAGS_num_output_box_feature + 3);
    float dz = detections->at(i * FLAGS_num_output_box_feature + 5);
    float yaw = detections->at(i * FLAGS_num_output_box_feature + 6);
    yaw += M_PI / 2;
    yaw = std::atan2(sinf(yaw), cosf(yaw));
    yaw = -yaw;

    // directions
    object->theta = yaw;
    object->direction[0] = cosf(yaw);
    object->direction[1] = sinf(yaw);
    object->direction[2] = 0;
    object->lidar_supplement.is_orientation_ready = true;

    // compute vertexes of bounding box and transform to world coordinate
    object->lidar_supplement.num_points_in_roi = 8;
    object->lidar_supplement.on_use = true;
    object->lidar_supplement.is_background = false;
    float roll = 0, pitch = 0;
    Eigen::Quaternionf quater =
        Eigen::AngleAxisf(roll, Eigen::Vector3f::UnitX()) *
        Eigen::AngleAxisf(pitch, Eigen::Vector3f::UnitY()) *
        Eigen::AngleAxisf(yaw, Eigen::Vector3f::UnitZ());
    Eigen::Translation3f translation(x, y, z);
    Eigen::Affine3f affine3f = translation * quater.toRotationMatrix();
    for (float vx : std::vector<float>{dx / 2, -dx / 2}) {
      for (float vy : std::vector<float>{dy / 2, -dy / 2}) {
        for (float vz : std::vector<float>{0, dz}) {
          Eigen::Vector3f v3f(vx, vy, vz);
          v3f = affine3f * v3f;
          PointF point;
          point.x = v3f.x();
          point.y = v3f.y();
          point.z = v3f.z();
          object->lidar_supplement.cloud.push_back(point);

          Eigen::Vector3d trans_point(point.x, point.y, point.z);
          trans_point = pose * trans_point;
          PointD world_point;
          world_point.x = trans_point(0);
          world_point.y = trans_point(1);
          world_point.z = trans_point(2);
          object->lidar_supplement.cloud_world.push_back(world_point);
        }
      }
    }

    // classification
    object->lidar_supplement.raw_probs.push_back(std::vector<float>(
        static_cast<int>(base::ObjectType::MAX_OBJECT_TYPE), 0.f));
    object->lidar_supplement.raw_classification_methods.push_back(Name());
    object->sub_type = GetObjectSubType(labels->at(i));
    object->type = base::kSubType2TypeMap.at(object->sub_type);
    object->lidar_supplement.raw_probs.back()[static_cast<int>(object->type)] =
        1.0f;
    // copy to type
    object->type_probs.assign(object->lidar_supplement.raw_probs.back().begin(),
                              object->lidar_supplement.raw_probs.back().end());
  }
}

// TODO(chenjiahao): update the base ObjectSubType with more fine-grained types
base::ObjectSubType PointPillarsDetection::GetObjectSubType(const int label) {
  switch (label) {
    case 0:
      return base::ObjectSubType::BUS;
    case 1:
      return base::ObjectSubType::CAR;
    case 2:  // construction vehicle
      return base::ObjectSubType::UNKNOWN_MOVABLE;
    case 3:  // trailer
      return base::ObjectSubType::UNKNOWN_MOVABLE;
    case 4:
      return base::ObjectSubType::TRUCK;
    case 5:  // barrier
      return base::ObjectSubType::UNKNOWN_UNMOVABLE;
    case 6:
      return base::ObjectSubType::CYCLIST;
    case 7:
      return base::ObjectSubType::MOTORCYCLIST;
    case 8:
      return base::ObjectSubType::PEDESTRIAN;
    case 9:
      return base::ObjectSubType::TRAFFICCONE;
    default:
      return base::ObjectSubType::UNKNOWN;
  }
}

}  // namespace lidar
}  // namespace perception
}  // namespace apollo
