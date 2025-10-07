#ifndef SUPPORT_THREADPOOL_H
#define SUPPORT_THREADPOOL_H

#include <llvm/Support/ManagedStatic.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "Support/ADT/MapIterators.h"

class ThreadPool {
private:
    ThreadPool();

public:
    ~ThreadPool();

    /// add new work item to the pool
    template<class F, class... Args>
    auto enqueue(F &&, Args &&...) -> std::future<typename std::result_of<F(Args...)>::type>;

    /// Wait until no tasks remain
    void wait();

    /// each thread is allowed to deaclare a thread local
    /// if you want to decalre more, you can pack them into a struct
    /// you need manually call deinitThreadLocal to delete the
    /// thread locals
    template<class LocalTy>
    void initThreadLocal() {
        // Add main thread id
        auto Id = std::this_thread::get_id();
        if (ThreadLocals.find(Id) == ThreadLocals.end()) {
            ThreadLocals[Id] = new LocalTy;
        }

        for (auto &Worker: Workers) {
            if (ThreadLocals[Worker.get_id()]) {
                llvm_unreachable("thread local already declared");
            }
            ThreadLocals[Worker.get_id()] = new LocalTy;
        }
    }

    template<class LocalTy>
    void deinitThreadLocal() {
        for (auto &It: ThreadLocals) {
            delete (LocalTy *) It.second;
            It.second = nullptr;
        }
    }

    template<class LocalTy>
    LocalTy *getThreadLocal() const {
        auto It = ThreadLocals.find(std::this_thread::get_id());
        assert(It != ThreadLocals.end());
        return (LocalTy *) It->second;
    }

    value_iterator<std::map<std::thread::id, void *>::iterator>
    threadLocalsBegin() {
        return {ThreadLocals.begin()};
    }

    value_iterator<std::map<std::thread::id, void *>::iterator>
    threadLocalsEnd() {
        return {ThreadLocals.end()};
    }

    /// we need to keep track of threads so we can join them recording the
    /// workers of the thread pool
    std::vector<std::thread> Workers;

    /// the task queue containing tasks
    std::queue<std::function<void()>> TaskQueue;

    std::mutex QueueMutex;             ///< The lock
    std::condition_variable Condition; ///< the wait cond

    bool IsStop; ///< identifying if the thread pool is running

    int NumRunningTask; /// < number of running task

    std::map<std::thread::id, void *> ThreadLocals;

public:
    static ThreadPool *get();
};


template<class F, class... Args>
auto ThreadPool::enqueue(F &&Func, Args &&... Arguments) -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type; // The return type

    auto Task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(Func), std::forward<Args>(Arguments)...));
    std::future<return_type> Res = Task->get_future();

    if (Workers.empty()) {
        (*Task)();
        return Res;
    }

    {
        std::unique_lock<std::mutex> Lock(QueueMutex); // acquiring lock

        // don't allow to enqueue after stopping the pool
        if (IsStop)
            llvm_unreachable("enqueue on stopped ThreadPool");

        TaskQueue.emplace([Task]() { (*Task)(); });
    }
    Condition.notify_one();
    return Res;
}

#endif
