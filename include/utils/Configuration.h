#ifndef CONSMLP_CONFIGURATION_H
#define CONSMLP_CONFIGURATION_H

#include "Types.h"

namespace consmlp {

/**
 * @brief Configuration parameters for hypergraph partitioning
 */
struct Configuration {
    // Partitioning parameters
    PartitionID num_partitions;     // Number of partitions (k-way partitioning)
    double imbalance_factor;        // Allowed imbalance factor (e.g., 0.03 for 3%)
    
    // Coarsening parameters
    uint32_t coarsening_threshold;  // Stop coarsening when #nodes < threshold
    double contraction_limit;       // Contraction limit ratio
    bool coarsen_opt;               // Enable coarsening optimization (skip large nets when small net found)
    
    // Initial partitioning parameters
    uint32_t initial_partition_runs; // Number of initial partition runs
    
    // Refinement parameters
    uint32_t max_refinement_passes;  // Maximum refinement passes
    
    // Random seed
    uint32_t seed;

    // total node weight
    Weight total_node_weight;

    // large net threshold
    uint64_t large_net_threshold;

    //trial refine levels
    int trial_refine_levels;
    
    // Default constructor with default values
    Configuration()
        : num_partitions(2)
        , imbalance_factor(0.03)
        , coarsening_threshold(100)
        , contraction_limit(0.5)
        , coarsen_opt(false)
        , initial_partition_runs(10)
        , max_refinement_passes(10)
        , seed(0)
        , total_node_weight(0)
        , large_net_threshold(500)
        , trial_refine_levels(2)
    {}
};

} // namespace consmlp

#endif // CONSMLP_CONFIGURATION_H

