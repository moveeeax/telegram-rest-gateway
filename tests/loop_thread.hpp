#pragma once

#include <trantor/net/EventLoop.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace tgw_test {

// trantor::EventLoop на отдельном потоке для тестов, общий для всех тестов с петлей.
// Старт: поток создает EventLoop (eventfd создается на ЕГО потоке) и публикует указатель под
// мьютексом + cv; тест-тред постит в петлю только после этого, поэтому создание eventfd
// happens-before любого queueInLoop.
// Останов: quit() исполняется на потоке петли (через queueInLoop), но EventLoop разрушается
// только после того, как деструктор вышел из queueInLoop. Иначе петля успевала выйти и закрыть
// eventfd, пока тест-тред еще писал в него в wakeup() (гонка close/write, видна TSan при
// инструментированном trantor).
class LoopThread {
   public:
    LoopThread() {
        thread_ = std::thread([this] {
            trantor::EventLoop loop;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                loop_ = &loop;
            }
            cv_.notify_all();
            loop.loop();  // крутится до quit()
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_posted_; });
            // EventLoop разрушается здесь, на своем потоке, когда в него уже никто не пишет.
        });
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return loop_ != nullptr; });
    }

    ~LoopThread() {
        loop_->queueInLoop([loop = loop_] { loop->quit(); });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_posted_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    // loop_ после конструктора больше не меняется (запись под мьютексом до cv.wait), поэтому
    // читать его без лока безопасно.
    trantor::EventLoop* loop() const { return loop_; }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    trantor::EventLoop* loop_ = nullptr;
    bool stop_posted_ = false;
    std::thread thread_;
};

}  // namespace tgw_test
