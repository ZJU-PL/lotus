// the basic framework for andersen-based algorithm, including common routines
// override neccessary ones, and the call will be STATICALLY redirected to it
#ifndef ASER_PTA_SOLVERBASE_H
#define ASER_PTA_SOLVERBASE_H

#define DEBUG_TYPE "pta"
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "Alias/AserPTA/PointerAnalysis/Graph/ConstraintGraph/ConstraintGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/MemModelTrait.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/CallGraph.h"
#include "Alias/AserPTA/Util/Statistics.h"
#include "Alias/AserPTA/Util/Log.h"

extern llvm::cl::opt<bool> ConfigPrintConstraintGraph;
extern llvm::cl::opt<bool> ConfigPrintCallGraph;
extern llvm::cl::opt<bool> ConfigDumpPointsToSet;

namespace aser {

template <typename ctx>
class CallGraph;

template <typename LangModel, typename SubClass>
class SolverBase {
private:
    struct Noop {
        template <typename... Args>
        __attribute__((always_inline)) void operator()(Args &&...) {}
    };
    std::unique_ptr<LangModel> langModel;

    LOCAL_STATISTIC(ProcessedCopy, "Number of Processed Copy Edges");
    LOCAL_STATISTIC(ProcessedLoad, "Number of Processed Load Edges");
    LOCAL_STATISTIC(ProcessedStore, "Number of Processed Store Edges");
    LOCAL_STATISTIC(ProcessedOffset, "Number of Processed Offset Edges");

    LOCAL_STATISTIC(EffectiveCopy, "Number of Effective Copy Edges");
    LOCAL_STATISTIC(EffectiveLoad, "Number of Effective Load Edges");
    LOCAL_STATISTIC(EffectiveStore, "Number of Effective Store Edges");
    LOCAL_STATISTIC(EffectiveOffset, "Number of Effective Offset Edges");

public:
    using LMT = LangModelTrait<LangModel>;
    using MemModel = typename LMT::MemModelTy;
    using MMT = MemModelTrait<MemModel>;
    using ctx = typename LangModelTrait<LangModel>::CtxTy;
    using CT = CtxTrait<ctx>;
    using ObjTy = typename MMT::ObjectTy;

protected:
    using PtsTy = typename LMT::PointsToTy;
    using PT = PTSTrait<PtsTy>;

    using CallGraphTy = CallGraph<ctx>;
    using CallNodeTy = typename CallGraphTy::NodeType;
    using ConsGraphTy = ConstraintGraph<ctx>;
    using CGNodeTy = CGNodeBase<ctx>;
    using PtrNodeTy = CGPtrNode<ctx>;
    using ObjNodeTy = CGObjNode<MemModel>;

    ConsGraphTy *consGraph;
    llvm::SparseBitVector<> updatedFunPtrs;

    // TODO: the intersection on pts should be done through PtsTrait for better extensibility
    llvm::DenseMap<PtrNodeTy *, llvm::SparseBitVector<5120>> handledGEPMap;

    inline void updateFunPtr(NodeID indirectNode) {
        updatedFunPtrs.set(indirectNode);
    }

    inline bool resolveFunPtrs() {
        bool reanalyze = LMT::updateFunPtrs(langModel.get(), updatedFunPtrs);
        updatedFunPtrs.clear();

        return reanalyze;
    }

    // seems like the scc becomes the bottleneck, need to merge large scc
    // return the super node of the scc
    CGNodeTy *processCopySCC(const std::vector<CGNodeTy *> &scc) {
        assert(scc.size() > 1);

        CGNodeTy *superNode = scc.front();
        for (auto nit = ++(scc.begin()), nie = scc.end(); nit != nie; nit++) {
            // merge pts in scc all into front
            this->processCopy(*nit, superNode);
        }

        // collapse scc to the front node
        this->getConsGraph()->collapseSCCTo(scc, superNode);

        // if there is a function ptr in the scc, update the function ptr
        if (superNode->isFunctionPtr()) {
            this->updateFunPtr(superNode->getNodeID());
        }

        for (auto cit = superNode->succ_copy_begin(), cie = superNode->succ_copy_end(); cit != cie; cit++) {
            this->processCopy(superNode, *cit);
        }

        return superNode;
    }

    // some helper function that might be needed by subclasses
    constexpr inline bool processAddrOf(CGNodeTy *src, CGNodeTy *dst) const;
    inline bool processCopy(CGNodeTy *src, CGNodeTy *dst);

    // TODO: only process diff pts
    template <typename CallBack=Noop>
    inline bool processOffset(CGNodeTy *src, CGNodeTy *dst, CallBack callBack=Noop{}) {
        assert(!src->hasSuperNode() && !dst->hasSuperNode());
        ProcessedOffset ++;

        // TODO: use llvm::cast in debugging build
        // gep for sure create a pointer node
        CGPtrNode<ctx> *ptrNode = static_cast<CGPtrNode<ctx> *>(dst);
        // assert(ptrNode);

        // we must be handling a getelemntptr instruction if we are indexing a object
        auto gep = static_cast<const llvm::GetElementPtrInst *>(ptrNode->getPointer()->getValue());
        // assert(gep);

        // TODO: the intersection on pts should be done through PtsTrait for better extensibility
        llvm::SparseBitVector<5120> &handled = handledGEPMap.try_emplace(ptrNode).first->second;
        const llvm::SparseBitVector<5120> &curPts = PT::getPointsTo(src->getNodeID());

        llvm::SparseBitVector<5120> newGEPs;
        newGEPs.intersectWithComplement(curPts, handled);

        assert(!handled.intersects(newGEPs));
        assert(curPts.contains(newGEPs));
        handled |= newGEPs; // update handled gep

        bool changed = false;
        std::vector<ObjNodeTy *> nodeVec;
        size_t ptsSize = newGEPs.count();
        if (ptsSize == 0) {
            return false;
        }

        nodeVec.reserve(ptsSize);
        // We need to cache all the node here because the PT might be modified and the iterator might be invalid
        for (auto it = newGEPs.begin(), ie = newGEPs.end(); it != ie; ++it) {
            // TODO: use llvm::cast in debugging build
            auto objNode = static_cast<ObjNodeTy *>((*consGraph)[*it]);
            nodeVec.push_back(objNode);
        }

        // update the cached pts
        for (auto objNode : nodeVec) {
            // this might create new object, thus modify the points-to set
            CGNodeTy *fieldObj = LMT::indexObject(this->getLangModel(), objNode, gep);
            if (fieldObj == nullptr) {
                continue;
            }

            if (!PT::has(ptrNode->getNodeID(), fieldObj->getNodeID())) {
                // insert an addr_of constraint if ptrNode does not points to field object previous
//#ifndef NO_ADDR_OF_FOR_OFFSET
              // insert an addr_of constraint if ptrNode does not points to field object previous
                // this is the major source for newly inserted constraints
                // remove this but relying on solver to handle it correctly
                // can improve both performance and memory effciency
                // but the visualization of the constraint graph will be affected.
                this->consGraph->addConstraints(fieldObj, ptrNode, Constraints::addr_of);
//#endif
                //this->consGraph->addConstraints(fieldObj, ptrNode, Constraints::addr_of);
                callBack(fieldObj, ptrNode);
                changed = true;
            }
        }

        if (changed) {
            EffectiveOffset ++;
        }
        return changed;
    }

    // TODO: only process diff pts
    // src --LOAD-->dst
    // for every node in pts(src):
    //     node --COPY--> dst
    template <typename CallBack = Noop>
    bool processLoad(CGNodeTy *src, CGNodeTy *dst,
                     CallBack callBack = Noop{}, const typename PT::PtsTy *diffPts = nullptr) {
        assert(!src->hasSuperNode() && !dst->hasSuperNode());
        ProcessedLoad ++;
        if (diffPts == nullptr) {
            diffPts = &PT::getPointsTo(src->getNodeID());
        }

        bool changed = false;
        for (auto it = diffPts->begin(), ie = diffPts->end(); it != ie; it++) {
            auto node = (*consGraph)[*it];
            node = node->getSuperNode();
            if (consGraph->addConstraints(node, dst, Constraints::copy)) {
                changed = true;
                callBack(node, dst);
            }
        }

        if (changed) {
            EffectiveLoad ++;
        }
        return changed;
    }

    // TODO: only process diff pts
    // src --STORE-->dst
    // for every node in pts(dst):
    //      src --COPY--> node
    template <typename CallBack = Noop>
    bool processStore(CGNodeTy *src, CGNodeTy *dst,
                      CallBack callBack = Noop{}, const typename PT::PtsTy *diffPts = nullptr) {
        assert(!src->hasSuperNode() && !dst->hasSuperNode());
        if (diffPts == nullptr) {
            diffPts = &PT::getPointsTo(dst->getNodeID());
        }

        ProcessedStore ++;
        bool changed = false;
        for (auto it = diffPts->begin(), ie = diffPts->end(); it != ie; it++) {
            auto node = (*consGraph)[*it];
            node = node->getSuperNode();

            if (consGraph->addConstraints(src, node, Constraints::copy)) {
                changed = true;
                callBack(src, node);
            }
        }

        if (changed) {
            EffectiveStore ++;
        }
        return changed;
    }

    void solve() {
        // this is the main entrance of the pointer analysis, which performs the pointer analysis with on-the-fly call graph construction
        // TODO: add a new mode: use a pre-built call graph (e.g., DyckAA or some type-based approach)
        // A problem is: it might be hard to "hook" resolveFunPtrs to update the call graph.
        bool reanalyze;
        // from here
        do {
            static_cast<SubClass *>(this)->runSolver(*langModel);
            // resolve indirect calls in language model
            reanalyze = resolveFunPtrs();
        } while (reanalyze);
        // llvm::outs() << this->getConsGraph()->getNodeNum();
    }

    [[nodiscard]]
    inline LangModel *getLangModel() const {
        return this->langModel.get();
    }

    void dumpPointsTo() {
        std::error_code ErrInfo;
        std::string fileName;
        llvm::raw_string_ostream os(fileName);
        os << "PTS" << this;

        llvm::ToolOutputFile F(os.str(), ErrInfo, llvm::sys::fs::OF_None);
        if (!ErrInfo) {
            // dump the points to set

            // 1st, dump the Object Node Information
            for (auto it = this->getConsGraph()->begin(), ie = this->getConsGraph()->end();
                 it != ie; it++) {
                CGNodeTy *node = *it;
                if (llvm::isa<ObjNodeTy>(node)) {
                    // dump the information
                    F.os() << "Object " << node->getNodeID() << " : \n";
                    F.os() << node->toString() << "\n";
                }
            }

            // 2nd, dump the points to set of every node
            for (auto it = this->getConsGraph()->begin(), ie = this->getConsGraph()->end();
                 it != ie; it++) {
                CGNodeTy *node = *it;
                F.os() << node->toString() << " : ";
                F.os() << "{";
                bool isFirst = true;

                for (auto it = PT::begin(node->getNodeID()), ie = PT::end(node->getNodeID());
                     it != ie; it++) {
                    if (isFirst) {
                        F.os() << *it;
                        isFirst = false;
                    } else {
                        F.os() << " ," << *it;
                    }

                }
                F.os() << "}\n\n\n";
            }

            if (!F.os().has_error()) {
                llvm::outs() << "\n";
                F.keep();
                return;
            }
        }
    }

public:
    virtual ~SolverBase() {
        CT::release();
        PT::clearAll();
    }

    // analyze the give module with specified entry function
    bool analyze(llvm::Module *module, llvm::StringRef entry = "main") {
        assert(langModel == nullptr && "can not run pointer analysis twice");
        // ensure the points to set are cleaned.
        // TODO: support different point-to set instance for different PTA instance
        // new they all share a global vector to store it.
        PT::clearAll();

        // using language model to construct language model
        langModel.reset(LMT::buildInitModel(module, entry));
        LMT::constructConsGraph(langModel.get());

        consGraph = LMT::getConsGraph(langModel.get());

        std::string fileName;
        llvm::raw_string_ostream os(fileName);
        os << this;

        if (ConfigPrintConstraintGraph) {
            WriteGraphToFile("ConstraintGraph_Initial_" + os.str(), *this->getConsGraph());
        }

        // subclass might override solve() directly for more aggressive overriding
        static_cast<SubClass *>(this)->solve();

        LOG_DEBUG("PTA constraint graph node number {}, "
                  "callgraph node number {}",
                  this->getConsGraph()->getNodeNum(),
                  this->getCallGraph()->getNodeNum());

        if (ConfigPrintConstraintGraph) {
            WriteGraphToFile("ConstraintGraph_Final_" + os.str(), *this->getConsGraph());
        }
        if (ConfigPrintCallGraph) {
            WriteGraphToFile("CallGraph_Final_" + os.str(), *this->getCallGraph());
        }
        if (ConfigDumpPointsToSet) {
            // dump the points to set of every pointers
            dumpPointsTo();
        }

        return false;
    }

    CGNodeTy *getCGNode(const ctx *context, const llvm::Value *V) const {
        NodeID id = LMT::getSuperNodeIDForValue(langModel.get(), context, V);
        return (*consGraph)[id];
    }

    void getPointsTo(const ctx *context, const llvm::Value *V, std::vector<const ObjTy *> &result) const {
        assert(V->getType()->isPointerTy());

        // get the node value
        NodeID node = LMT::getSuperNodeIDForValue(langModel.get(), context, V);
        if (node == INVALID_NODE_ID) {
            return;
        }

        for (auto it = PT::begin(node), ie = PT::end(node); it != ie; it++) {
            auto objNode = llvm::dyn_cast<ObjNodeTy>(consGraph->getNode(*it));
            assert(objNode);
            if (objNode->isSpecialNode()) {
                continue;
            }
            result.push_back(objNode->getObject());
        }
    }

    const llvm::Type *getPointedType(const ctx *context, const llvm::Value *V) const {
        std::vector<const ObjTy *> result;
        getPointsTo(context, V, result);

        if (result.size() == 1) {
            const llvm::Type *type = result[0]->getType();
            // the allocation site is a pointer type
            assert(type->isPointerTy());
            // get the actually allocated object type
            return type->getPointerElementType();
        }
        // do not know the type
        return nullptr;
    }

    [[nodiscard]] bool alias(const ctx *c1, const llvm::Value *v1, const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        assert(n1 != INVALID_NODE_ID && n2 != INVALID_NODE_ID && "can not find node in constraint graph!");
        return PT::intersectWithNoSpecialNode(n1, n2);
    }

    [[nodiscard]] bool aliasIfExsit(const ctx *c1, const llvm::Value *v1, const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        if(n1 == INVALID_NODE_ID || n2 == INVALID_NODE_ID) {
            return false;
        }
        return PT::intersectWithNoSpecialNode(n1, n2);
    }

    [[nodiscard]] bool hasIdenticalPTS(const ctx *c1, const llvm::Value *v1,
                                       const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        assert(n1 != INVALID_NODE_ID && n2 != INVALID_NODE_ID && "can not find node in constraint graph!");
        return PT::equal(n1, n2);
    }

    [[nodiscard]] bool containsPTS(const ctx *c1, const llvm::Value *v1,
                                   const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        assert(n1 != INVALID_NODE_ID && n2 != INVALID_NODE_ID && "can not find node in constraint graph!");
        return PT::contains(n1, n2);
    }



    // Delegator of the language model
    [[nodiscard]]
    inline ConsGraphTy *getConsGraph() const {
        return LMT::getConsGraph(langModel.get());
    }

    [[nodiscard]]
    inline const CallGraphTy *getCallGraph() const {
        return LMT::getCallGraph(langModel.get());
    }

    [[nodiscard]]
    inline llvm::StringRef getEntryName() const {
        return LMT::getEntryName(this->getLangModel());
    }

    [[nodiscard]]
    inline const llvm::Module *getLLVMModule() const {
        return LMT::getLLVMModule(this->getLangModel());
    }

    [[nodiscard]]
    inline const CallGraphNode<ctx> *getDirectNode(const ctx *C, const llvm::Function *F) {
        return LMT::getDirectNode(this->getLangModel(), C, F); //->getDirectNode(C, F);
    }

    [[nodiscard]]
    inline const CallGraphNode<ctx> *getDirectNodeOrNull(const ctx *C, const llvm::Function *F) {
        return LMT::getDirectNodeOrNull(this->getLangModel(), C, F);
    }

    [[nodiscard]]
    inline const InDirectCallSite<ctx> *getInDirectCallSite(const ctx *C, const llvm::Instruction *I) {
        return LMT::getInDirectCallSite(this->getLangModel(), C, I);
    }
};

template <typename LangModel, typename SubClass>
constexpr bool SolverBase<LangModel, SubClass>::processAddrOf(CGNodeTy *src, CGNodeTy *dst) const {
#ifndef NDEBUG
    // should already been handled
    assert(!PT::insert(dst->getNodeID(), src->getNodeID()));
#endif
    return false;
}

// site. pts(dst) |= pts(src);
template <typename LangModel, typename SubClass>
bool SolverBase<LangModel, SubClass>::processCopy(CGNodeTy *src, CGNodeTy *dst) {
    ProcessedCopy++;
    if (PT::unionWith(dst->getNodeID(), src->getNodeID())) {
        if (dst->isFunctionPtr()) {
            // node used for indirect call
            this->updateFunPtr(dst->getNodeID());
        }
        EffectiveCopy++;
        return true;
    }
    return false;
}

}  // namespace aser

#undef DEBUG_TYPE

#endif