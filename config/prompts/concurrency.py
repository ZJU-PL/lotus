data_race_detection_prompts = {
    "detection": """
You are an expert at concurrent program design and data race detection. In the following, you will be given a program. You'll need to carefully look over the program to check whether it contains data race bugs. If it contains data race bugs, please locate them in line number pairs.

The data race bug is a bug that occurs when (1) two or more threads access a shared variable at the same time, and (2) at least one of the accesses is a write. Note that, two operations **cannot** execute at the same time when (1) both are atomical operations, (2) both are protected by the same mutex, (3) they are guarded by a semaphare which ensures the exclusive access of the shared variable, or (4) other mechanism that forbids the two operations to execute at the same time.

The program can use `__VERIFIER_atomic_begin()` and `__VERIFIER_atomic_end()` to mark the start and the end of an atomic zone. Besides, if the function name has the `__VERIFIER_atomic` prefix, the corresponding function should also be regarded as an atomic zone. All operations inside the atomic zone should be regarded as atomic.

The program can use `pthread_mutex_lock(&m)` and `pthread_mutex_unlock(&m)` to lock and unlock a mutex `m`.

The program can use `sem_wait()` and `sem_post()` to control semaphores; they do not lock or unlock mutexes. A semaphore holds an integer value. The `sem_wait()` is used to decrease the semaphore's value (typically by 1) to signal that the program wants to enter a critical section or use a resource. If the semaphore's value is greater than 0, `sem_wait()` decrements it and then proceeds. If the semaphore's value is 0, `sem_wait()` is blocked until the semaphore's value becomes greater than 0. The `sem_post` is used to increment the semaphore's value (typically by 1), indicating that a resource has been released.

The program can use `pthread_create()` to create a new thread and use `pthread_join()` to join the created thread. All the operations inside the new thread should happen after the `pthread_create()` site and before the `pthread_join()` site.

The program can use `pthread_cond_wait()` and `pthread_cond_signal()` to wait and signal a condition variable. It can also use `pthread_barrier_wait()` to wait for a barrier.

The program also uses `assume_abort_if_not()` as `assert()`. It can use `__VERIFIER_nondet_int()` to get a random integer. Besides, the indices of the lines are provided at the beginning of each line, e.g., "1:", to help locate the line numbers.

You can follow the following steps to detect the data race bugs:
1. Read the program carefully and understand how the threads are created and joined.
2. Check the shared variables and their accesses.
3. Check the synchronization mechanisms (atomic zones, mutexes, semaphores, condition variables, etc.) and their usage.
4. For each pair of accesses to the same shared variable, check whether they can constitute a data race.

After thoroughly checking all potential data race bugs, please output the all the confirmed data races. If no data race is found, please answer an empty list. Please answer in the following JSON format (each race as one dict):

```json
{
  "races": [
    {
      "shared_variable": "the name of the same shared variable",
      "lineA": the line number of the first access in `int` format,
      "lineB": the line number of the second access in `int` format
    },
    ...
  ]
}
```
"""
}