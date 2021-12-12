#include <iostream>
#include <thread>
#include <array>
#include <chrono>
#include <atomic>
#include <mutex>
 
struct Fork {
  std::mutex guard_;
 
  bool TryTake() {
    return guard_.try_lock();
  }
 
  void Release() {
    guard_.unlock();
  }
};
 
struct Philosopher {
  Fork* left_ = nullptr;
  Fork* right_ = nullptr;
  int id_ = 0;
  int eat_count_ = 0;
 
  void Bind(Fork* left, Fork* right, int id) {
    left_ = left;
    right_ = right;
    id_ = id;
  }
 
  void Simulate() {
    if (std::try_lock(left_->guard_, right_->guard_) == -1) {
      Serve();
    }
  }
 
  void Serve() {
    std::cout << "Philosopher #" << id_ << " has eaten!\n";
    ++eat_count_;
    left_->Release();
    right_->Release();
  }
 
  void Finish() const {
    std::cout << "Philosopher #" << id_ << " has eaten " << eat_count_ << " times\n";
  }
 
};
 
 
int main() {
  using namespace std::chrono_literals;
  std::array<Philosopher, 5> philosophers;
  std::array<Fork, 5> forks;
  std::atomic<bool> finished = false;
 
  std::for_each(std::begin(philosophers), std::end(philosophers),
                [&, i = 0](auto& p) mutable {
                  auto left_index = i == 0 ? forks.size() - 1 : i-1;
                  auto right_index = i == forks.size() - 1 ? 0 : i + 1;
                  p.Bind(std::addressof(forks[left_index]), std::addressof(forks[right_index]), i++);
                });
 
  std::array<std::thread, 5> workers;
 
  auto worker = [&finished](auto& phil) { return [&]() { while(!finished) { phil.Simulate(); } }; };
  std::for_each(std::begin(workers), std::end(workers),
                [&philosophers, w = std::move(worker), i = 0](auto& t) mutable { t = std::thread(w(philosophers[i++]));});
 
  std::this_thread::sleep_for(5000ms);
  finished = true;
 
  std::for_each(std::begin(workers), std::end(workers),
                [&philosophers, i = 0](auto& w) mutable {
                  w.join();
                  philosophers[i].Finish();
                  ++i;
                });
 
  return 0;
}