#ifndef ZXPART_MULTILEVEL_PARTITIONER_H
#define ZXPART_MULTILEVEL_PARTITIONER_H

#include "utils/HgrParser.h"
#include "utils/Timer.h"
#include "utils/Configuration.h"
#include "datastructures/Hypergraph.h"
#include "datastructures/HypergraphHierarchy.h"
#include "coarsening/Coarsener.h"
#include "coarsening/MultilevelCoarsener.h"
#include "partitioning/Partition.h"
#include "partitioning/PartitionConstraints.h"
#include "partitioning/PartitionMetrics.h"
#include "partitioning/Partitioner.h"
#include "refinement/Refiner.h"

#include <string>
#include <memory>
#include <mutex>

namespace consmlp {

// Statistics for bipartition
struct BipartitionStats {
    int depth = 0;
    PartitionID part_start = 0;
    PartitionID part_end = 0;
    NodeID num_nodes = 0;
    EdgeID num_nets = 0;
    int num_levels = 0;
    double coarsen_time = 0.0;
    double initial_time = 0.0;
    double refine_time = 0.0;
    Weight cut_size = 0;
};

/**
 * @brief Application class for multilevel hypergraph partitioning
 * 
 * Supports three partitioning modes:
 * 1. Direct bipartition (k=2, mode=direct)
 * 2. Direct k-way partitioning (k>2, mode=direct)
 * 3. Recursive bipartition (k=2^n, mode=recursive)
 */
class MultilevelPartitionerApp {
public:
    MultilevelPartitionerApp() = default;
    ~MultilevelPartitionerApp() = default;
    
    /**
     * @brief Parse command line arguments and configure the application
     */
    bool parseArguments(int argc, char* argv[]);
    
    /**
     * @brief Run the multilevel partitioning process
     */
    int run();
    
    // Getters for configuration
    const Configuration& getConfig() const { return config_; }
    const std::string& getCoarsenAlgo() const { return coarsen_algo_; }
    const std::string& getRefineAlgo() const { return refine_algo_; }
    const std::string& getInitMode() const { return init_mode_; }
    bool useTypeConstraints() const { return use_type_constraints_; }
    bool useXMLConstraints() const { return use_xml_constraints_; }
    const std::string& getXMLFile() const { return xml_file_; }
    double getRelaxedMultiplier() const { return relaxed_multiplier_; }
    
private:
    // Configuration
    std::string input_file_;
    std::string output_file_;
    std::string type_file_;
    std::string xml_file_;  // XML constraint file path
    std::string coarsen_algo_;
    std::string refine_algo_;
    std::string partition_mode_;
    std::string init_mode_;
    double relaxed_multiplier_ = 3.0;
    Configuration config_;
    
    // Flags
    bool use_type_constraints_ = false;
    bool use_xml_constraints_ = false;  // Using XML-based absolute capacity constraints
    bool use_recursive_ = false;
    
    // Cached XML constraints (parsed once, reused at all levels)
    std::unique_ptr<PartitionConstraints> xml_constraints_;
    
    // Helper methods
    void setLargeNetThreshold(Hypergraph& hg);
    void printConfiguration() const;
    Hypergraph parseHypergraph();
    
    // Main partitioning methods
    int runRecursivePartitioning(Hypergraph& hg);
    int runDirectPartitioning(Hypergraph& hg, double parse_time);
    
    // Initial partitioning
    Partition runInitialPartitioning(const HypergraphHierarchy& hierarchy,
                                    const int coarsest_level,
                                    const PartitionConstraints& coarsest_constraints,
                                    bool use_type_constraints,
                                    double relaxed_multiplier,
                                    const std::string& refine_algo,
                                    const std::string& init_mode,
                                    int& best_trial_level);
};

// Utility functions
bool isPowerOfTwo(PartitionID value);

std::unique_ptr<Coarsener> createCoarsener(const std::string& algo,
                                           const Configuration& config);

std::unique_ptr<Refiner> createRefiner(const std::string& algo,
                                       const Configuration& config);

Hypergraph buildSubHypergraph(const Hypergraph& parent,
                              const std::vector<NodeID>& subset);

// Direct bipartition with statistics
Partition runDirectBipartition(Hypergraph&& hg,
                               const Configuration& config,
                               const std::string& coarsen_algo,
                               const std::string& refine_algo,
                               const std::string& init_mode,
                               bool use_type_constraints,
                               double relaxed_multiplier,
                               BipartitionStats& stats);

// Recursive bipartition using direct flow
void recursiveBipartitionDirect(const Hypergraph& original_hg,
                                const std::vector<NodeID>& subset,
                                PartitionID base_partition_id,
                                PartitionID target_parts,
                                const Configuration& base_config,
                                const std::string& coarsen_algo,
                                const std::string& refine_algo,
                                const std::string& init_mode,
                                bool use_type_constraints,
                                double relaxed_multiplier,
                                Partition& final_partition,
                                std::mutex& partition_mutex,
                                std::mutex& print_mutex,
                                int depth);

// Print functions
void printUsage(const char* prog_name);
void printHypergraphInfo(const Hypergraph& hg, bool use_types = false);
void printCoarseningResults(const MultilevelCoarsener& coarsener, 
                            const HypergraphHierarchy& hierarchy,
                            double time_s);
void printRefinementResults(const std::vector<RefinementStats>& stats, 
                           double time_s);
void printFinalResults(const Hypergraph& hg, const Partition& partition,
                      const Configuration& config);

} // namespace consmlp

#endif // ZXPART_MULTILEVEL_PARTITIONER_H
