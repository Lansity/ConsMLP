#ifndef ZXPART_TIMER_H
#define ZXPART_TIMER_H

#include <chrono>
#include <string>

namespace consmlp {

/**
 * @brief Simple high-resolution timer
 */
class Timer {
public:
    Timer() : running_(false), elapsed_(0.0) {}
    
    /**
     * @brief Start the timer
     */
    void start() {
        running_ = true;
        start_time_ = std::chrono::high_resolution_clock::now();
    }
    
    /**
     * @brief Stop the timer and accumulate elapsed time
     */
    void stop() {
        if (running_) {
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = end_time - start_time_;
            elapsed_ += diff.count();
            running_ = false;
        }
    }
    
    /**
     * @brief Reset the timer
     */
    void reset() {
        elapsed_ = 0.0;
        running_ = false;
    }
    
    /**
     * @brief Get elapsed time in seconds
     * @return Elapsed time
     */
    double elapsed() const {
        if (running_) {
            auto current_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff = current_time - start_time_;
            return elapsed_ + diff.count();
        }
        return elapsed_;
    }
    
    /**
     * @brief Get elapsed time in milliseconds
     * @return Elapsed time in ms
     */
    double elapsedMs() const {
        return elapsed() * 1000.0;
    }

private:
    bool running_;
    double elapsed_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
};

} // namespace consmlp

#endif // ZXPART_TIMER_H

