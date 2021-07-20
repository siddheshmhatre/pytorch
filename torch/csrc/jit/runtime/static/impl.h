#pragma once

#include <ATen/core/interned_strings.h>
#include <ATen/core/ivalue.h>
#include <c10/core/CPUAllocator.h>
#include <jit/runtime/memory_planner.h>
#include <torch/csrc/jit/api/module.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/freeze_module.h>
#include <torch/csrc/jit/passes/inliner.h>

namespace torch {
namespace jit {

TORCH_API bool canEnableStaticRuntime(
    const std::shared_ptr<torch::jit::Graph>& graph);

struct TORCH_API StaticModuleOptions {
  // to batch allocate (deallocate) tensor storage for all non-escaping
  // temporary tensors
  bool cleanup_activations{true};
  // enabling out variant allows Static Runtime to do memory planning
  bool enable_out_variant{true};
  // to reuse tensor storage for tensors whose live-range do not overlap to
  // reduce memory footprint (enable_out_variant must be true)
  bool optimize_memory{true};
  // to batch allocate tensor storage for output tensors of the
  // graph, where storage is deallocated outside static runtime
  // (enable_out_variant must be true)
  bool optimize_graph_output_memory{false};
};

/// The static runime supports two execution modes.
///
/// Mode 1: single-threaded with no parallelism except for intra-op parallelism
/// For this mode, you can do either:
/// @code
///   // m is a TorchScript module
///   auto module = StaticModule(m, opts);
///   auto output = module(args, kwargs);
/// @endcode
///
/// or
///
/// @code
///   // g is the TorchScript graph
///   auto module = StaticModule(g, opts);
///   auto output = module(args, kwargs);
/// @endcode
///
/// Mode 2: similar to data parallelism, run the same model for different inputs
/// on different threads at the same time.
/// You should have one StaticModule per model, and one StaticRuntime instance
/// per running thread. To avoiding creating StaticRuntimes on the fly, use a
/// synchronized stack (i.e. boost::lockfree::stack) to cache all the
/// StaticRuntime instances in your code.
/// @code
///   // initialization
///   auto module = std::make_shared<StaticModule>(m, opts);
///
///   // 128 is good for most cases. Pick a number that works for you
///   boost::lockfree::stack<std::shared_ptr<StaticRuntime>,
///     boost::lockfree::fixed_sized<true>> pool(128);
///
///   // inference
///   std::shared_ptr<StaticRuntime> runtime = nullptr;
///   pool.pop(runtime);
///   if (!runtime) {
///     // holds a reference to the underlying module
///     // but does its own memory management
///     runtime = std::make_shared<StaticRuntime>(*module);
///   }
///   auto output = runtime(args, kwargs);
///   pool.push(runtime);
/// @endcode
///

class StaticRuntime;
class TORCH_API StaticModule {
 public:
  explicit StaticModule(
      std::shared_ptr<torch::jit::Graph> g,
      const StaticModuleOptions& opts = StaticModuleOptions());

  explicit StaticModule(
      const torch::jit::Module& m,
      const StaticModuleOptions& opts = StaticModuleOptions());

  typedef enum {
    CONSTANT_VALUE = -2, // VALUE nodes defined by prim::Constant
    INPUT_VALUE = -1, // VALUE nodes representing graph inputs
  } VALUE_KIND;

 private:
  explicit StaticModule(
      std::pair<std::shared_ptr<torch::jit::Graph>, std::shared_ptr<Module>>
          graph_and_module,
      const StaticModuleOptions& opts);

  // for <kind, idx>
  //   if kind == CONSTANT_VALUE: map to constants_[idx]
  //   if kind == INPUT_VALUE: map to inputs_[idx]
  //   otherwise: map to nodes_[kind].outputs()[idx]
  using DefInfo = std::pair<int, int>;

 public:
  std::vector<at::Tensor> operator()(const std::vector<at::Tensor>& inps);

  // This interface only works if StaticModule was initialized
  // with a TorchScript module, otherwise use the above interface
  c10::IValue operator()(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs);

  const Graph& graph() const {
    return *graph_;
  }

  const Module& module() const {
    return *module_;
  }

  const StaticModuleOptions& opts() const;
  size_t num_inputs() const;
  size_t num_outputs() const;

  const std::unordered_map<int, std::vector<DefInfo>>& index_map() const {
    return node_inputs_ssa_def_map_;
  }

  const std::vector<DefInfo>& output_indices() const {
    return output_ssa_defs_;
  }

  const std::vector<IValue>& constants() const {
    return constants_;
  }

  const std::vector<ProcessedNode>& nodes() const {
    return nodes_;
  }

  const c10::optional<c10::FunctionSchema>& schema() const {
    return schema_;
  }

  const std::unordered_map<const Value*, std::vector<const Value*>>&
  values_share_same_storage() const {
    return value_to_same_storage_values_;
  }

  const std::unordered_set<const Value*>& external_values() const {
    return external_values_;
  }

  bool first_input_is_self() const {
    return first_input_is_self_;
  }

  StaticRuntime& runtime();

 private:
  StaticModuleOptions opts_;
  bool first_input_is_self_{false};
  std::shared_ptr<torch::jit::Graph> graph_;
  std::shared_ptr<torch::jit::Module> module_;
  c10::optional<c10::FunctionSchema> schema_;
  std::unique_ptr<StaticRuntime> cached_runtime_;

  // Bookkeeping for creating new StaticRuntime instances
  // IValue table (defined by prim::Constant nodes)
  std::vector<IValue> constants_;
  // The nodes we need to run
  std::vector<ProcessedNode> nodes_;
  // a vector of ssa_defs corresponding to graph->outputs()
  std::vector<DefInfo> output_ssa_defs_;
  // map a node idx (in graph order) to a vector of ssa_defs for node inputs
  std::unordered_map<int, std::vector<DefInfo>> node_inputs_ssa_def_map_;

  // Bookkeeping for MemoryPlanner in StaticRuntime
  // values whose live-time exceeds that of running one inference (e.g., input,
  // output, prim::Constants, and their aliases)
  std::unordered_set<const Value*> external_values_;
  // map a value to the set of values that may share the same storage with it
  std::unordered_map<const Value*, std::vector<const Value*>>
      value_to_same_storage_values_;
};

class TORCH_API StaticRuntime {
 public:
  explicit StaticRuntime(const StaticModule& sm);

  std::vector<at::Tensor> operator()(const std::vector<at::Tensor>& inps);

  // This interface only works if StaticModule was initialized
  // with a TorchScript module, otherwise use the above interface
  c10::IValue operator()(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs);

  void display_nodes(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs);

  void benchmark(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs,
      const int warmup_runs,
      const int main_runs);

  float benchmark_model(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs,
      const int warmup_runs,
      const int main_runs);

  struct IndividualMetrics {
    float setup_time{0.0};
    float memory_alloc_time{0.0};
    float memory_dealloc_time{0.0};
    float output_dealloc_time{0.0};
    float total_time{0.0};
    size_t out_nodes_count{0};
    size_t total_nodes_count{0};
    std::vector<float> time_per_node;
    std::unordered_map<std::string, float> time_per_node_type;
    std::unordered_map<std::string, float> percent_per_node_type;
    std::unordered_map<std::string, int> instances_per_node_type;
    std::unordered_set<std::string> out_nodes;
  };

  IndividualMetrics benchmark_individual_ops(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs,
      const int warmup_runs,
      const int main_runs);

  // Input is readwrite
  IValue& Input(size_t i) {
    DCHECK(i < inputs_.size());
    return inputs_[i];
  }

  // Output is readonly. The writing process happens inside ProcessedNodes
  const IValue& Output(size_t i) const {
    DCHECK(i < outputs_.size());
    return *outputs_[i];
  }

  std::vector<IValue*> outputs() const {
    return outputs_;
  }

  const std::vector<ProcessedNode>& nodes() const {
    return nodes_;
  }

  std::vector<ProcessedNode>& nodes() {
    return nodes_;
  }

  const Graph& graph() const {
    return static_module_.graph();
  }

  void check_for_memory_leak(bool output_returned = true);

 private:
  // helper method for copying input args/kwargs into inputs_
  void set_inputs(
      const std::vector<c10::IValue>& args,
      const std::unordered_map<std::string, c10::IValue>& kwargs);

  // clean up owning refs of input IValues
  void clean_up_input_ivalues() {
    for (IValue& ival : inputs_) {
      ival = IValue();
    }
  }

  // Memory planning is only enabled if sm->opts().cleanup_activations is true.
  // Otherwise, the memory used by activations is cached inside the static
  // runtime.
  const StaticModule& static_module_;
  std::unique_ptr<MemoryPlanner> planner_;
  std::vector<IValue> inputs_;
  std::vector<IValue*> outputs_;
  std::vector<ProcessedNode> nodes_;
};

} // namespace jit
} // namespace torch
