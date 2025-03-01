// Copyright (c) 2008, Willow Garage, Inc.
// All rights reserved.
//
// Software License Agreement (BSD License 2.0)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//  * Neither the name of the Willow Garage nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "tracetools_image_pipeline/tracetools.h"
#include "cv_bridge/cv_bridge.h"

#include <depth_image_proc/conversions.hpp>
#include <depth_image_proc/point_cloud_xyzrgb.hpp>
#include <image_transport/image_transport.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>

namespace depth_image_proc
{


PointCloudXyzrgbNode::PointCloudXyzrgbNode(const rclcpp::NodeOptions & options)
: rclcpp::Node("PointCloudXyzrgbNode", options)
{
  // Read parameters
  int queue_size = this->declare_parameter<int>("queue_size", 5);
  bool use_exact_sync = this->declare_parameter<bool>("exact_sync", false);

  // Synchronize inputs. Topic subscriptions happen on demand in the connection callback.
  if (use_exact_sync) {
    exact_sync_ = std::make_shared<ExactSynchronizer>(
      ExactSyncPolicy(queue_size),
      sub_depth_,
      sub_rgb_,
      sub_info_);
    exact_sync_->registerCallback(
      std::bind(
        &PointCloudXyzrgbNode::imageCb,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3));
  } else {
    sync_ = std::make_shared<Synchronizer>(SyncPolicy(queue_size), sub_depth_, sub_rgb_, sub_info_);
    sync_->registerCallback(
      std::bind(
        &PointCloudXyzrgbNode::imageCb,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3));
  }

  // Monitor whether anyone is subscribed to the output
  // TODO(ros2) Implement when SubscriberStatusCallback is available
  // ros::SubscriberStatusCallback connect_cb = boost::bind(&PointCloudXyzrgbNode::connectCb, this);
  connectCb();
  // TODO(ros2) Implement when SubscriberStatusCallback is available
  // Make sure we don't enter connectCb() between advertising and assigning to pub_point_cloud_
  std::lock_guard<std::mutex> lock(connect_mutex_);
  // TODO(ros2) Implement connect_cb when SubscriberStatusCallback is available
  // pub_point_cloud_ = depth_nh.advertise<PointCloud>("points", 1, connect_cb, connect_cb);
  pub_point_cloud_ = create_publisher<PointCloud2>("points", rclcpp::SensorDataQoS());
  // TODO(ros2) Implement connect_cb when SubscriberStatusCallback is available
}

size_t PointCloudXyzrgbNode::get_msg_size(sensor_msgs::msg::Image::ConstSharedPtr image_msg){
  //Serialize the Image and CameraInfo messages
  rclcpp::SerializedMessage serialized_data_img;
  rclcpp::Serialization<sensor_msgs::msg::Image> image_serialization;
  const void* image_ptr = reinterpret_cast<const void*>(image_msg.get());
  image_serialization.serialize_message(image_ptr, &serialized_data_img);
  size_t image_msg_size = serialized_data_img.size();
  return image_msg_size;
}

size_t PointCloudXyzrgbNode::get_msg_size(sensor_msgs::msg::CameraInfo::ConstSharedPtr info_msg){
  rclcpp::SerializedMessage serialized_data_info;
  rclcpp::Serialization<sensor_msgs::msg::CameraInfo> info_serialization;
  const void* info_ptr = reinterpret_cast<const void*>(info_msg.get());
  info_serialization.serialize_message(info_ptr, &serialized_data_info);
  size_t info_msg_size = serialized_data_info.size();
  return info_msg_size;
}

// Handles (un)subscribing when clients (un)subscribe
void PointCloudXyzrgbNode::connectCb()
{
  std::lock_guard<std::mutex> lock(connect_mutex_);
  // TODO(ros2) Implement getNumSubscribers when rcl/rmw support it
  // if (pub_point_cloud_->getNumSubscribers() == 0)
  if (0) {
    // TODO(ros2) Implement getNumSubscribers when rcl/rmw support it
    sub_depth_.unsubscribe();
    sub_rgb_.unsubscribe();
    sub_info_.unsubscribe();
  } else if (!sub_depth_.getSubscriber()) {
    // parameter for depth_image_transport hint
    std::string depth_image_transport_param = "depth_image_transport";
    image_transport::TransportHints depth_hints(this, "raw", depth_image_transport_param);

    // depth image can use different transport.(e.g. compressedDepth)
    sub_depth_.subscribe(this, "depth_registered/image_rect", depth_hints.getTransport());

    // rgb uses normal ros transport hints.
    image_transport::TransportHints hints(this, "raw");
    sub_rgb_.subscribe(this, "rgb/image_rect_color", hints.getTransport());
    sub_info_.subscribe(this, "rgb/camera_info");
  }
}

void PointCloudXyzrgbNode::imageCb(
  const Image::ConstSharedPtr & depth_msg,
  const Image::ConstSharedPtr & rgb_msg_in,
  const CameraInfo::ConstSharedPtr & info_msg)
{

  TRACEPOINT(
    depth_image_proc_transform_to_pointcloud_cb_init,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*depth_msg)),
    static_cast<const void *>(&(*rgb_msg_in)),
    static_cast<const void *>(&(*info_msg)),
    depth_msg->header.stamp.nanosec,
    depth_msg->header.stamp.sec,
    get_msg_size(depth_msg),
    get_msg_size(rgb_msg_in),
    get_msg_size(info_msg));

  // Check for bad inputs
  if (depth_msg->header.frame_id != rgb_msg_in->header.frame_id) {
    RCLCPP_WARN_THROTTLE(
      get_logger(),
      *get_clock(),
      10000,  // 10 seconds
      "Depth image frame id [%s] doesn't match RGB image frame id [%s]",
      depth_msg->header.frame_id.c_str(), rgb_msg_in->header.frame_id.c_str());
  }

  // Update camera model
  model_.fromCameraInfo(info_msg);

  // Check if the input image has to be resized
  Image::ConstSharedPtr rgb_msg = rgb_msg_in;
  if (depth_msg->width != rgb_msg->width || depth_msg->height != rgb_msg->height) {
    CameraInfo info_msg_tmp = *info_msg;
    info_msg_tmp.width = depth_msg->width;
    info_msg_tmp.height = depth_msg->height;
    float ratio = static_cast<float>(depth_msg->width) / static_cast<float>(rgb_msg->width);
    info_msg_tmp.k[0] *= ratio;
    info_msg_tmp.k[2] *= ratio;
    info_msg_tmp.k[4] *= ratio;
    info_msg_tmp.k[5] *= ratio;
    info_msg_tmp.p[0] *= ratio;
    info_msg_tmp.p[2] *= ratio;
    info_msg_tmp.p[5] *= ratio;
    info_msg_tmp.p[6] *= ratio;
    model_.fromCameraInfo(info_msg_tmp);

    cv_bridge::CvImageConstPtr cv_ptr;
    try {
      cv_ptr = cv_bridge::toCvShare(rgb_msg, rgb_msg->encoding);
    } catch (cv_bridge::Exception & e) {
      RCLCPP_ERROR(get_logger(), "cv_bridge exception: %s", e.what());

      TRACEPOINT(
        depth_image_proc_transform_to_pointcloud_cb_fini,
        static_cast<const void *>(this),
        static_cast<const void *>(&(*depth_msg)),
        static_cast<const void *>(&(*rgb_msg_in)),
        static_cast<const void *>(&(*info_msg)),
        depth_msg->header.stamp.nanosec,
        depth_msg->header.stamp.sec,
        get_msg_size(depth_msg),
        get_msg_size(rgb_msg_in),
        get_msg_size(info_msg));

      return;
    }
    cv_bridge::CvImage cv_rsz;
    cv_rsz.header = cv_ptr->header;
    cv_rsz.encoding = cv_ptr->encoding;
    cv::resize(
      cv_ptr->image.rowRange(0, depth_msg->height / ratio), cv_rsz.image,
      cv::Size(depth_msg->width, depth_msg->height));
    if ((rgb_msg->encoding == sensor_msgs::image_encodings::RGB8) ||
      (rgb_msg->encoding == sensor_msgs::image_encodings::BGR8) ||
      (rgb_msg->encoding == sensor_msgs::image_encodings::MONO8))
    {
      rgb_msg = cv_rsz.toImageMsg();
    } else {
      rgb_msg =
        cv_bridge::toCvCopy(cv_rsz.toImageMsg(), sensor_msgs::image_encodings::RGB8)->toImageMsg();
    }

    RCLCPP_ERROR(
      get_logger(), "Depth resolution (%ux%u) does not match RGB resolution (%ux%u)",
      depth_msg->width, depth_msg->height, rgb_msg->width, rgb_msg->height);

    TRACEPOINT(
        depth_image_proc_transform_to_pointcloud_cb_fini,
        static_cast<const void *>(this),
        static_cast<const void *>(&(*depth_msg)),
        static_cast<const void *>(&(*rgb_msg_in)),
        static_cast<const void *>(&(*info_msg)),
        depth_msg->header.stamp.nanosec,
        depth_msg->header.stamp.sec,
        get_msg_size(depth_msg),
        get_msg_size(rgb_msg_in),
        get_msg_size(info_msg));
  
    return;
  } else {
    rgb_msg = rgb_msg_in;
  }

  // Supported color encodings: RGB8, BGR8, MONO8
  int red_offset, green_offset, blue_offset, color_step;
  if (rgb_msg->encoding == sensor_msgs::image_encodings::RGB8) {
    red_offset = 0;
    green_offset = 1;
    blue_offset = 2;
    color_step = 3;
  } else if (rgb_msg->encoding == sensor_msgs::image_encodings::BGR8) {
    red_offset = 2;
    green_offset = 1;
    blue_offset = 0;
    color_step = 3;
  } else if (rgb_msg->encoding == sensor_msgs::image_encodings::MONO8) {
    red_offset = 0;
    green_offset = 0;
    blue_offset = 0;
    color_step = 1;
  } else {
    try {
      rgb_msg = cv_bridge::toCvCopy(rgb_msg, sensor_msgs::image_encodings::RGB8)->toImageMsg();
    } catch (cv_bridge::Exception & e) {
      RCLCPP_ERROR(
        get_logger(), "Unsupported encoding [%s]: %s", rgb_msg->encoding.c_str(), e.what());

      TRACEPOINT(
        depth_image_proc_transform_to_pointcloud_cb_fini,
        static_cast<const void *>(this),
        static_cast<const void *>(&(*depth_msg)),
        static_cast<const void *>(&(*rgb_msg_in)),
        static_cast<const void *>(&(*info_msg)),
        depth_msg->header.stamp.nanosec,
        depth_msg->header.stamp.sec,
        get_msg_size(depth_msg),
        get_msg_size(rgb_msg_in),
        get_msg_size(info_msg));

      return;
    }
    red_offset = 0;
    green_offset = 1;
    blue_offset = 2;
    color_step = 3;
  }

  TRACEPOINT(
    depth_image_proc_transform_to_pointcloud_init,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*depth_msg)),
    static_cast<const void *>(&(*rgb_msg_in)),
    static_cast<const void *>(&(*info_msg)),
    depth_msg->header.stamp.nanosec,
    depth_msg->header.stamp.sec);

  auto cloud_msg = std::make_shared<PointCloud2>();
  cloud_msg->header = depth_msg->header;  // Use depth image time stamp
  cloud_msg->height = depth_msg->height;
  cloud_msg->width = depth_msg->width;
  cloud_msg->is_dense = false;
  cloud_msg->is_bigendian = false;

  sensor_msgs::PointCloud2Modifier pcd_modifier(*cloud_msg);
  pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");

  // Convert Depth Image to Pointcloud
  if (depth_msg->encoding == sensor_msgs::image_encodings::TYPE_16UC1) {
    convertDepth<uint16_t>(depth_msg, cloud_msg, model_);
  } else if (depth_msg->encoding == sensor_msgs::image_encodings::TYPE_32FC1) {
    convertDepth<float>(depth_msg, cloud_msg, model_);
  } else {
    RCLCPP_ERROR(
      get_logger(), "Depth image has unsupported encoding [%s]", depth_msg->encoding.c_str());

    TRACEPOINT(
      depth_image_proc_transform_to_pointcloud_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*depth_msg)),
      static_cast<const void *>(&(*rgb_msg_in)),
      static_cast<const void *>(&(*info_msg)),
      depth_msg->header.stamp.nanosec,
      depth_msg->header.stamp.sec);

    TRACEPOINT(
        depth_image_proc_transform_to_pointcloud_cb_fini,
        static_cast<const void *>(this),
        static_cast<const void *>(&(*depth_msg)),
        static_cast<const void *>(&(*rgb_msg_in)),
        static_cast<const void *>(&(*info_msg)),
        depth_msg->header.stamp.nanosec,
        depth_msg->header.stamp.sec,
        get_msg_size(depth_msg),
        get_msg_size(rgb_msg_in),
        get_msg_size(info_msg));

    return;
  }

  // Convert RGB
  if (rgb_msg->encoding == sensor_msgs::image_encodings::RGB8) {
    convertRgb(rgb_msg, cloud_msg, red_offset, green_offset, blue_offset, color_step);
  } else if (rgb_msg->encoding == sensor_msgs::image_encodings::BGR8) {
    convertRgb(rgb_msg, cloud_msg, red_offset, green_offset, blue_offset, color_step);
  } else if (rgb_msg->encoding == sensor_msgs::image_encodings::MONO8) {
    convertRgb(rgb_msg, cloud_msg, red_offset, green_offset, blue_offset, color_step);
  } else {
    RCLCPP_ERROR(
      get_logger(), "RGB image has unsupported encoding [%s]", rgb_msg->encoding.c_str());

    TRACEPOINT(
      depth_image_proc_transform_to_pointcloud_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*depth_msg)),
      static_cast<const void *>(&(*rgb_msg_in)),
      static_cast<const void *>(&(*info_msg)),
      depth_msg->header.stamp.nanosec,
      depth_msg->header.stamp.sec);

    TRACEPOINT(
      depth_image_proc_transform_to_pointcloud_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*depth_msg)),
      static_cast<const void *>(&(*rgb_msg_in)),
      static_cast<const void *>(&(*info_msg)),
      depth_msg->header.stamp.nanosec,
      depth_msg->header.stamp.sec,
      get_msg_size(depth_msg),
      get_msg_size(rgb_msg_in),
      get_msg_size(info_msg));

    return;
  }

  TRACEPOINT(
    depth_image_proc_transform_to_pointcloud_fini,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*depth_msg)),
    static_cast<const void *>(&(*rgb_msg_in)),
    static_cast<const void *>(&(*info_msg)),
    depth_msg->header.stamp.nanosec,
    depth_msg->header.stamp.sec);

  pub_point_cloud_->publish(*cloud_msg);

  TRACEPOINT(
    depth_image_proc_transform_to_pointcloud_cb_fini,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*depth_msg)),
    static_cast<const void *>(&(*rgb_msg_in)),
    static_cast<const void *>(&(*info_msg)),
    depth_msg->header.stamp.nanosec,
    depth_msg->header.stamp.sec,
    get_msg_size(depth_msg),
    get_msg_size(rgb_msg_in),
    get_msg_size(info_msg));
}

}  // namespace depth_image_proc

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
RCLCPP_COMPONENTS_REGISTER_NODE(depth_image_proc::PointCloudXyzrgbNode)
