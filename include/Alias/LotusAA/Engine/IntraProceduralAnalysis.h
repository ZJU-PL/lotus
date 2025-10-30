/*
 * LotusAA - Function-Level Pointer Analysis
 * 
 * Flow-sensitive, field-sensitive intra-procedural pointer analysis.
 * This is the core analysis engine that processes individual functions.
 * 
 * Key Responsibilities:
 * - Process LLVM instructions to build points-to graph
 * - Generate function summaries (inputs/outputs/escaped objects)
 * - Track field-sensitive memory objects
 * - Support inter-procedural analysis via summaries
 */

#pragma once

#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <map>
#include <set>

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/MemoryModel/PointsToGraph.h"
#include "Alias/LotusAA/MemoryModel/Types.h"

namespace llvm {

/*
 * Global configuration for LotusAA
 */
class IntraLotusAAConfig {
public:
  static int lotus_restrict_inline_depth;
  static double lotus_timeout;
  static int lotus_restrict_cg_size;
  static bool lotus_test_correctness;
  static int lotus_restrict_inline_size;
  static int lotus_restrict_ap_level;

  static void setParam();
};

/*
 * IntraLotusAA - Intra-procedural pointer analysis
 */
class IntraLotusAA : public PTGraph {
public:
  PTGType getKind() const { return IntraLotusAATy; }

  static bool classof(const PTGraph *G) {
    return G->getKind() == IntraLotusAATy;
  }

  // Symbolic access path: parent->offset
  class AccessPath {
    Value *parent;
    int64_t offset;

  public:
    AccessPath(Value *parent = nullptr, int64_t offset = 0)
        : parent(parent), offset(offset) {}
    AccessPath(const AccessPath &info)
        : parent(info.parent), offset(info.offset) {}
    AccessPath &operator=(const AccessPath &info) {
      parent = info.parent;
      offset = info.offset;
      return *this;
    }

    void reset(Value *p = nullptr, int64_t off = 0) {
      parent = p;
      offset = off;
    }

    int64_t getOffset() { return offset; }
    Value *getParentPtr() { return parent; }

    friend class IntraLotusAA;
  };

  // Function output
  class OutputItem {
    AccessPath symbolic_info;
    std::map<ReturnInst *, mem_value_t, llvm_cmp> val;
    Type *output_ty;
    std::vector<AccessPath> pseudo_pts;  // Simplified (no conditions)
    int func_level;

  public:
    AccessPath &getSymbolicInfo() { return symbolic_info; }
    std::vector<AccessPath> &getPseudoPointTo() { return pseudo_pts; }
    std::map<ReturnInst *, mem_value_t, llvm_cmp> &getVal() { return val; }
    void setType(Type *type) { output_ty = type; }
    Type *getType() { return output_ty; }
    int getFuncLevel() { return func_level; }

    friend class IntraLotusAA;
  };

private:
  using func_arg_t = std::map<Value *, mem_value_t, llvm_cmp>;
  using cg_result_t = std::set<Function *, llvm_cmp>;

  // Function interface
  std::map<Value *, AccessPath, llvm_cmp> inputs;
  std::map<Value *, int, llvm_cmp> inputs_func_level;
  std::vector<OutputItem *> outputs;
  std::set<MemObject *, mem_obj_cmp> escape_objs;
  std::set<Value *, llvm_cmp> escape_source;

  // Return instructions
  std::map<ReturnInst *, bool, llvm_cmp> ret_insts;

  // Call information
  std::map<Value *, std::map<Function *, func_arg_t, llvm_cmp>, llvm_cmp> func_arg;
  std::map<Instruction *, std::map<Function *, std::vector<Value *>, llvm_cmp>, llvm_cmp> func_ret;
  std::map<Value *, std::pair<Instruction *, int>, llvm_cmp> func_pseudo_ret_cache;

  // CG resolution
  std::map<Value *, cg_result_t, llvm_cmp> cg_resolve_result;
  
  // CG summaries
  std::vector<cg_result_t> output_cg_summary;
  std::map<Argument *, std::map<cg_result_t *, bool>, llvm_cmp> input_cg_summary;

  // Escaped object mapping
  using escapedMap = std::map<MemObject *, MemObject *, mem_obj_cmp>;
  std::map<Value *, std::map<Function *, escapedMap, llvm_cmp>, llvm_cmp> func_escape;
  
  // Pseudo objects for merging
  std::map<MemObject *, MemObject *, mem_obj_cmp> real_to_pseudo_map;
  std::map<MemObject *, std::set<MemObject *, mem_obj_cmp>, mem_obj_cmp> pseudo_to_real_map;

  // Access path tracking for escaped objects
  std::map<Value *, std::pair<AccessPath, int64_t>, llvm_cmp> escape_obj_path;
  std::map<Value *, std::pair<Value *, int64_t>, llvm_cmp> escape_ret_path;

  // Topological BB order
  std::vector<BasicBlock *> topBBs;

  // Special objects
  MemObject *func_obj;
  Argument *func_new;

  // Value sequence
  std::map<Value *, int, llvm_cmp> value_seq;

  // Flags
  bool is_PTA_computed;
  bool is_CG_computed;
  bool is_considered_as_library;
  bool is_timeout_found;

  int inline_ap_depth;

  // Index for escaped object pointers
  static const int PTR_TO_ESC_OBJ;

private:
  // Instruction processors
  PTResult *processPhi(PHINode *phi);
  void processLoad(LoadInst *load);
  void processStore(StoreInst *store);
  void processCall(CallBase *call);
  PTResult *processAlloca(AllocaInst *alloca);
  PTResult *processSelect(SelectInst *select);
  PTResult *processArg(Argument *arg);
  PTResult *processGlobal(GlobalValue *global);
  PTResult *processNullptr(ConstantPointerNull *null_ptr);
  PTResult *processNonPointer(Value *non_pointer_val);
  PTResult *processUnknown(Value *unknown_val);
  PTResult *processGepBitcast(Value *val);
  PTResult *processCast(CastInst *ptr);
  PTResult *processBasePointer(Value *val);

  void processUnknownLibraryCall(CallBase *call);
  
  void processCalleeInput(std::map<Value *, AccessPath, llvm_cmp> &callee_input,
                          std::map<Value *, int, llvm_cmp> &inputs_func_level,
                          std::vector<Value *> &real_args, std::vector<Value *> &formal_args,
                          CallBase *callsite, func_arg_t &result);

  void processCalleeOutput(std::vector<OutputItem *> &callee_output,
                           std::set<MemObject *, mem_obj_cmp> &callee_escape,
                           Instruction *callsite, Function *callee);

  // Helper functions for processCalleeOutput
  std::vector<Value *> &createPseudoOutputNodes(std::vector<OutputItem *> &callee_output,
                                                 Instruction *callsite, Function *callee);
  
  void createEscapedObjects(std::set<MemObject *, mem_obj_cmp> &callee_escape,
                            Instruction *callsite, Function *callee,
                            std::map<Value *, MemObject *, llvm_cmp> &escape_object_map);
  
  void linkOutputPointsToResults(OutputItem *output, Value *curr_output,
                                  std::map<Value *, MemObject *, llvm_cmp> &escape_object_map,
                                  func_arg_t &callee_func_arg,
                                  std::set<PTResult *> &visited);
  
  void linkOutputValues(OutputItem *output, Value *curr_output, size_t idx,
                        std::map<Value *, MemObject *, llvm_cmp> &escape_object_map,
                        func_arg_t &callee_func_arg,
                        Instruction *callsite,
                        std::unordered_map<PTResult *, PTResultIterator> &pt_result_cache);

  void collectOutputs();
  void collectInputs();
  void finalizeInterface();
  void cacheFunctionCallInfo();

  void collectEscapedObjects(
      std::map<MemObject *, MemObject *, mem_obj_cmp> &real_to_pseudo_map,
      std::map<MemObject *, std::set<MemObject *, mem_obj_cmp>, mem_obj_cmp> &pseudo_to_real_map);

  void resolveCallValue(Value *val, cg_result_t &target);

public:
  IntraLotusAA(Function *F, LotusAA *lotus_aa);
  ~IntraLotusAA();

  // Main analysis methods
  void computePTA();
  void computeCG();

  // Utilities
  void show();
  void showFunctionPointers();
  bool isPure();
  bool isPseudoInput(Value *val);
  bool isSameInterface(IntraLotusAA *to_compare);

  int getSequenceNum(Value *val) override;
  int getInlineApDepth() override;
  PTGraph *getPtGraph(Function *F) override;

  std::map<Value *, AccessPath, llvm_cmp> &getInputs() { return inputs; }
  std::vector<OutputItem *> &getOutputs() { return outputs; }
  std::set<MemObject *, mem_obj_cmp> &getEscapeObjs() { return escape_objs; }

  void getReturnInst();
  
  // Access path utilities
  int getArgLevel(AccessPath &path);
  void getFullAccessPath(Value *target_val, 
                         std::vector<std::pair<Value*, int64_t>> &result);
  void getFullAccessPath(AccessPath &ap,
                         std::vector<std::pair<Value*, int64_t>> &result);
  void getFullOutputAccessPath(int output_index,
                               std::vector<std::pair<Value*, int64_t>> &result);

  // Caller-callee object mapping
  void getCallerObj(Value *call, Function *callee, SymbolicMemObject *calleeObj,
                    std::vector<std::pair<MemObject *, int64_t>> &result);
  MemObject *getCallerEscapeObj(Value *call, Function *callee, MemObject *calleeObj);

  // Memory cleanup
  void clearIntermediatePtsResult();
  void clearIntermediateCgResult();
  void clearGlobalCgResult();
  void clearMemObjectResult();
  void clearInterfaceResult();

  // Interface check
  bool isPseudoInterface(Value *target);

  friend class LotusAA;
};

} // namespace llvm


