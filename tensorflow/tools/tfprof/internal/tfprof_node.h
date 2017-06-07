/* Copyright 2016 The TensorFlow Authors All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef THIRD_PARTY_TENSORFLOW_TOOLS_TFPROF_INTERNAL_TFPROF_NODE_H_
#define THIRD_PARTY_TENSORFLOW_TOOLS_TFPROF_INTERNAL_TFPROF_NODE_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "tensorflow/core/framework/allocation_description.pb.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/step_stats.pb.h"
#include "tensorflow/core/framework/tensor_description.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/regexp.h"
#include "tensorflow/tools/tfprof/internal/tfprof_options.h"
#include "tensorflow/tools/tfprof/tfprof_log.pb.h"

namespace tensorflow {
namespace tfprof {

class ExecStep {
 public:
  ExecStep()
      : all_start_micros_(0),
        latest_end_rel_micros_(0),
        mem_initiated_(false),
        requested_bytes_(0),
        host_temp_bytes_(0),
        host_persistent_bytes_(0),
        accelerator_temp_bytes_(0),
        accelerator_persistent_bytes_(0),
        allocator_bytes_in_use_(0) {}

  void AddTimeStats(const string& dev, const NodeExecStats& step_stat) {
    devices_.insert(dev);
    if (step_stat.all_start_micros() > 0) {
      if (all_start_micros_ > 0) {
        all_start_micros_ =
            std::min(all_start_micros_,
                     static_cast<int64>(step_stat.all_start_micros()));
      } else {
        all_start_micros_ = step_stat.all_start_micros();
      }
      int64 op_end_rel_micros = step_stat.op_end_rel_micros();
      // Round quick execution to 1 micro to be semantically robust.
      if (op_end_rel_micros == 0) {
        ++op_end_rel_micros;
      }
      latest_end_rel_micros_ =
          std::max(latest_end_rel_micros_, op_end_rel_micros);

      op_execs_[dev].push_back(
          std::make_pair(step_stat.all_start_micros(), op_end_rel_micros));

      if (dev.find("stream") != dev.npos &&
          dev.find("stream:all") == dev.npos) {
        gpu_kernel_execs_[dev].push_back(
            std::make_pair(step_stat.all_start_micros(), op_end_rel_micros));
      }
    }
  }

  void AddMemoryStats(const string& dev, const NodeExecStats& step_stat) {
    if (mem_initiated_) {
      // fprintf(stderr, "Memory initiated twice on %s", dev.c_str());
      return;
    }
    mem_initiated_ = true;

    for (const auto& mem : step_stat.memory()) {
      // TODO(xpan): Fix this hack. Currently the allocator name seems quite
      // ad-hoc.
      if (mem.allocator_name().find("GPU") == mem.allocator_name().npos) {
        continue;
      }
      allocator_bytes_in_use_ =
          std::max(allocator_bytes_in_use_,
                   static_cast<int64>(mem.allocator_bytes_in_use()));
    }
    int64 total_output_bytes = 0;
    for (const auto& output : step_stat.output()) {
      if (output.has_tensor_description() &&
          output.tensor_description().has_allocation_description()) {
        // TODO(xpan): Maybe allocated_bytes.
        int64 output_bytes = std::max(output.tensor_description()
                                          .allocation_description()
                                          .allocated_bytes(),
                                      output.tensor_description()
                                          .allocation_description()
                                          .requested_bytes());
        uint64 output_ptr =
            output.tensor_description().allocation_description().ptr();
        total_output_bytes += output_bytes;
        output_bytes_[output.slot()] = std::make_pair(output_bytes, output_ptr);
      }
    }
    if (step_stat.has_memory_stats()) {
      host_temp_bytes_ += step_stat.memory_stats().host_temp_memory_size();
      host_persistent_bytes_ +=
          step_stat.memory_stats().host_persistent_memory_size();
      accelerator_temp_bytes_ +=
          step_stat.memory_stats().device_temp_memory_size();
      accelerator_persistent_bytes_ +=
          step_stat.memory_stats().device_persistent_memory_size();
    }
    requested_bytes_ = total_output_bytes;
  }

  int64 exec_micros() const {
    int64 total = 0;
    for (const auto& execs : gpu_kernel_execs_) {
      for (const auto& exec : execs.second) {
        total += exec.second;
      }
    }
    if (total > 0) return total;

    // If there is no gpu kernel time, fall back to assume it runs on cpu.
    // TODO(xpan): No way to track CPU async op timing accurately?
    for (const auto& execs : op_execs_) {
      for (const auto& exec : execs.second) {
        total += exec.second;
      }
    }
    return total;
  }

  const std::map<string, std::vector<std::pair<int64, int64>>>& op_execs()
      const {
    return op_execs_;
  }
  int64 all_start_micros() const { return all_start_micros_; }
  int64 latest_end_rel_micros() const { return latest_end_rel_micros_; }

  int64 requested_bytes() const { return requested_bytes_; }
  int64 accelerator_temp_bytes() const { return accelerator_temp_bytes_; }
  int64 host_temp_bytes() const { return host_temp_bytes_; }
  int64 accelerator_persistent_bytes() const {
    return accelerator_persistent_bytes_;
  }
  int64 host_persistent_bytes() const { return host_persistent_bytes_; }
  const std::map<int64, std::pair<int64, uint64>>& output_bytes() const {
    return output_bytes_;
  }
  int64 allocator_bytes_in_use() const { return allocator_bytes_in_use_; }

 private:
  // The earliest/latest time including scheduling and kernel execution.
  int64 all_start_micros_;
  int64 latest_end_rel_micros_;
  // device -> vector of {op_start_micros, op_kernel_exec_micros} pairs.
  std::map<string, std::vector<std::pair<int64, int64>>> gpu_kernel_execs_;
  std::map<string, std::vector<std::pair<int64, int64>>> op_execs_;
  // All devices the op is associated with (e.g. gpu:0 (scheduling),
  // gpu:0:stream:xx (kernel exec), cpu:0 host)
  std::set<string> devices_;

  bool mem_initiated_;
  // Total output bytes requested by the op.
  int64 requested_bytes_;
  // Total temporary bytes allocated and released by the op.
  int64 host_temp_bytes_;
  // Total persistent bytes (e.g. variable) allocated by the op.
  int64 host_persistent_bytes_;
  int64 accelerator_temp_bytes_;
  int64 accelerator_persistent_bytes_;
  // The total number of bytes currently allocated by the allocator if >0.
  int64 allocator_bytes_in_use_;
  // output_idx -> {output_bytes, memory_ptr}
  std::map<int64, std::pair<int64, uint64>> output_bytes_;
};

class TFGraphNode {
 public:
  TFGraphNode(const NodeDef* node)
      : node_(node), float_ops_(0), op_(node->op()) {
    for (const auto& attr : node->attr()) {
      // TODO(xpan): Also consider _output_shapes.
      if (attr.first != "shape" || !attr.second.has_shape()) continue;
      if (!shape_.empty()) {
        fprintf(stderr, "Found duplicated shapes!\n");
        continue;
      }
      std::vector<int64> shape_vec;
      if (attr.second.shape().dim_size() == 0 &&
          !attr.second.shape().unknown_rank()) {
        // Scalar parameter with empty shape but known rank.
        shape_vec.push_back(1);
      } else {
        for (const auto& d : attr.second.shape().dim()) {
          shape_vec.push_back(d.size());
        }
      }
      update_shape(shape_vec);
    }
    op_types_.insert(node->op());
  }

  void AddInput(TFGraphNode* input, int64 output_idx) {
    inputs_[input->name()] = input;
    output_idx_[input->name()] = output_idx;
  }

  void AddOpType(const string& op_type) { op_types_.insert(op_type); }

  void AddStepStat(int64 step, const string& device,
                   const NodeExecStats& step_stat);

  void AddFloatOps(int64 float_ops) { float_ops_ = float_ops; }

  // TODO(xpan): This could take a lot of memory.
  void AddCode(const CodeDef& code) { code_.MergeFrom(code); }

  const string& name() const { return node_->name(); }
  const string& op() const { return op_; }
  const NodeDef* node_def() { return node_; }

  bool trackable(int64 step) const {
    auto exec = execs_.find(step);
    if (exec == execs_.end()) return false;

    if (exec->second.all_start_micros() == 0) return false;
    if (canonical_device_.empty() || host_device_.empty()) {
      return false;
    }
    return true;
  }

  const std::map<string, TFGraphNode*>& inputs() const { return inputs_; }
  const std::map<string, int64>& output_idx() const { return output_idx_; }

  // This is time spent in kernel execution.
  int64 kernel_exec_micros(int64 step) const {
    if (execs_.empty()) {
      return 0;
    }
    if (step >= 0) {
      auto exec = execs_.find(step);
      CHECK(exec != execs_.end());
      return exec->second.exec_micros();
    }

    int64 total_micros = 0;
    for (const auto& exec : execs_) {
      total_micros += exec.second.exec_micros();
    }
    return total_micros / execs_.size();
  }

  int64 requested_bytes(int64 step) const {
    if (execs_.empty()) {
      return 0;
    }
    if (step >= 0) {
      auto exec = execs_.find(step);
      CHECK(exec != execs_.end()) << "unknown step " << step;
      return exec->second.requested_bytes();
    }

    int64 requested_bytes = 0;
    for (const auto& exec : execs_) {
      requested_bytes += exec.second.requested_bytes();
    }
    return requested_bytes / execs_.size();
  }

  int64 all_start_micros(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.all_start_micros();
  }

  int64 latest_end_rel_micros(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.latest_end_rel_micros();
  }

  const std::map<string, std::vector<std::pair<int64, int64>>>& op_execs(
      int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.op_execs();
  }

  int64 accelerator_temp_bytes(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.accelerator_temp_bytes();
  }
  int64 host_temp_bytes(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.host_temp_bytes();
  }
  int64 accelerator_persistent_bytes(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.accelerator_persistent_bytes();
  }
  int64 host_persistent_bytes(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.host_persistent_bytes();
  }
  const std::map<int64, std::pair<int64, uint64>>& output_bytes(
      int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.output_bytes();
  }
  int64 allocator_bytes_in_use(int64 step) const {
    auto exec = execs_.find(step);
    CHECK(exec != execs_.end()) << "unknown step " << step;
    return exec->second.allocator_bytes_in_use();
  }

  int64 float_ops() const { return float_ops_; }
  const CodeDef& code() { return code_; }
  string canonical_device() const { return canonical_device_; }
  string host_device() const { return host_device_; }
  const std::set<string>& op_types() const { return op_types_; }
  const std::vector<int64>& shape() const { return shape_; }

 private:
  void update_shape(const std::vector<int64>& shape) { shape_ = shape; }

  std::map<string, TFGraphNode*> inputs_;
  std::map<string, int64> output_idx_;

  const NodeDef* node_;

  CodeDef code_;

  std::vector<int64> shape_;
  std::set<string> op_types_;

  std::map<int64, ExecStep> execs_;

  // /j:#/t:#/r:#/device:#. A canonical device name without extra suffix.
  string canonical_device_;
  // The host device name.
  string host_device_;

  int64 float_ops_;

  string op_;
};

class TFMultiGraphNode {
 public:
  TFMultiGraphNode(const string& name)
      : name_(name),
        kernel_exec_micros_(0),
        requested_bytes_(0),
        float_ops_(0) {}

  bool SnapshotNodes(int64 step, const std::vector<string>& type_regexes) {
    kernel_exec_micros_ = 0;
    requested_bytes_ = 0;
    float_ops_ = 0;
    op_types_.clear();
    shapes_.clear();
    devices_.clear();
    snapshot_nodes_.clear();

    std::vector<const TFGraphNode*> nodes = pick_nodes(type_regexes);

    if (nodes.empty()) {
      return (type_regexes.size() == 1 && type_regexes[0] == ".*");
    }

    for (const TFGraphNode* node : nodes) {
      op_types_.insert(node->op_types().begin(), node->op_types().end());

      kernel_exec_micros_ += node->kernel_exec_micros(step);
      requested_bytes_ += node->requested_bytes(step);
      float_ops_ += node->float_ops();
      if (node->shape().size() > 0) {
        shapes_.push_back(node->shape());
      }
      devices_.insert(node->canonical_device());
      snapshot_nodes_[node->name()] = node;
    }
    return true;
  }

  void AddGraphNode(const TFGraphNode* node) {
    if (nodes_.find(node->name()) != nodes_.end()) {
      return;
    }
    nodes_[node->name()] = node;
  }

  const std::map<string, const TFGraphNode*>& graph_nodes() const {
    return snapshot_nodes_;
  }

  void AddChildren(const string& name) {
    if (children_.find(name) != children_.end()) {
      return;
    }
    children_[name].reset(new TFMultiGraphNode(name));
  }
  const std::map<string, std::unique_ptr<TFMultiGraphNode>>& children() const {
    return children_;
  }

  const string& name() const { return name_; }

  int64 kernel_exec_micros() const { return kernel_exec_micros_; }

  int64 requested_bytes() const { return requested_bytes_; }

  int64 float_ops() const { return float_ops_; }

  const std::set<string>& devices() const { return devices_; }

  const std::set<string>& op_types() const { return op_types_; }

  const std::vector<std::vector<int64>>& shapes() const { return shapes_; }

 private:
  std::vector<const TFGraphNode*> pick_nodes(
      const std::vector<string>& type_regexes) {
    if (type_regexes.empty()) {
      return {};
    }
    std::vector<const TFGraphNode*> ret;
    if (type_regexes.size() == 1 && type_regexes[0] == ".*") {
      for (const auto& n : nodes_) {
        ret.push_back(n.second);
      }
      return ret;
    }

    for (const string& regex : type_regexes) {
      for (const auto& n : nodes_) {
        for (const string& type : n.second->op_types()) {
          if (RE2::FullMatch(type, regex)) {
            ret.push_back(n.second);
            break;
          }
        }
      }
    }
    return ret;
  }

  const string name_;
  // Snapshot based on type_regexes
  std::set<string> op_types_;
  int64 kernel_exec_micros_;
  int64 requested_bytes_;
  int64 float_ops_;
  std::set<string> devices_;
  std::vector<std::vector<int64>> shapes_;
  std::map<string, const TFGraphNode*> snapshot_nodes_;

  // Overall data held by the TFMultiGraphNode.
  std::map<string, const TFGraphNode*> nodes_;
  std::map<string, std::unique_ptr<TFMultiGraphNode>> children_;
};
}  // namespace tfprof
}  // namespace tensorflow

#endif  // THIRD_PARTY_TENSORFLOW_TOOLS_TFPROF_INTERNAL_TFPROF_NODE_H_
