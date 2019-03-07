// Copyright 2016 The TensorFlow Authors. All Rights Reserved.
// Modifications copyright (C) 2019 Uber Technologies, Inc.
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
// =============================================================================

#include "collective_operations.h"

namespace horovod {
namespace common {

HorovodOp::HorovodOp(HorovodGlobalState* global_state) : global_state_(global_state) {}

// Allreduce
AllreduceOp::AllreduceOp(HorovodGlobalState* global_state) : HorovodOp(global_state) {}

int64_t AllreduceOp::NumElements(std::vector<TensorTableEntry>& entries) {
  int64_t num_elements = 0;
  for (auto& e : entries) {
    num_elements += e.tensor->shape().num_elements();
  }
  return num_elements;
}

void AllreduceOp::MemcpyInFusionBuffer(std::vector<TensorTableEntry>& entries, const void*& fused_input_data,
                                       void*& buffer_data, size_t& buffer_len) {
  // Access the fusion buffer.
  auto& first_entry = entries[0];
  auto& buffer = global_state_->fusion_buffer.GetBuffer(
      first_entry.device, first_entry.context->framework());
  buffer_data = const_cast<void*>(buffer->AccessData(first_entry.context));

  int64_t offset = 0;
  for (auto& e : entries) {
    void* buffer_data_at_offset = (uint8_t*) buffer_data + offset;
    MemcpyEntryInFusionBuffer(buffer_data_at_offset, e, entries);
    offset += e.tensor->size();
  }

  buffer_len = (size_t) offset;

  // Set the input data to originate from the buffer.
  fused_input_data = buffer_data;
}

void AllreduceOp::MemcpyOutFusionBuffer(std::vector<TensorTableEntry>& entries, void* buffer_data) {
  int64_t offset = 0;
  for (auto& e : entries) {
    void* buffer_data_at_offset = (uint8_t*) buffer_data + offset;
    MemcpyEntryOutFusionBuffer(buffer_data_at_offset, e, entries);
    offset += e.tensor->size();
  }
}

void AllreduceOp::MemcpyEntryInFusionBuffer(void* buffer_data_at_offset, TensorTableEntry& e,
                                            std::vector<TensorTableEntry>& entries) {
  std::memcpy(buffer_data_at_offset, e.tensor->data(),
              (size_t) e.tensor->size());
}

void AllreduceOp::MemcpyEntryOutFusionBuffer(void* buffer_data_at_offset, TensorTableEntry& e,
                                             std::vector<TensorTableEntry>& entries) {
  std::memcpy((void*) e.output->data(), buffer_data_at_offset,
              (size_t) e.tensor->size());
}

// Allgather
AllgatherOp::AllgatherOp(HorovodGlobalState* global_state) : HorovodOp(global_state) {}

Status AllgatherOp::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  auto& timeline = global_state_->timeline;

  // Sizes of subcomponents of each entry from all ranks
  auto** entry_component_sizes = new int64_t* [entries.size()];

  // Offset of each subcomponent of every entry in the final buffer after
  // allgatherv
  auto** entry_component_offsets = new int64_t* [entries.size()];

  auto* recvcounts = new int[global_state_->size]();
  auto* displcmnts = new int[global_state_->size]();

  for (size_t ec = 0; ec < entries.size(); ++ec) {
    entry_component_sizes[ec] = new int64_t[global_state_->size]();
    entry_component_offsets[ec] = new int64_t[global_state_->size]();
  }

  auto& first_entry = entries[0];

  timeline.ActivityStartAll(entries, ALLOCATE_OUTPUT);
  for (size_t ec = 0; ec < entries.size(); ++ec) {
    auto& e = entries[ec];
    // Every tensor participating in Allgather operation may have different
    // first dimension size, but the rest of dimensions are same for all
    // tensors.  Here we get shape of tensor sliced by first dimension.
    TensorShape single_slice_shape;
    for (int i = 1; i < e.tensor->shape().dims(); ++i) {
      single_slice_shape.AddDim(e.tensor->shape().dim_size(i));
    }

    // Copy tensor sizes from the MPI response into a vector of int64_t
    // and compute total size.  This is size of first dimension.
    int64_t total_entry_dimension_size = 0;
    const auto& tensor_sizes = response.tensor_sizes();
    for (int rc = 0; rc < global_state_->size; ++rc) {
      auto component_size = tensor_sizes[ec * global_state_->size + rc];
      total_entry_dimension_size += component_size;
      recvcounts[rc] += component_size * single_slice_shape.num_elements();
      entry_component_sizes[ec][rc] =
          component_size * single_slice_shape.num_elements();
    }

    // Allgather output will have shape of:
    // (sum of first dimension of every tensor) x (tensor slice shape).
    TensorShape output_shape;
    output_shape.AddDim((int64_t) total_entry_dimension_size);
    output_shape.AppendShape(single_slice_shape);

    Status status = e.context->AllocateOutput(output_shape, &e.output);
    if (!status.ok()) {
      return status;
    }
  }
  timeline.ActivityEndAll(entries);

  for (int rc = 0; rc < global_state_->size; ++rc) {
    if (rc == 0) {
      displcmnts[rc] = 0;
    } else {
      displcmnts[rc] = displcmnts[rc - 1] + recvcounts[rc - 1];
    }
  }

  unsigned int rank_displacement = 0;
  for (int rc = 0; rc < global_state_->size; ++rc) {
    for (size_t ec = 0; ec < entries.size(); ++ec) {
      if (ec == 0) {
        entry_component_offsets[ec][rc] = rank_displacement;
      } else {
        entry_component_offsets[ec][rc] =
            entry_component_offsets[ec - 1][rc] +
            entry_component_sizes[ec - 1][rc];
      }
    }
    rank_displacement += recvcounts[rc];
  }

  int element_size = GetElementSize(first_entry.tensor->dtype());
  int64_t total_size = displcmnts[global_state_->size - 1] +
                       recvcounts[global_state_->size - 1];

  DoAllgather(entries, recvcounts, displcmnts,
              entry_component_offsets, entry_component_sizes,
              total_size, element_size);

  return Status::OK();
}

BroadcastOp::BroadcastOp(HorovodGlobalState* global_state) : HorovodOp(global_state) {}

Status BroadcastOp::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  assert(entries.size() == 1);
  auto e = entries[0];

  // On root rank, MPI_Bcast sends data, on other ranks it receives data.
  void* data_ptr;
  if (global_state_->rank == e.root_rank) {
    data_ptr = (void*) e.tensor->data();
  } else {
    data_ptr = (void*) e.output->data();
  }

  DoBroadcast(entries, data_ptr, (int) e.tensor->shape().num_elements(), e.tensor->dtype(), e.root_rank);

  return Status::OK();
}

bool BroadcastOp::Enabled(ParameterManager& param_manager,
                          std::vector<TensorTableEntry>& entries,
                          const Response& response) const {
  return true;
}

ErrorOp::ErrorOp(HorovodGlobalState* global_state) : HorovodOp(global_state) {}

Status ErrorOp::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  assert(entries.size() == 1);
  auto e = entries[0];
  return Status::PreconditionError(response.error_message());
}

} // namespace common
} // namespace horovod
