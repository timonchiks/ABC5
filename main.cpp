#include <thread>
#include <iostream>
#include <mutex>
#include <array>
#include <chrono>
#include <queue>
#include <random>
#include <condition_variable>
#include <atomic>

template<class OStream>
class SynchronizedOut {
    OStream &out_;
    std::mutex io_mutex_;

public:
    SynchronizedOut(OStream &os)
            : out_(os) {}

    template<typename... Args>
    void Log(Args &&... args) {
        std::unique_lock<std::mutex> lock{io_mutex_};
        (out_ << ... << std::forward<Args>(args));
    }
};

static SynchronizedOut sync_logger{std::cout};
constexpr auto sync_log = [](auto &&... args) {
    sync_logger.Log(std::forward<decltype(args)>(args)...);
};

template<int Min, int Max>
struct RNGSettings {
    std::uniform_int_distribution<> distribution_{Min, Max};

    template<typename RNG>
    int Next(RNG &engine) {
        return distribution_(engine);
    }
};

using BeeHuntSettings = RNGSettings<800, 1200>;
using BeeReleaseSettings = RNGSettings<50, 100>;

using namespace std::literals;  // NOLINT

struct Bee {
    std::mutex bee_mutex_;
    bool at_home_ = true;
    std::chrono::milliseconds time_to_hunt_;
    std::condition_variable condition_;
    struct Hive *owner_;
    std::thread this_thread_;
    int id_;

    bool stop_signal_ = false;

    Bee(Hive *owner, int id)
            : owner_(owner), id_(id) {}

    Bee(Bee &&other)
            : at_home_(other.at_home_), time_to_hunt_(other.time_to_hunt_), owner_(other.owner_),
              this_thread_(std::move(other.this_thread_)), id_(other.id_) {}

    void Start() {
        this_thread_ = std::thread([this]() { Run(); });
    }

    void Hunt(std::chrono::milliseconds time) {
        {
            std::unique_lock<std::mutex> lock{bee_mutex_};
            at_home_ = false;
            time_to_hunt_ = time;
        }
        condition_.notify_one();
    }

    void End() {
        stop_signal_ = true;
        condition_.notify_all();
    }

    void Finish() {
        if (this_thread_.joinable()) {
            this_thread_.join();
        }
    }

    void Run();
};

struct Hive {
    BeeHuntSettings bee_hunting_time_;
    BeeReleaseSettings bee_release_time_;
    static constexpr int kMaxHoneyCount = 30;

    std::vector<Bee> all_bees_;
    std::mutex queue_mutex_;
    std::queue<Bee *> bees_currently_in_hive_;
    std::condition_variable bee_count_condition_;
    std::condition_variable honey_count_condition_;
    std::atomic<int> honey_count_ = 0;
    std::mt19937 rng_;
    std::mutex hive_mutex_;
    std::thread this_thread_;

    bool stop_signal_ = false;

    Hive(int num_bees) {
        all_bees_.reserve(num_bees);
        for (int i = 0; i < num_bees; ++i) {
            auto &bee = all_bees_.emplace_back(this, i);
            bees_currently_in_hive_.push(std::addressof(bee));
        }
    }

    ~Hive() {
        for (auto &bee: all_bees_) {
            bee.Finish();
        }
        if (this_thread_.joinable()) {
            this_thread_.join();
        }
    }

    void Start() {
        for (auto &b: all_bees_) {
            b.Start();
        }
        this_thread_ = std::thread([this]() { Run(); });
    }

    int Size() {
        // We need this mutex only here because push and pop are guarded by hive_mutex_
        std::unique_lock<std::mutex> lock{queue_mutex_};
        return bees_currently_in_hive_.size();
    }

    void ReleaseOne() {
        Bee *next = [this]() {
            std::unique_lock<std::mutex> lock{hive_mutex_};
            Bee *next = bees_currently_in_hive_.front();
            bees_currently_in_hive_.pop();
            return next;
        }();

        int release_ms = bee_hunting_time_.Next(rng_);
        sync_log("Bee ", next->id_, " is going for a hunt for ", release_ms, "ms. Current bee count: ", Size(), "\n");
        next->Hunt(std::chrono::milliseconds{release_ms});
    }

    void ReturnOne(Bee *bee) {
        {
            std::unique_lock<std::mutex> lock{hive_mutex_};
            bees_currently_in_hive_.push(bee);
            if (honey_count_ < kMaxHoneyCount) {
                ++honey_count_;
            }
            sync_log("Bee ", bee->id_, " returned from a hunt. Current honey: ", honey_count_, "\n");
        }
        bee_count_condition_.notify_one();
        honey_count_condition_.notify_one();
    }

    bool TryAttack() {
        if (Size() < 3) {
            honey_count_ = 0;
            return true;
        } else {
            return false;
        }
    }

    void Run() {
        while (!stop_signal_) {
            std::unique_lock<std::mutex> lock{hive_mutex_};
            if (Size() <= 1) {
                bee_count_condition_.wait(lock, [this]() { return Size() > 1 || stop_signal_; });
            }

            if (stop_signal_) {
                sync_log("Shutting down hive\n");
                return;
            }

            lock.unlock();
            ReleaseOne();

            std::this_thread::sleep_for(std::chrono::milliseconds{bee_release_time_.Next(rng_)});
        }
        sync_log("Shutting down hive\n");
    }

    void End() {
        stop_signal_ = true;
        for (auto &bee: all_bees_) {
            bee.End();
        }
        bee_count_condition_.notify_all();
        honey_count_condition_.notify_all();
    }
};

void Bee::Run() {
    while (!stop_signal_) {
        std::unique_lock<std::mutex> lock{bee_mutex_};
        condition_.wait(lock, [this] { return !at_home_ || stop_signal_; });

        if (stop_signal_) {
            sync_log("Shutting down bee #", id_, "\n");
            return;
        }

        std::this_thread::sleep_for(time_to_hunt_);
        at_home_ = true;
        owner_->ReturnOne(this);
    }
    sync_log("Shutting down bee #", id_, "\n");
}

struct Winnie {
    Hive *hive_;
    std::thread this_thread_;

    bool stop_signal_ = false;

    static constexpr int kCureTime = 2000;

    Winnie(Hive *hive)
            : hive_(hive) {}

    ~Winnie() {
        if (this_thread_.joinable()) {
            this_thread_.join();
        }
    }

    void Start() {
        this_thread_ = std::thread([this]() { Run(); });
    }

    bool Attack() {
        sync_log("Winnie is trying to attack the hive. Hive bee count is: ", hive_->Size(), "\n");
        return hive_->TryAttack();
    }

    void Cure() {
        sync_log("Winnie is curing himself :(\n");
        std::this_thread::sleep_for(std::chrono::milliseconds{kCureTime});
        sync_log("Winnie is healthy now\n");
    }

    void Run() {
        while (!stop_signal_) {
            std::unique_lock<std::mutex> lock{hive_->hive_mutex_};
            hive_->honey_count_condition_.wait(lock, [this]() { return hive_->honey_count_ >= 15 || stop_signal_; });

            if (stop_signal_) {
                sync_log("Shutting down Winnie the pooh\n");
                return;
            }

            if (Attack()) {
                sync_log("Winnie succesfully attacked the hive and ate all honey\n");
                continue;
            } else {
                lock.unlock();
                Cure();
            }
        }
        sync_log("Shutting down Winnie the pooh\n");
    }

    void End() {
        stop_signal_ = true;
    }
};

class App {
    Hive hive_;
    Winnie winnie_;

public:
    App(int max_bee_count)
            : hive_(max_bee_count), winnie_(&hive_) {}

    void Start() {
        hive_.Start();
        winnie_.Start();
    }

    void End() {
        sync_log("Shutting down the application\n");
        hive_.End();
        winnie_.End();
    }
};

int main() {
    App app{10};
    app.Start();
    std::this_thread::sleep_for(15s);
    app.End();
    return 0;
}