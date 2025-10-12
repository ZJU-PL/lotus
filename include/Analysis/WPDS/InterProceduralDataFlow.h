#ifndef ANALYSIS_INTERPROCEDURALDATAFLOW_H_
#define ANALYSIS_INTERPROCEDURALDATAFLOW_H_

#include "LLVMUtils/SystemHeaders.h"
#include "Analysis/Mono/DataFlow.h"
#include "Solvers/WPDS/WPDS.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/semiring.h"
#include "Solvers/WPDS/key_source.h"
#include "Solvers/WPDS/keys.h"
#include <memory>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace dataflow {

// DataFlowFacts is the domain of our analysis
class DataFlowFacts {
public:
    DataFlowFacts();
    DataFlowFacts(const std::set<Value*>& facts);
    DataFlowFacts(const DataFlowFacts& other);
    ~DataFlowFacts() = default;

    DataFlowFacts& operator=(const DataFlowFacts& other);
    bool operator==(const DataFlowFacts& other) const;

    // Required set operations for WPDS
    static DataFlowFacts EmptySet();
    static DataFlowFacts UniverseSet();
    static DataFlowFacts Union(const DataFlowFacts& x, const DataFlowFacts& y);
    static DataFlowFacts Intersect(const DataFlowFacts& x, const DataFlowFacts& y);
    static DataFlowFacts Diff(const DataFlowFacts& x, const DataFlowFacts& y);
    static bool Eq(const DataFlowFacts& x, const DataFlowFacts& y);

    // Get the underlying set of facts
    const std::set<Value*>& getFacts() const;
    void addFact(Value* val);
    void removeFact(Value* val);
    bool containsFact(Value* val) const;
    std::size_t size() const;
    bool isEmpty() const;

    // Debug printing
    std::ostream& print(std::ostream& os) const;

private:
    std::set<Value*> facts;
    static std::set<Value*> universe;  // Used for UniverseSet
};

// GenKillTransformer implements the semiring operations for gen/kill data flow problems
class GenKillTransformer {
public:
    GenKillTransformer();
    GenKillTransformer(const DataFlowFacts& kill, const DataFlowFacts& gen);
    ~GenKillTransformer() = default;

    // Factory method to ensure unique representatives for special values
    static GenKillTransformer* makeGenKillTransformer(
        const DataFlowFacts& kill, 
        const DataFlowFacts& gen);

    // Semiring operations required by WPDS
    static GenKillTransformer* one();
    static GenKillTransformer* zero();
    static GenKillTransformer* bottom();
    GenKillTransformer* extend(GenKillTransformer* other);
    GenKillTransformer* combine(GenKillTransformer* other);
    GenKillTransformer* diff(GenKillTransformer* other);
    GenKillTransformer* quasiOne() const;
    bool equal(GenKillTransformer* other) const;

    // Apply the transformer to a set of facts
    DataFlowFacts apply(const DataFlowFacts& input);

    // Getters for the gen and kill sets
    const DataFlowFacts& getKill() const;
    const DataFlowFacts& getGen() const;

    // Debug printing
    std::ostream& print(std::ostream& os) const;

    // Reference counter for ref_ptr
    int count;

private:
    DataFlowFacts kill;
    DataFlowFacts gen;
    // Special constructor for one/zero/bottom
    GenKillTransformer(const DataFlowFacts& k, const DataFlowFacts& g, int);
};

// InterProceduralDataFlowEngine implements inter-procedural dataflow analysis using WPDS
class InterProceduralDataFlowEngine {
public:
    InterProceduralDataFlowEngine();
    ~InterProceduralDataFlowEngine() = default;

    // Main method to run forward inter-procedural dataflow analysis
    std::unique_ptr<DataFlowResult> runForwardAnalysis(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const std::set<Value*>& initialFacts = {});

    // Main method to run backward inter-procedural dataflow analysis
    std::unique_ptr<DataFlowResult> runBackwardAnalysis(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const std::set<Value*>& initialFacts = {});

    // Helper methods to query results
    const std::set<Value*>& getInSet(Instruction* inst) const;
    const std::set<Value*>& getOutSet(Instruction* inst) const;

private:
    // Convert LLVM Module to WPDS
    void buildWPDS(
        Module& m, 
        wpds::WPDS<GenKillTransformer>& wpds,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer);

    // Create a configuration automaton for the initial states
    void buildInitialAutomaton(
        Module& m, 
        wpds::CA<GenKillTransformer>& ca,
        const std::set<Value*>& initialFacts,
        bool isForward);

    // Map WPDS keys to LLVM values for easy lookup
    wpds::wpds_key_t getKeyForFunction(Function* f);
    wpds::wpds_key_t getKeyForInstruction(Instruction* inst);
    wpds::wpds_key_t getKeyForBasicBlock(BasicBlock* bb);
    wpds::wpds_key_t getKeyForCallSite(CallInst* callInst);
    wpds::wpds_key_t getKeyForReturnSite(CallInst* callInst);

    // Extract dataflow results from the resulting automaton
    void extractResults(
        Module& m,
        wpds::CA<GenKillTransformer>& resultCA,
        std::unique_ptr<DataFlowResult>& result,
        bool isForward);

    // Map program elements to WPDS keys and vice versa
    std::map<Function*, wpds::wpds_key_t> functionToKey;          // function entry keys
    std::map<Function*, wpds::wpds_key_t> functionExitToKey;      // function exit keys
    std::map<Instruction*, wpds::wpds_key_t> instToKey;
    std::map<BasicBlock*, wpds::wpds_key_t> bbToKey;
    std::map<wpds::wpds_key_t, Instruction*> keyToInst;

    // Maintain the dataflow result for the most recent analysis
    mutable std::unique_ptr<DataFlowResult> currentResult;
};

} // namespace dataflow

#endif // ANALYSIS_INTERPROCEDURALDATAFLOW_H_ 