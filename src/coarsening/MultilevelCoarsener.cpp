#include "coarsening/MultilevelCoarsener.h"
#include <iostream>
#include <iomanip>

namespace consmlp {

MultilevelCoarsener::MultilevelCoarsener(const Configuration& config,
                                         std::unique_ptr<Coarsener> coarsener)
    : config_(config)
    , coarsener_(std::move(coarsener))
{
}

HypergraphHierarchy MultilevelCoarsener::coarsen(Hypergraph&& finest_hg, bool silent) {
    stats_.clear();
    
    // Create hierarchy with finest level
    HypergraphHierarchy hierarchy(std::move(finest_hg));
    
    uint32_t level = 0;
    
    if (!silent) {
        std::cout << "Starting multilevel coarsening..." << std::endl;
    }
    
    // Iteratively coarsen
    while (true) {
        const Hypergraph& current_hg = hierarchy.getLevel(level).getHypergraph();
        NodeID num_nodes = current_hg.getNumNodes();
        
        // Check stopping criterion
        if (coarsener_->shouldStopCoarsening(num_nodes)) {
            break;
        }
        
        // Perform coarsening
        CoarseningStats stats = coarsener_->coarsen(hierarchy, level, nullptr);
        stats_.push_back(stats);
        
        if (stats.coarse_nodes < 1000) {
            if (stats.coarse_nodes < 100 || stats.coarse_nets < 200) {
                break;
            }
            int valued_nodes = 0;
            auto coarse_hg = hierarchy.getLevel(level + 1).getHypergraph();
            for (NodeID node = 0; node < stats.coarse_nodes; ++node) {
                if (coarse_hg.getNodeWeight(node) > 0 && coarse_hg.getNodeType(node) != NodeType::IO && !coarse_hg.isNodeIgnored(node) && !coarse_hg.isNodeFixed(node)) {
                    valued_nodes++;
                }
            }
            if (valued_nodes < 80) {
                break;
            }
        }
        if (stats.contraction_ratio < 1.1) {
            break;
        }
        
        level++;
        
        // Safety limit on number of levels
        if (level >= 40) {
            std::cout << "Stopping: Maximum levels reached" << std::endl;
            break;
        }
    }
    
    return hierarchy;
}

bool MultilevelCoarsener::isContractionRatioGood(const CoarseningStats& stats) const {
    return stats.contraction_ratio >= 1.5 && stats.contraction_ratio <= 1.7;
}

void MultilevelCoarsener::printSummary() const {
    std::cout << "\n========== Coarsening Summary ==========" << std::endl;
    std::cout << "Number of levels: " << (stats_.size() + 1) << std::endl;
    
    if (stats_.empty()) {
        std::cout << "No coarsening performed" << std::endl;
        return;
    }
    
    std::cout << "\nLevel-by-level statistics:" << std::endl;
    std::cout << std::setw(6) << "Level"
              << std::setw(10) << "Nodes"
              << std::setw(10) << "Nets"
              << std::setw(12) << "Ratio"
              << std::setw(10) << "Matched"
              << std::setw(12) << "Singletons" << std::endl;
    std::cout << std::string(60, '-') << std::endl;
    
    for (size_t i = 0; i < stats_.size(); ++i) {
        const auto& s = stats_[i];
        std::cout << std::setw(6) << i
                  << std::setw(10) << s.original_nodes
                  << std::setw(10) << s.original_nets
                  << std::setw(12) << std::fixed << std::setprecision(2) 
                  << s.contraction_ratio
                  << std::setw(10) << s.num_matched_pairs
                  << std::setw(12) << s.num_singletons << std::endl;
    }
    
    // Final level
    const auto& last = stats_.back();
    std::cout << std::setw(6) << stats_.size()
              << std::setw(10) << last.coarse_nodes
              << std::setw(10) << last.coarse_nets
              << std::setw(12) << "-"
              << std::setw(10) << "-"
              << std::setw(12) << "-" << std::endl;
    
    // Summary statistics
    double total_ratio = static_cast<double>(stats_[0].original_nodes) /
                        stats_.back().coarse_nodes;
    
    std::cout << "\nOverall statistics:" << std::endl;
    std::cout << "  Initial nodes: " << stats_[0].original_nodes << std::endl;
    std::cout << "  Final nodes:   " << last.coarse_nodes << std::endl;
    std::cout << "  Total reduction: " << std::fixed << std::setprecision(1)
              << (100.0 * (1.0 - 1.0/total_ratio)) << "%" << std::endl;
    std::cout << "  Overall ratio:   " << std::fixed << std::setprecision(2)
              << total_ratio << std::endl;
    
    std::cout << "========================================\n" << std::endl;
}

} // namespace consmlp

