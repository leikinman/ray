// Copyright 2022 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once


#include <memory>
#include <type_traits>
#include "ray/gcs/gcs_server/grpc_based_resource_broadcaster.h"
#include "ray/gcs/gcs_server/gcs_resource_report_poller.h"
#include "ray/common/asio/periodical_runner.h"

namespace ray {
namespace sync {

class RaySync {
 public:
  RaySync(instrumented_io_context& main_thread,
          std::unique_ptr<::ray::gcs::GrpcBasedResourceBroadcaster> braodcaster,
          std::unique_ptr<::ray::gcs::GcsResourceReportPoller> poller)
      : 
      ticker_(main_thread),
      broadcaster_(std::move(braodcaster)), 
      poller_(std::move(poller)) {}

  void Start() {
    poller_->Start();
    broadcast_thread_ = std::make_unique<std::thread>([this]() {
      SetThreadName("resource_report_broadcaster");
      boost::asio::io_service::work work(broadcast_service_);
      broadcast_service_.run();
    });
    ticker_.RunFnPeriodically(
      [this] { 
        auto beg = resources_buffer_.begin();
        auto ptr = beg;
        static auto max_batch = RayConfig::instance().resource_broadcast_batch_size();
        for (size_t cnt = resources_buffer_proto_.batch().size();
            cnt <  max_batch && cnt < resources_buffer_.size();
            ++ptr, ++cnt) {
          resources_buffer_proto_.add_batch()->mutable_data()->Swap(&ptr->second);
        }
        resources_buffer_.erase(beg, ptr);        
        broadcaster_->SendBroadcast(std::move(resources_buffer_proto_)); 
        resources_buffer_proto_.Clear();
      }, 
      RayConfig::instance().raylet_report_resources_period_milliseconds(),
      "RaySyncer.deadline_timer.report_resource_report");
  }

  void Stop() {
    poller_->Stop();
    if (broadcast_thread_ != nullptr) {
      broadcast_service_.stop();
      if (broadcast_thread_->joinable()) {
        broadcast_thread_->join();
      }
    }
  }

  // External API
  template <typename T>
  void Update(T update) {
    static_assert(std::is_same_v<T, rpc::NodeResourceChange> ||
                  std::is_same_v<T, rpc::ResourcesData>, "unknown type");

    if constexpr (std::is_same_v<T, rpc::NodeResourceChange>) {
      resources_buffer_proto_.add_batch()->mutable_change()->Swap(&update);
    } else if constexpr (std::is_same_v<T, rpc::ResourcesData>) {
      if (update.should_global_gc() ||
          update.resources_total_size() > 0 ||
          update.resources_available_changed() ||
          update.resource_load_changed()) {
        update.clear_resource_load();
        update.clear_resource_load_by_shape();
        update.clear_resources_normal_task();
        auto& orig = resources_buffer_[update.node_id()];
        orig.Swap(&update);
      }
    } 
  }

  void AddNode(const rpc::GcsNodeInfo &node_info) {
    broadcaster_->HandleNodeAdded(node_info);
    poller_->HandleNodeAdded(node_info);
  }

  void RemoveNode(const rpc::GcsNodeInfo &node_info) {
    broadcaster_->HandleNodeRemoved(node_info);
    poller_->HandleNodeRemoved(node_info);
    resources_buffer_.erase(node_info.node_id());
  }

  std::string DebugString() { return broadcaster_->DebugString(); }

 private:
  PeriodicalRunner ticker_;
  rpc::ResourceUsageBroadcastData resources_buffer_proto_;
  std::unique_ptr<::ray::gcs::GrpcBasedResourceBroadcaster> broadcaster_;
  std::unique_ptr<::ray::gcs::GcsResourceReportPoller> poller_;
  absl::flat_hash_map<std::string, rpc::ResourcesData> resources_buffer_;
  std::unique_ptr<std::thread> broadcast_thread_;
  instrumented_io_context broadcast_service_;
};

}  // namespace sync
}  // namespace ray
