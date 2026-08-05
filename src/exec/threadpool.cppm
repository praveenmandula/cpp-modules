module;

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

export module cppm.exec.threadpool;

export namespace exec
{
class ThreadPool
{
public:
    explicit ThreadPool(std::size_t threadCount = std::thread::hardware_concurrency())
    {
        if (threadCount == 0)
            threadCount = 1;

        start(threadCount);
    }

    ~ThreadPool()
    {
        stop();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    [[nodiscard]] std::size_t size() const
    {
        return mWorkers.size();
    }

    [[nodiscard]] bool isRunning() const
    {
        return mRunning;
    }

    template<typename F, typename... Args>
    auto enqueue(F&& task, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ResultType = std::invoke_result_t<F, Args...>;

        auto boundTask = std::bind(std::forward<F>(task), std::forward<Args>(args)...);
        auto sharedTask = std::make_shared<std::packaged_task<ResultType()>>(std::move(boundTask));
        std::future<ResultType> result = sharedTask->get_future();

        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning)
                throw std::runtime_error("ThreadPool is not running");

            mTasks.emplace([sharedTask]() {
                (*sharedTask)();
            });
        }

        mCondition.notify_one();
        return result;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning)
                return;

            mRunning = false;
        }

        mCondition.notify_all();

        for (auto& worker : mWorkers)
        {
            if (worker.joinable())
                worker.join();
        }

        mWorkers.clear();
    }

private:
    void start(std::size_t threadCount)
    {
        mRunning = true;
        mWorkers.reserve(threadCount);

        for (std::size_t i = 0; i < threadCount; ++i)
        {
            mWorkers.emplace_back([this]() {
                workerLoop();
            });
        }
    }

    void workerLoop()
    {
        while (true)
        {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mMutex);
                mCondition.wait(lock, [this]() {
                    return !mRunning || !mTasks.empty();
                });

                if (!mRunning && mTasks.empty())
                    return;

                task = std::move(mTasks.front());
                mTasks.pop();
            }

            task();
        }
    }

private:
    std::vector<std::thread> mWorkers;
    std::queue<std::function<void()>> mTasks;
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mRunning = false;
};

}
