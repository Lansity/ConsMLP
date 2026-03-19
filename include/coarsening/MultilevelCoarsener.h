#ifndef ZXPART_MULTILEVEL_COARSENER_H
#define ZXPART_MULTILEVEL_COARSENER_H

#include "Coarsener.h"
#include "datastructures/HypergraphHierarchy.h"
#include "utils/Configuration.h"
#include <memory>
#include <vector>

namespace consmlp {

/**
 * @brief Manages multilevel coarsening process
 * 
 * Features:
 * - Iterative coarsening until threshold
 * - Contraction ratio control (1.5-1.7)
 * - Statistics collection
 */
class MultilevelCoarsener {
public:
    /**
     * @brief Constructor
     * @param config Configuration
     * @param coarsener Coarsening algorithm to use
     */
    MultilevelCoarsener(const Configuration& config,
                       std::unique_ptr<Coarsener> coarsener);
    
    /**
     * @brief Perform multilevel coarsening
     * @param finest_hg Finest hypergraph
     * @param silent If true, suppress progress output
     * @return Hypergraph hierarchy
     */
    HypergraphHierarchy coarsen(Hypergraph&& finest_hg, bool silent = false);
    
    /**
     * @brief Get coarsening statistics for all levels
     * @return Vector of statistics
     */
    const std::vector<CoarseningStats>& getStatistics() const {
        return stats_;
    }
    
    /**
     * @brief Print coarsening summary
     */
    void printSummary() const;

private:
    Configuration config_;
    std::unique_ptr<Coarsener> coarsener_;
    std::vector<CoarseningStats> stats_;
    
    /**
     * @brief Check if contraction ratio is acceptable
     * @param stats Coarsening statistics
     * @return True if ratio in range [1.5, 1.7]
     */
    bool isContractionRatioGood(const CoarseningStats& stats) const;
};

} // namespace consmlp

#endif // ZXPART_MULTILEVEL_COARSENER_H

