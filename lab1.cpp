#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// Несериализуемые данные: объект передаём по указателю
struct Payload {
    int id;
    // Здесь может быть что угодно несериализуемое/некопируемое: сокеты, mutex, file handle и т.п.
    explicit Payload(int id_) : id(id_) {}
};

// Монитор: односвязный буфер (slot) + CV для "пусто/полно"
class EventMonitor {
public:
    void put(std::unique_ptr<Payload> p) {
        std::unique_lock<std::mutex> lk(m_);

        // Ждём пока слот освободится (без активного ожидания)
        cv_empty_.wait(lk, [&] { return stopped_ || !slot_.has_value(); });
        if (stopped_) return;

        slot_ = std::move(p);

        // Сообщение об отправке (печать под тем же замком => порядок не ломается)
        std::cout << "Producer: event " << (*slot_)->id << " sent\n" << std::flush;

        // Разбудить потребителя
        cv_full_.notify_one();
    }

    // Возвращает nullptr, когда остановлено и событий больше нет
    std::unique_ptr<Payload> get() {
        std::unique_lock<std::mutex> lk(m_);

        // Ждём пока появится событие
        cv_full_.wait(lk, [&] { return stopped_ || slot_.has_value(); });

        if (!slot_.has_value()) {
            // stopped_ == true и слот пуст => завершаемся
            return nullptr;
        }

        auto p = std::move(*slot_);
        slot_.reset();

        std::cout << "Consumer: event " << p->id << " handled\n" << std::flush;

        // Разбудить поставщика (слот снова пуст)
        cv_empty_.notify_one();
        return p;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(m_);
        stopped_ = true;
        cv_full_.notify_all();
        cv_empty_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_full_;
    std::condition_variable cv_empty_;
    std::optional<std::unique_ptr<Payload>> slot_; // 0 или 1 событие
    bool stopped_ = false;
};

int main() {
    using namespace std::chrono_literals;

    EventMonitor mon;
    constexpr int N = 10;

    std::thread producer([&] {
        for (int i = 1; i <= N; ++i) {
            std::this_thread::sleep_for(1s);
            mon.put(std::make_unique<Payload>(i));
        }
        mon.stop(); // сигнал потребителю: больше событий не будет
    });

    std::thread consumer([&] {
        while (auto ev = mon.get()) {
            // Здесь могла бы быть обработка ev (владелец — потребитель)
            // Например: использовать ev->..., затем объект удалится автоматически.
        }
    });

    producer.join();
    consumer.join();
    return 0;
}