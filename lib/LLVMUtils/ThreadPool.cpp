
#include <llvm/Support/CommandLine.h>
#include <unistd.h>

#include "LLVMUtils/ThreadPool.h"

using namespace llvm;

// Command line option for specifying the number of worker threads.
static cl::opt<unsigned>
    NumWorkers("nworkers",
               cl::desc("Specify the number of workers to perform analysis. "
                        "Default is min(the number of hardware cores, 10)."),
               cl::value_desc("num of workers"), cl::init(0));

// Global thread pool instance.
static ThreadPool *Threads = nullptr;

/// Hook functions to run at the beginning and end of a thread
/// @{
void (*before_thread_start_hook)() = nullptr;
void (*after_thread_complete_hook)() = nullptr;
/// @}

// Returns the global thread pool instance, creating it if necessary.
ThreadPool *ThreadPool::get() {
  if (!Threads)
    Threads = new ThreadPool;
  return Threads;
}

// Constructs the thread pool and launches worker threads.
ThreadPool::ThreadPool() : IsStop(false) {
  unsigned NCores = std::thread::hardware_concurrency();
  if (NumWorkers == 0) {
    // We do not fork any threads, just use the main thread
  } else if (NumWorkers > NCores) {
    // Set default value
    NumWorkers.setValue(NCores <= 10 ? (NCores >= 2 ? NCores - 1 : 1) : 10);
  }

  for (auto &Worker : Workers) {
    ThreadLocals[Worker.get_id()] = nullptr;
  }

  NumRunningTask = 0;

  for (unsigned I = 0; I < NumWorkers.getValue(); ++I) {
    Workers.emplace_back([this] {
      if (before_thread_start_hook)
        before_thread_start_hook();

      for (;;) {
        std::function<void()> Task;

        {
          std::unique_lock<std::mutex> Lock(this->QueueMutex);
          this->Condition.wait(Lock, [this] {
            return this->IsStop || !this->TaskQueue.empty();
          });
          // If ThreadPool already stopped, return without checking
          // tasks.
          if (this->IsStop) { // && this->tasks.empty())
            if (after_thread_complete_hook)
              after_thread_complete_hook();
            return;
          }
          if (!this->TaskQueue.empty()) {
            Task = std::move(this->TaskQueue.front());
            this->TaskQueue.pop();
          }

          NumRunningTask++;
        }

        Task();

        {
          std::unique_lock<std::mutex> Lock(this->QueueMutex);
          NumRunningTask--;
        }
      }
    });
  }
}

// Waits for all tasks to complete.
void ThreadPool::wait() {
  while (true) {
    {
      std::unique_lock<std::mutex> Lock(this->QueueMutex);
      if (TaskQueue.empty() && NumRunningTask == 0) {
        break;
      }
    }
    usleep(10000);
  }
}

// Destructor joins all worker threads.
ThreadPool::~ThreadPool() {
  {
    std::unique_lock<std::mutex> Lock(QueueMutex);
    IsStop = true;
  }
  Condition.notify_all();
  for (std::thread &Worker : Workers) {
    Worker.join();
  }
}
