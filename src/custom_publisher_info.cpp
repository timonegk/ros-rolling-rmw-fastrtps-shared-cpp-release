// Copyright 2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rmw_fastrtps_shared_cpp/custom_publisher_info.hpp"

#include "fastdds/dds/core/status/BaseStatus.hpp"
#include "fastdds/dds/core/status/DeadlineMissedStatus.hpp"

#include "event_helpers.hpp"
#include "types/event_types.hpp"

EventListenerInterface *
CustomPublisherInfo::getListener() const
{
  return listener_;
}

void
PubListener::on_offered_deadline_missed(
  eprosima::fastdds::dds::DataWriter * /* writer */,
  const eprosima::fastdds::dds::OfferedDeadlineMissedStatus & status)
{
  std::lock_guard<std::mutex> lock(internalMutex_);

  // the change to liveliness_lost_count_ needs to be mutually exclusive with
  // rmw_wait() which checks hasEvent() and decides if wait() needs to be called
  ConditionalScopedLock clock(conditionMutex_, conditionVariable_);

  // Assign absolute values
  offered_deadline_missed_status_.total_count = status.total_count;
  // Accumulate deltas
  offered_deadline_missed_status_.total_count_change += status.total_count_change;

  deadline_changes_.store(true, std::memory_order_relaxed);

  std::unique_lock<std::mutex> lock_mutex(on_new_event_m_);

  if (on_new_event_cb_) {
    on_new_event_cb_(user_data_, 1);
  } else {
    unread_events_count_++;
  }
}

void PubListener::on_liveliness_lost(
  eprosima::fastdds::dds::DataWriter * /* writer */,
  const eprosima::fastdds::dds::LivelinessLostStatus & status)
{
  std::lock_guard<std::mutex> lock(internalMutex_);

  // the change to liveliness_lost_count_ needs to be mutually exclusive with
  // rmw_wait() which checks hasEvent() and decides if wait() needs to be called
  ConditionalScopedLock clock(conditionMutex_, conditionVariable_);

  // Assign absolute values
  liveliness_lost_status_.total_count = status.total_count;
  // Accumulate deltas
  liveliness_lost_status_.total_count_change += status.total_count_change;

  liveliness_changes_.store(true, std::memory_order_relaxed);

  std::unique_lock<std::mutex> lock_mutex(on_new_event_m_);

  if (on_new_event_cb_) {
    on_new_event_cb_(user_data_, 1);
  } else {
    unread_events_count_++;
  }
}

void PubListener::on_offered_incompatible_qos(
  eprosima::fastdds::dds::DataWriter * /* writer */,
  const eprosima::fastdds::dds::OfferedIncompatibleQosStatus & status)
{
  std::lock_guard<std::mutex> lock(internalMutex_);

  // the change to incompatible_qos_status_ needs to be mutually exclusive with
  // rmw_wait() which checks hasEvent() and decides if wait() needs to be called
  ConditionalScopedLock clock(conditionMutex_, conditionVariable_);

  // Assign absolute values
  incompatible_qos_status_.last_policy_id = status.last_policy_id;
  incompatible_qos_status_.total_count = status.total_count;
  // Accumulate deltas
  incompatible_qos_status_.total_count_change += status.total_count_change;

  incompatible_qos_changes_.store(true, std::memory_order_relaxed);
}

bool PubListener::hasEvent(rmw_event_type_t event_type) const
{
  assert(rmw_fastrtps_shared_cpp::internal::is_event_supported(event_type));
  switch (event_type) {
    case RMW_EVENT_LIVELINESS_LOST:
      return liveliness_changes_.load(std::memory_order_relaxed);
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
      return deadline_changes_.load(std::memory_order_relaxed);
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE:
      return incompatible_qos_changes_.load(std::memory_order_relaxed);
    default:
      break;
  }
  return false;
}

void PubListener::set_on_new_event_callback(
  const void * user_data,
  rmw_event_callback_t callback)
{
  std::unique_lock<std::mutex> lock_mutex(on_new_event_m_);

  if (callback) {
    // Push events arrived before setting the executor's callback
    if (unread_events_count_) {
      callback(user_data, unread_events_count_);
      unread_events_count_ = 0;
    }
    user_data_ = user_data;
    on_new_event_cb_ = callback;
  } else {
    user_data_ = nullptr;
    on_new_event_cb_ = nullptr;
  }
}

bool PubListener::takeNextEvent(rmw_event_type_t event_type, void * event_info)
{
  assert(rmw_fastrtps_shared_cpp::internal::is_event_supported(event_type));
  std::lock_guard<std::mutex> lock(internalMutex_);
  switch (event_type) {
    case RMW_EVENT_LIVELINESS_LOST:
      {
        auto rmw_data = static_cast<rmw_liveliness_lost_status_t *>(event_info);
        rmw_data->total_count = liveliness_lost_status_.total_count;
        rmw_data->total_count_change = liveliness_lost_status_.total_count_change;
        liveliness_lost_status_.total_count_change = 0;
        liveliness_changes_.store(false, std::memory_order_relaxed);
      }
      break;
    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
      {
        auto rmw_data = static_cast<rmw_offered_deadline_missed_status_t *>(event_info);
        rmw_data->total_count = offered_deadline_missed_status_.total_count;
        rmw_data->total_count_change = offered_deadline_missed_status_.total_count_change;
        offered_deadline_missed_status_.total_count_change = 0;
        deadline_changes_.store(false, std::memory_order_relaxed);
      }
      break;
    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE:
      {
        auto rmw_data = static_cast<rmw_requested_qos_incompatible_event_status_t *>(event_info);
        rmw_data->total_count = incompatible_qos_status_.total_count;
        rmw_data->total_count_change = incompatible_qos_status_.total_count_change;
        rmw_data->last_policy_kind =
          rmw_fastrtps_shared_cpp::internal::dds_qos_policy_to_rmw_qos_policy(
          incompatible_qos_status_.last_policy_id);
        incompatible_qos_status_.total_count_change = 0;
        incompatible_qos_changes_.store(false, std::memory_order_relaxed);
      }
      break;
    default:
      return false;
  }
  return true;
}
