#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace gvfg {

class GuardedValueFlowGraph;

/// Common identity for values and instruction sites in a diagnostic trace.
/// The graph owns these objects; references remain valid for its lifetime.
class GuardedValueFlowObject {
public:
  enum class Domain { Node, Site };

  virtual ~GuardedValueFlowObject() = default;
  Domain getDomain() const { return domain_; }
  unsigned getObjectId() const { return object_id_; }

  virtual GuardedValueFlowGraph *getGraph() const = 0;
  virtual llvm::BasicBlock *getParentBasicBlock() const = 0;
  virtual llvm::Value *getDebugValue() const = 0;
  virtual llvm::Instruction *getDebugInstruction() const = 0;
  llvm::Function *getParentFunction() const;

protected:
  explicit GuardedValueFlowObject(Domain domain) : domain_(domain) {}

private:
  Domain domain_;
  unsigned object_id_{0};
  friend class GuardedValueFlowGraph;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                              const GuardedValueFlowObject &object);

} // namespace gvfg
} // namespace lotus
