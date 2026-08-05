import cppm.exec.threadpool;

#include <iostream>
#include <string>

int main()
{
    exec::ThreadPool pool(4);

    auto first = pool.enqueue([]() {
        return std::string("task-1 finished");
    });

    auto second = pool.enqueue([](int value) {
        return value * value;
    }, 12);

    auto third = pool.enqueue([]() {
        std::cout << "task-3 ran on the pool\n";
    });

    std::cout << first.get() << "\n";
    std::cout << "task-2 result: " << second.get() << "\n";
    third.get();

    std::cout << "workers: " << pool.size() << "\n";
    return 0;
}
