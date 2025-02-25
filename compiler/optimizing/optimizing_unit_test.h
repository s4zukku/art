/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_

#include <memory>
#include <string_view>
#include <vector>

#include "base/indenter.h"
#include "base/malloc_arena_pool.h"
#include "base/scoped_arena_allocator.h"
#include "builder.h"
#include "common_compiler_test.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file.h"
#include "dex/dex_instruction.h"
#include "dex/standard_dex_file.h"
#include "driver/dex_compilation_unit.h"
#include "graph_checker.h"
#include "gtest/gtest.h"
#include "handle_scope-inl.h"
#include "handle_scope.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "scoped_thread_state_change.h"
#include "ssa_builder.h"
#include "ssa_liveness_analysis.h"

namespace art {

#define NUM_INSTRUCTIONS(...)  \
  (sizeof((uint16_t[]) {__VA_ARGS__}) /sizeof(uint16_t))

#define N_REGISTERS_CODE_ITEM(NUM_REGS, ...)                            \
    { NUM_REGS, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define ZERO_REGISTER_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(0, __VA_ARGS__)
#define ONE_REGISTER_CODE_ITEM(...)    N_REGISTERS_CODE_ITEM(1, __VA_ARGS__)
#define TWO_REGISTERS_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(2, __VA_ARGS__)
#define THREE_REGISTERS_CODE_ITEM(...) N_REGISTERS_CODE_ITEM(3, __VA_ARGS__)
#define FOUR_REGISTERS_CODE_ITEM(...)  N_REGISTERS_CODE_ITEM(4, __VA_ARGS__)
#define FIVE_REGISTERS_CODE_ITEM(...)  N_REGISTERS_CODE_ITEM(5, __VA_ARGS__)
#define SIX_REGISTERS_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(6, __VA_ARGS__)

inline LiveInterval* BuildInterval(const size_t ranges[][2],
                                   size_t number_of_ranges,
                                   ScopedArenaAllocator* allocator,
                                   int reg = -1,
                                   HInstruction* defined_by = nullptr) {
  LiveInterval* interval =
      LiveInterval::MakeInterval(allocator, DataType::Type::kInt32, defined_by);
  if (defined_by != nullptr) {
    defined_by->SetLiveInterval(interval);
  }
  for (size_t i = number_of_ranges; i > 0; --i) {
    interval->AddRange(ranges[i - 1][0], ranges[i - 1][1]);
  }
  interval->SetRegister(reg);
  return interval;
}

inline void RemoveSuspendChecks(HGraph* graph) {
  for (HBasicBlock* block : graph->GetBlocks()) {
    if (block != nullptr) {
      if (block->GetLoopInformation() != nullptr) {
        block->GetLoopInformation()->SetSuspendCheck(nullptr);
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        HInstruction* current = it.Current();
        if (current->IsSuspendCheck()) {
          current->GetBlock()->RemoveInstruction(current);
        }
      }
    }
  }
}

class ArenaPoolAndAllocator {
 public:
  ArenaPoolAndAllocator()
      : pool_(), allocator_(&pool_), arena_stack_(&pool_), scoped_allocator_(&arena_stack_) { }

  ArenaAllocator* GetAllocator() { return &allocator_; }
  ArenaStack* GetArenaStack() { return &arena_stack_; }
  ScopedArenaAllocator* GetScopedAllocator() { return &scoped_allocator_; }

 private:
  MallocArenaPool pool_;
  ArenaAllocator allocator_;
  ArenaStack arena_stack_;
  ScopedArenaAllocator scoped_allocator_;
};

// Have a separate helper so the OptimizingCFITest can inherit it without causing
// multiple inheritance errors from having two gtest as a parent twice.
class OptimizingUnitTestHelper {
 public:
  OptimizingUnitTestHelper()
      : pool_and_allocator_(new ArenaPoolAndAllocator()),
        graph_(nullptr),
        entry_block_(nullptr),
        return_block_(nullptr),
        exit_block_(nullptr) { }

  ArenaAllocator* GetAllocator() { return pool_and_allocator_->GetAllocator(); }
  ArenaStack* GetArenaStack() { return pool_and_allocator_->GetArenaStack(); }
  ScopedArenaAllocator* GetScopedAllocator() { return pool_and_allocator_->GetScopedAllocator(); }

  void ResetPoolAndAllocator() {
    pool_and_allocator_.reset(new ArenaPoolAndAllocator());
  }

  HGraph* CreateGraph(VariableSizedHandleScope* handles = nullptr) {
    ArenaAllocator* const allocator = pool_and_allocator_->GetAllocator();

    // Reserve a big array of 0s so the dex file constructor can offsets from the header.
    static constexpr size_t kDexDataSize = 4 * KB;
    const uint8_t* dex_data = reinterpret_cast<uint8_t*>(allocator->Alloc(kDexDataSize));

    // Create the dex file based on the fake data. Call the constructor so that we can use virtual
    // functions. Don't use the arena for the StandardDexFile otherwise the dex location leaks.
    dex_files_.emplace_back(new StandardDexFile(
        dex_data,
        sizeof(StandardDexFile::Header),
        "no_location",
        /*location_checksum*/ 0,
        /*oat_dex_file*/ nullptr,
        /*container*/ nullptr));

    graph_ = new (allocator) HGraph(
        allocator,
        pool_and_allocator_->GetArenaStack(),
        handles,
        *dex_files_.back(),
        /*method_idx*/-1,
        kRuntimeISA);
    return graph_;
  }

  // Create a control-flow graph from Dex instructions.
  HGraph* CreateCFG(const std::vector<uint16_t>& data,
                    DataType::Type return_type = DataType::Type::kInt32,
                    VariableSizedHandleScope* handles = nullptr) {
    HGraph* graph = CreateGraph(handles);

    // The code item data might not aligned to 4 bytes, copy it to ensure that.
    const size_t code_item_size = data.size() * sizeof(data.front());
    void* aligned_data = GetAllocator()->Alloc(code_item_size);
    memcpy(aligned_data, &data[0], code_item_size);
    CHECK_ALIGNED(aligned_data, StandardDexFile::CodeItem::kAlignment);
    const dex::CodeItem* code_item = reinterpret_cast<const dex::CodeItem*>(aligned_data);

    {
      const DexCompilationUnit* dex_compilation_unit =
          new (graph->GetAllocator()) DexCompilationUnit(
              /* class_loader= */ Handle<mirror::ClassLoader>(),  // Invalid handle.
              /* class_linker= */ nullptr,
              graph->GetDexFile(),
              code_item,
              /* class_def_index= */ DexFile::kDexNoIndex16,
              /* method_idx= */ dex::kDexNoIndex,
              /* access_flags= */ 0u,
              /* verified_method= */ nullptr,
              /* dex_cache= */ Handle<mirror::DexCache>());  // Invalid handle.
      CodeItemDebugInfoAccessor accessor(graph->GetDexFile(), code_item, /*dex_method_idx*/ 0u);
      HGraphBuilder builder(graph, dex_compilation_unit, accessor, return_type);
      bool graph_built = (builder.BuildGraph() == kAnalysisSuccess);
      return graph_built ? graph : nullptr;
    }
  }

  void InitGraph(VariableSizedHandleScope* handles = nullptr) {
    CreateGraph(handles);
    entry_block_ = AddNewBlock();
    return_block_ = AddNewBlock();
    exit_block_ = AddNewBlock();

    graph_->SetEntryBlock(entry_block_);
    graph_->SetExitBlock(exit_block_);

    entry_block_->AddSuccessor(return_block_);
    return_block_->AddSuccessor(exit_block_);

    return_block_->AddInstruction(new (GetAllocator()) HReturnVoid());
    exit_block_->AddInstruction(new (GetAllocator()) HExit());
  }

  void AddParameter(HInstruction* parameter) {
    entry_block_->AddInstruction(parameter);
    parameters_.push_back(parameter);
  }

  HBasicBlock* AddNewBlock() {
    HBasicBlock* block = new (GetAllocator()) HBasicBlock(graph_);
    graph_->AddBlock(block);
    return block;
  }

  // Run GraphChecker with all checks.
  //
  // Return: the status whether the run is successful.
  bool CheckGraph(HGraph* graph, std::ostream& oss = std::cerr) {
    return CheckGraph(graph, /*check_ref_type_info=*/true, oss);
  }

  bool CheckGraph(std::ostream& oss = std::cerr) {
    return CheckGraph(graph_, oss);
  }

  // Run GraphChecker with all checks except reference type information checks.
  //
  // Return: the status whether the run is successful.
  bool CheckGraphSkipRefTypeInfoChecks(HGraph* graph, std::ostream& oss = std::cerr) {
    return CheckGraph(graph, /*check_ref_type_info=*/false, oss);
  }

  bool CheckGraphSkipRefTypeInfoChecks(std::ostream& oss = std::cerr) {
    return CheckGraphSkipRefTypeInfoChecks(graph_, oss);
  }

  HEnvironment* ManuallyBuildEnvFor(HInstruction* instruction,
                                    ArenaVector<HInstruction*>* current_locals) {
    HEnvironment* environment = new (GetAllocator()) HEnvironment(
        (GetAllocator()),
        current_locals->size(),
        graph_->GetArtMethod(),
        instruction->GetDexPc(),
        instruction);

    environment->CopyFrom(ArrayRef<HInstruction* const>(*current_locals));
    instruction->SetRawEnvironment(environment);
    return environment;
  }

  void EnsurePredecessorOrder(HBasicBlock* target, std::initializer_list<HBasicBlock*> preds) {
    // Make sure the given preds and block predecessors have the same blocks.
    BitVector bv(preds.size(), false, Allocator::GetMallocAllocator());
    auto preds_and_idx = ZipCount(MakeIterationRange(target->GetPredecessors()));
    bool correct_preds = preds.size() == target->GetPredecessors().size() &&
                         std::all_of(preds.begin(), preds.end(), [&](HBasicBlock* pred) {
                           return std::any_of(preds_and_idx.begin(),
                                              preds_and_idx.end(),
                                              // Make sure every target predecessor is used only
                                              // once.
                                              [&](std::pair<HBasicBlock*, uint32_t> cur) {
                                                if (cur.first == pred && !bv.IsBitSet(cur.second)) {
                                                  bv.SetBit(cur.second);
                                                  return true;
                                                } else {
                                                  return false;
                                                }
                                              });
                         }) &&
                         bv.NumSetBits() == preds.size();
    auto dump_list = [](auto it) {
      std::ostringstream oss;
      oss << "[";
      bool first = true;
      for (HBasicBlock* b : it) {
        if (!first) {
          oss << ", ";
        }
        first = false;
        oss << b->GetBlockId();
      }
      oss << "]";
      return oss.str();
    };
    ASSERT_TRUE(correct_preds) << "Predecessors of " << target->GetBlockId() << " are "
                               << dump_list(target->GetPredecessors()) << " not "
                               << dump_list(preds);
    if (correct_preds) {
      std::copy(preds.begin(), preds.end(), target->predecessors_.begin());
    }
  }

 protected:
  bool CheckGraph(HGraph* graph, bool check_ref_type_info, std::ostream& oss) {
    GraphChecker checker(graph);
    checker.SetRefTypeInfoCheckEnabled(check_ref_type_info);
    checker.Run();
    checker.Dump(oss);
    return checker.IsValid();
  }

  std::vector<std::unique_ptr<const StandardDexFile>> dex_files_;
  std::unique_ptr<ArenaPoolAndAllocator> pool_and_allocator_;

  HGraph* graph_;
  HBasicBlock* entry_block_;
  HBasicBlock* return_block_;
  HBasicBlock* exit_block_;

  std::vector<HInstruction*> parameters_;
};

class OptimizingUnitTest : public CommonArtTest, public OptimizingUnitTestHelper {};

// Naive string diff data type.
typedef std::list<std::pair<std::string, std::string>> diff_t;

// An alias for the empty string used to make it clear that a line is
// removed in a diff.
static const std::string removed = "";  // NOLINT [runtime/string] [4]

// Naive patch command: apply a diff to a string.
inline std::string Patch(const std::string& original, const diff_t& diff) {
  std::string result = original;
  for (const auto& p : diff) {
    std::string::size_type pos = result.find(p.first);
    DCHECK_NE(pos, std::string::npos)
        << "Could not find: \"" << p.first << "\" in \"" << result << "\"";
    result.replace(pos, p.first.size(), p.second);
  }
  return result;
}

// Returns if the instruction is removed from the graph.
inline bool IsRemoved(HInstruction* instruction) {
  return instruction->GetBlock() == nullptr;
}

class AdjacencyListGraph {
 public:
  using Edge = std::pair<const std::string_view, const std::string_view>;
  AdjacencyListGraph(
      HGraph* graph,
      ArenaAllocator* alloc,
      const std::string_view entry_name,
      const std::string_view exit_name,
      const std::vector<Edge>& adj) : graph_(graph) {
    auto create_block = [&]() {
      HBasicBlock* blk = new (alloc) HBasicBlock(graph_);
      graph_->AddBlock(blk);
      return blk;
    };
    HBasicBlock* entry = create_block();
    HBasicBlock* exit = create_block();
    graph_->SetEntryBlock(entry);
    graph_->SetExitBlock(exit);
    name_to_block_.Put(entry_name, entry);
    name_to_block_.Put(exit_name, exit);
    for (const auto& [src, dest] : adj) {
      HBasicBlock* src_blk = name_to_block_.GetOrCreate(src, create_block);
      HBasicBlock* dest_blk = name_to_block_.GetOrCreate(dest, create_block);
      src_blk->AddSuccessor(dest_blk);
    }
    graph_->ClearReachabilityInformation();
    graph_->ComputeDominanceInformation();
    graph_->ComputeReachabilityInformation();
    for (auto [name, blk] : name_to_block_) {
      block_to_name_.Put(blk, name);
    }
  }

  bool HasBlock(const HBasicBlock* blk) const {
    return block_to_name_.find(blk) != block_to_name_.end();
  }

  std::string_view GetName(const HBasicBlock* blk) const {
    return block_to_name_.Get(blk);
  }

  HBasicBlock* Get(const std::string_view& sv) const {
    return name_to_block_.Get(sv);
  }

  AdjacencyListGraph(AdjacencyListGraph&&) = default;
  AdjacencyListGraph(const AdjacencyListGraph&) = default;
  AdjacencyListGraph& operator=(AdjacencyListGraph&&) = default;
  AdjacencyListGraph& operator=(const AdjacencyListGraph&) = default;

  std::ostream& Dump(std::ostream& os) const {
    struct Namer : public BlockNamer {
     public:
      explicit Namer(const AdjacencyListGraph& alg) : BlockNamer(), alg_(alg) {}
      std::ostream& PrintName(std::ostream& os, HBasicBlock* blk) const override {
        if (alg_.HasBlock(blk)) {
          return os << alg_.GetName(blk) << " (" << blk->GetBlockId() << ")";
        } else {
          return os << "<Unnamed B" << blk->GetBlockId() << ">";
        }
      }

      const AdjacencyListGraph& alg_;
    };
    Namer namer(*this);
    return graph_->Dump(os, namer);
  }

 private:
  HGraph* graph_;
  SafeMap<const std::string_view, HBasicBlock*> name_to_block_;
  SafeMap<const HBasicBlock*, const std::string_view> block_to_name_;
};

inline std::ostream& operator<<(std::ostream& oss, const AdjacencyListGraph& alg) {
  return alg.Dump(oss);
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
