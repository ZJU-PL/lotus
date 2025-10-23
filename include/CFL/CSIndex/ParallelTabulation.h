
#ifndef _PARALLEL_TABULATION_H
#define _PARALLEL_TABULATION_H

#include <set>
#include <memory>
#include <vector>
#include <future>
#include <mutex>

#include "AbstractQuery.h"
#include "Graph.h"

// Thread-safe wrapper for sets using thread-local storage
class ThreadSafeVisitedSet {
private:
    std::vector<std::set<int>> local_visited_sets;
    std::mutex mutex;

public:
    ThreadSafeVisitedSet(size_t num_threads) : local_visited_sets(num_threads) {}

    void clear(size_t thread_id) {
        if (thread_id < local_visited_sets.size()) {
            local_visited_sets[thread_id].clear();
        }
    }

    void insert(size_t thread_id, int value) {
        if (thread_id < local_visited_sets.size()) {
            local_visited_sets[thread_id].insert(value);
        }
    }

    bool count(size_t thread_id, int value) {
        if (thread_id < local_visited_sets.size()) {
            return local_visited_sets[thread_id].count(value) > 0;
        }
        return false;
    }

    std::set<int>& get_set(size_t thread_id) {
        if (thread_id < local_visited_sets.size()) {
            return local_visited_sets[thread_id];
        }
        static std::set<int> empty_set;
        return empty_set;
    }
};

class ParallelTabulation : public AbstractQuery {
private:
    Graph &vfg;
    size_t num_threads;
    std::unique_ptr<ThreadSafeVisitedSet> visited_sets;
    std::unique_ptr<ThreadSafeVisitedSet> func_visited_sets;

public:
    /**
     * @brief Constructor with graph reference and automatic thread detection
     * @param g Reference to the graph
     */
    explicit ParallelTabulation(Graph &g);

    /**
     * @brief Constructor with graph reference and specified number of threads
     * @param g Reference to the graph
     * @param threads Number of threads to use
     */
    ParallelTabulation(Graph &g, size_t threads);

    /**
     * @brief Check reachability between source and target vertices
     * @param s Source vertex ID
     * @param t Target vertex ID
     * @return true if reachable, false otherwise
     */
    bool reach(int s, int t) override;

    /**
     * @brief Check reachability within function body between source and target vertices
     * @param s Source vertex ID
     * @param t Target vertex ID
     * @param thread_id Thread ID for thread-local storage
     * @return true if reachable, false otherwise
     */
    bool reach_func(int s, int t, size_t thread_id);

    /**
     * @brief Check if edge represents a function call
     * @param s Source vertex ID
     * @param t Target vertex ID
     * @return true if call edge, false otherwise
     */
    bool is_call(int s, int t);

    /**
     * @brief Check if edge represents a function return
     * @param s Source vertex ID
     * @param t Target vertex ID
     * @return true if return edge, false otherwise
     */
    bool is_return(int s, int t);

    /**
     * @brief Compute transitive closure for all vertices (parallel version)
     * @return Memory usage in MB
     */
    double tc();

    /**
     * @brief Compute transitive closure using async/future pattern (alternative implementation)
     * @return Memory usage in MB
     */
    double tc_async();

    /**
     * @brief Process a range of vertices in parallel
     * @param start Start vertex index
     * @param end End vertex index
     * @param results Vector to store results
     * @param results_mutex Mutex for thread-safe result storage
     */
    void process_vertex_range(int start, int end,
                            std::vector<std::set<int>>& results,
                            std::mutex& results_mutex);

    /**
     * @brief Parallel traversal from source vertex
     * @param s Source vertex ID
     * @param tc Set to store reachable vertices
     * @param thread_id Thread ID for thread-local storage
     */
    void traverse_parallel(int s, std::set<int>& tc, size_t thread_id);

    /**
     * @brief Parallel traversal within function body from source vertex
     * @param s Source vertex ID
     * @param tc Set to store reachable vertices
     * @param thread_id Thread ID for thread-local storage
     */
    void traverse_func_parallel(int s, std::set<int>& tc, size_t thread_id);

    /**
     * @brief Get the method name
     * @return Method name string
     */
    const char *method() const override;

    /**
     * @brief Reset the internal state
     */
    void reset() override;
};

#endif //_PARALLEL_TABULATION_H
