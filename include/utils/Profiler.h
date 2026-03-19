#ifndef ZXPART_PROFILER_H
#define ZXPART_PROFILER_H

#include "Timer.h"
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace consmlp {

/**
 * @brief Performance profiler for detailed timing analysis
 */
class Profiler {
public:
    /**
     * @brief Start timing a section
     * @param section_name Section name
     */
    void start(const std::string& section_name) {
        timers_[section_name].start();
    }
    
    /**
     * @brief Stop timing a section
     * @param section_name Section name
     */
    void stop(const std::string& section_name) {
        timers_[section_name].stop();
    }
    
    /**
     * @brief Get elapsed time for a section
     * @param section_name Section name
     * @return Elapsed time in seconds
     */
    double getElapsed(const std::string& section_name) const {
        auto it = timers_.find(section_name);
        if (it != timers_.end()) {
            return it->second.elapsed();
        }
        return 0.0;
    }
    
    /**
     * @brief Reset all timings
     */
    void reset() {
        timers_.clear();
    }
    
    /**
     * @brief Print profiling results
     * @param title Title for the report
     */
    void printReport(const std::string& title = "Profiling Report") const {
        std::cout << "\n========== " << title << " ==========" << std::endl;
        
        if (timers_.empty()) {
            std::cout << "No profiling data collected." << std::endl;
            return;
        }
        
        // Calculate total time
        double total_time = 0.0;
        for (const auto& pair : timers_) {
            total_time += pair.second.elapsed();
        }
        
        // Sort by elapsed time (descending)
        std::vector<std::pair<std::string, double>> sorted;
        for (const auto& pair : timers_) {
            sorted.emplace_back(pair.first, pair.second.elapsed());
        }
        std::sort(sorted.begin(), sorted.end(),
                 [](const std::pair<std::string, double>& a, 
                    const std::pair<std::string, double>& b) {
                     return a.second > b.second;
                 });
        
        std::cout << std::setw(40) << "Section"
                  << std::setw(15) << "Time (s)"
                  << std::setw(15) << "Time (ms)"
                  << std::setw(15) << "Percentage" << std::endl;
        std::cout << std::string(85, '-') << std::endl;
        
        for (const auto& pair : sorted) {
            double time_s = pair.second;
            double time_ms = time_s * 1000.0;
            double percentage = (total_time > 0) ? (100.0 * time_s / total_time) : 0.0;
            
            std::cout << std::setw(40) << pair.first
                      << std::setw(15) << std::fixed << std::setprecision(4) << time_s
                      << std::setw(15) << std::fixed << std::setprecision(2) << time_ms
                      << std::setw(14) << std::fixed << std::setprecision(1) 
                      << percentage << "%" << std::endl;
        }
        
        std::cout << std::string(85, '-') << std::endl;
        std::cout << std::setw(40) << "TOTAL"
                  << std::setw(15) << std::fixed << std::setprecision(4) << total_time
                  << std::setw(15) << std::fixed << std::setprecision(2) 
                  << (total_time * 1000.0)
                  << std::setw(15) << "100.0%" << std::endl;
        
        std::cout << "==========================================\n" << std::endl;
    }

private:
    std::map<std::string, Timer> timers_;
};

} // namespace consmlp

#endif // ZXPART_PROFILER_H

