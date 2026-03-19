#include "partitioning/MultilevelPartitioner.h"

#include <iostream>
#include <iomanip>
#include <cstring>
#include <limits>
#include <cctype>
#include <fstream>
#include <numeric>
#include <thread>
#include <future>
#include <atomic>
#include <unordered_map>
#include <algorithm>

namespace consmlp {

// ========== Utility Functions ==========

bool isPowerOfTwo(PartitionID value) {
    return value > 0 && (value & (value - 1)) == 0;
}

std::unique_ptr<Coarsener> createCoarsener(const std::string& algo,
                                           const Configuration& config) {
    if (algo == "first") {
        return std::unique_ptr<Coarsener>(new FirstChoiceMatching(config));
    } else if (algo == "cluster") {
        return std::unique_ptr<Coarsener>(new ClusterMatching(config));
    }
    return std::unique_ptr<Coarsener>(new HeavyEdgeMatching(config));
}

std::unique_ptr<Refiner> createRefiner(const std::string& algo,
                                       const Configuration& config) {
    if (algo == "greedy") {
        return std::unique_ptr<Refiner>(new GreedyRefiner(config));
    } else if (algo == "greedyfm" || algo == "gfm") {
        return std::unique_ptr<Refiner>(new GreedyFMRefiner(config));
    }
    return std::unique_ptr<Refiner>(new FMRefiner(config));
}

Hypergraph buildSubHypergraph(const Hypergraph& parent,
                              const std::vector<NodeID>& subset) {
    Hypergraph sub(subset.size(), parent.getNumNets());
    std::unordered_map<NodeID, NodeID> old_to_new;
    old_to_new.reserve(subset.size() * 2);
    
    for (size_t i = 0; i < subset.size(); ++i) {
        NodeID old_id = subset[i];
        old_to_new[old_id] = static_cast<NodeID>(i);
        NodeID new_id = sub.addNode(parent.getNodeType(old_id),
                                    parent.getNodeWeight(old_id));
        sub.setNodeIgnored(new_id, parent.isNodeIgnored(old_id));
        sub.setNodeFixed(new_id, parent.isNodeFixed(old_id));
    }
    
    for (EdgeID net_id = 0; net_id < parent.getNumNets(); ++net_id) {
        auto nodes = parent.getNetNodes(net_id);
        std::vector<NodeID> mapped_nodes;
        mapped_nodes.reserve(nodes.second - nodes.first);
        
        for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
            auto found = old_to_new.find(*it);
            if (found != old_to_new.end()) {
                mapped_nodes.push_back(found->second);
            }
        }
        
        if (mapped_nodes.size() <= 1) {
            continue;
        }
        
        EdgeID new_net = sub.addNet(parent.getNetWeight(net_id),
                                    parent.isNetGlobal(net_id));
        for (NodeID mapped_node : mapped_nodes) {
            sub.addNodeToNet(new_net, mapped_node);
        }
    }
    
    sub.finalize();
    return sub;
}

// ========== Direct Bipartition ==========

Partition runDirectBipartition(Hypergraph&& hg,
                               const Configuration& config,
                               const std::string& coarsen_algo,
                               const std::string& refine_algo,
                               const std::string& init_mode,
                               bool use_type_constraints,
                               double relaxed_multiplier,
                               const PartitionConstraints* xml_constraints,
                               int debug_depth,
                               PartitionID debug_part_start,
                               PartitionID debug_part_end,
                               BipartitionStats& stats) {
    Configuration local_config = config;
    local_config.num_partitions = 2;  // Force bipartition
    
    stats.num_nodes = hg.getNumNodes();
    stats.num_nets = hg.getNumNets();
    
    // Coarsening phase
    Timer coarsen_timer;
    coarsen_timer.start();
    
    auto coarsener = createCoarsener(coarsen_algo, local_config);
    MultilevelCoarsener ml_coarsener(local_config, std::move(coarsener));
    // Silent mode for recursive bipartition (output is collected and printed at the end)
    HypergraphHierarchy hierarchy = ml_coarsener.coarsen(std::move(hg), true);
    
    coarsen_timer.stop();
    stats.coarsen_time = coarsen_timer.elapsed();
    stats.num_levels = hierarchy.getNumLevels();
    
    // Initial partitioning phase
    Timer initial_timer;
    initial_timer.start();
    
    const Hypergraph& coarsest_hg = hierarchy.getCoarsestLevel().getHypergraph();
    int coarsest_level = hierarchy.getNumLevels() - 1;
    int num_trial_refine_levels = std::min(local_config.trial_refine_levels, static_cast<int>(hierarchy.getNumLevels()));
    int trial_stop_level = std::max(0, coarsest_level - num_trial_refine_levels);
    const Hypergraph& eval_hg = hierarchy.getLevel(trial_stop_level).getHypergraph();
    
    PartitionConstraints coarsest_constraints(2, local_config);
    if (xml_constraints) {
        // Use XML constraints directly (already aggregated for this bipartition)
        coarsest_constraints = *xml_constraints;
    } else if (use_type_constraints) {
        coarsest_constraints.initializeBalancedWithTypes(coarsest_hg, local_config.imbalance_factor, relaxed_multiplier);
    } else {
        coarsest_constraints.initializeBalanced(coarsest_hg, local_config.imbalance_factor);
    }
    
    // Debug: print constraints for this bipartition
    if (xml_constraints) {
        std::cout << "[DEBUG Bipartition] depth=" << debug_depth 
                  << " parts=[" << debug_part_start << "," << debug_part_end << "] XML constraints:" << std::endl;
        for (int p = 0; p < 2; ++p) {
            std::cout << "  Part" << p << ": ";
            for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
                CapacityConstraint c = coarsest_constraints.getCapacity(p, static_cast<NodeType>(t));
                if (c.max_capacity != std::numeric_limits<Weight>::max()) {
                    std::cout << nodeTypeToString(static_cast<NodeType>(t)) << "=" << c.max_capacity << " ";
                }
            }
            std::cout << std::endl;
        }
    }
    
    // Pre-create level constraints
    std::vector<PartitionConstraints> level_constraints_cache;
    level_constraints_cache.reserve(num_trial_refine_levels + 1);
    for (int level = coarsest_level; level >= trial_stop_level; --level) {
        const Hypergraph& level_hg = hierarchy.getLevel(level).getHypergraph();
        PartitionConstraints constraints(2, local_config);
        if (xml_constraints) {
            constraints = *xml_constraints;
        } else if (use_type_constraints) {
            constraints.initializeBalancedWithTypes(level_hg, local_config.imbalance_factor, relaxed_multiplier);
        } else {
            constraints.initializeBalanced(level_hg, local_config.imbalance_factor);
        }
        level_constraints_cache.push_back(std::move(constraints));
    }
    
    Partition best_partition(eval_hg.getNumNodes(), 2);
    Weight best_cut = std::numeric_limits<Weight>::max();
    int best_trial_level = trial_stop_level;
    
    // Track best imbalanced partition as fallback (when constraints cannot be satisfied)
    Partition best_imbalanced_partition(eval_hg.getNumNodes(), 2);
    Weight best_imbalanced_cut = std::numeric_limits<Weight>::max();
    bool has_valid_partition = false;
    
    // Create FM refiner for initial partition (more aggressive, multi-pass)
    // Use FM instead of GreedyFM because coarsest level is small
    Configuration init_refine_config = local_config;
    init_refine_config.max_refinement_passes = 20;  // More passes for better quality
    auto trial_refiner = std::unique_ptr<Refiner>(new FMRefiner(init_refine_config));
    
    // Get final level constraints for balance checking
    PartitionConstraints eval_constraints(2, local_config);
    if (xml_constraints) {
        eval_constraints = *xml_constraints;
    } else if (use_type_constraints) {
        eval_constraints.initializeBalancedWithTypes(eval_hg, local_config.imbalance_factor, relaxed_multiplier);
    } else {
        eval_constraints.initializeBalanced(eval_hg, local_config.imbalance_factor);
    }
    
    auto runTrial = [&](Partition trial_partition, bool skip_balance_check = false) -> bool {
        // Check initial balance - skip if explicitly allowed (for fallback)
        if (!skip_balance_check && !coarsest_constraints.isBalanced(trial_partition, coarsest_hg)) {
            return false;  // Discard this trial
        }
        
        trial_refiner->refine(coarsest_hg, trial_partition, level_constraints_cache[0]);
        
        int constraint_idx = 1;
        for (int level = coarsest_level - 1; level >= trial_stop_level; --level, ++constraint_idx) {
            const Hypergraph& level_hg = hierarchy.getLevel(level).getHypergraph();
            
            Partition new_partition(level_hg.getNumNodes(), 2);
            for (NodeID node_id = 0; node_id < level_hg.getNumNodes(); ++node_id) {
                NodeID coarse_node = hierarchy.getLevel(level).getCoarserNode(node_id);
                if (coarse_node != INVALID_NODE) {
                    PartitionID part = trial_partition.getPartition(coarse_node);
                    new_partition.setPartition(node_id, part, level_hg);
                }
            }
            trial_partition = std::move(new_partition);
            trial_refiner->refine(level_hg, trial_partition, level_constraints_cache[constraint_idx]);
        }
        
        Weight trial_cut = PartitionMetrics::calculateCutSize(eval_hg, trial_partition);
        
        // Check final balance after refinement
        if (!eval_constraints.isBalanced(trial_partition, eval_hg)) {
            // Track best imbalanced partition as fallback
            if (trial_cut < best_imbalanced_cut) {
                best_imbalanced_cut = trial_cut;
                best_imbalanced_partition = trial_partition;
            }
            return false;  // Discard this trial (but saved as fallback)
        }
        
        if (trial_cut < best_cut) {
            best_cut = trial_cut;
            best_partition = std::move(trial_partition);
            has_valid_partition = true;
        }
        return true;
    };
    
    bool use_rand = (init_mode == "rand" || init_mode == "all");
    bool use_ghg = (init_mode == "ghg" || init_mode == "all");
    bool use_ghg_opt = (init_mode == "ghg_opt" || init_mode == "all");
    
    // Helper lambda to try a partition and enforce balance if needed
    auto tryPartitionWithBalance = [&](Partition trial_partition, const std::string& name) -> bool {
        // First try to enforce balance constraints if partition violates them
        bool balance_fixed = true;
        if (!coarsest_constraints.isBalanced(trial_partition, coarsest_hg)) {
            if (!enforceBalanceConstraints(coarsest_hg, trial_partition, 
                                           coarsest_constraints, use_type_constraints)) {
                // Could not fix balance, but still try as fallback candidate
                balance_fixed = false;
            }
        }
        // Run trial with skip_balance_check=true if balance couldn't be fixed
        // This allows tracking it as a fallback option
        return runTrial(std::move(trial_partition), !balance_fixed);
    };
    
    if (use_rand) {
        int kNumRandomTrials = local_config.initial_partition_runs;
        if (use_ghg || use_ghg_opt) 
            kNumRandomTrials = 5;
        for (int trial = 0; trial < kNumRandomTrials; ++trial) {
            Configuration trial_config = local_config;
            trial_config.seed = local_config.seed + trial;
            
            RandomPartitioner random_partitioner(trial_config);
            Partition trial_partition = random_partitioner.partition(coarsest_hg, coarsest_constraints);
            tryPartitionWithBalance(std::move(trial_partition), "Random");
        }
    }
    
    if (use_ghg) {
        // Run GHG with 5 different seeds
        constexpr int kNumGHGTrials = 5;
        for (int trial = 0; trial < kNumGHGTrials; ++trial) {
            Configuration trial_config = local_config;
            trial_config.seed = local_config.seed + trial * 100;  // Different seed for each trial
            
            GHGPartitioner ghg_partitioner(trial_config);
            Partition ghg_partition = ghg_partitioner.partition(coarsest_hg, coarsest_constraints);
            tryPartitionWithBalance(std::move(ghg_partition), "GHG");
        }
    }
    
    if (use_ghg_opt) {
        // Run GHGOpt with 5 different strategies (different seed selectors)
        constexpr int kNumGHGOptTrials = 5;
        for (int trial = 0; trial < kNumGHGOptTrials; ++trial) {
            Configuration trial_config = local_config;
            trial_config.seed = trial;  // Each trial uses different seed selection strategy
            
            GHGOptPartitioner ghg_opt_partitioner(trial_config);
            Partition ghg_opt_partition = ghg_opt_partitioner.partition(coarsest_hg, coarsest_constraints);
            tryPartitionWithBalance(std::move(ghg_opt_partition), "GHG_Opt");
        }
    }
    
    initial_timer.stop();
    stats.initial_time = initial_timer.elapsed();
    
    // If no valid partition found, use best imbalanced partition as fallback
    if (!has_valid_partition && best_imbalanced_cut < std::numeric_limits<Weight>::max()) {
        if (xml_constraints) {
            std::cout << "[WARNING] depth=" << debug_depth 
                      << " parts=[" << debug_part_start << "," << debug_part_end << "]"
                      << " Cannot satisfy XML constraints (resources exceed limits). Using best-effort partition." << std::endl;
        }
        best_partition = std::move(best_imbalanced_partition);
        best_cut = best_imbalanced_cut;
    }
    
    // Refinement phase
    Timer refine_timer;
    refine_timer.start();
    
    Partition partition = std::move(best_partition);
    auto refiner = createRefiner(refine_algo, local_config);
    
    for (int level = best_trial_level - 1; level >= 0; --level) {
        const Hypergraph& level_hg = hierarchy.getLevel(level).getHypergraph();
        
        Partition new_partition(level_hg.getNumNodes(), 2);
        
        for (NodeID node_id = 0; node_id < level_hg.getNumNodes(); ++node_id) {
            NodeID coarse_node = hierarchy.getLevel(level).getCoarserNode(node_id);
            if (coarse_node != INVALID_NODE) {
                PartitionID part = partition.getPartition(coarse_node);
                new_partition.setPartition(node_id, part, level_hg);
            }
        }
        partition = std::move(new_partition);
        
        PartitionConstraints level_constraints(2, local_config);
        if (xml_constraints) {
            // Use XML constraints directly (already aggregated for this bipartition)
            level_constraints = *xml_constraints;
        } else if (use_type_constraints) {
            level_constraints.initializeBalancedWithTypes(level_hg, local_config.imbalance_factor, relaxed_multiplier);
        } else {
            level_constraints.initializeBalanced(level_hg, local_config.imbalance_factor);
        }
        refiner->refine(level_hg, partition, level_constraints);
    }
    
    refine_timer.stop();
    stats.refine_time = refine_timer.elapsed();
    
    // Calculate final cut size
    const Hypergraph& final_hg = hierarchy.getFinestLevel().getHypergraph();
    stats.cut_size = PartitionMetrics::calculateCutSize(final_hg, partition);
    
    return partition;
}

// ========== Recursive Bipartition ==========

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
                                std::vector<BipartitionStats>& all_stats,
                                const PartitionConstraints* xml_constraints,
                                std::mutex& partition_mutex,
                                std::mutex& stats_mutex,
                                int depth) {
    if (subset.empty()) {
        return;
    }
    
    if (target_parts == 1) {
        std::lock_guard<std::mutex> lock(partition_mutex);
        for (NodeID node : subset) {
            final_partition.setPartition(node, base_partition_id, original_hg);
        }
        return;
    }
    
    // Build sub-hypergraph
    Hypergraph sub_hg = buildSubHypergraph(original_hg, subset);
    
    // Build aggregated XML constraints for this bipartition (if provided)
    PartitionConstraints bipart_constraints(2, base_config);
    const PartitionConstraints* bipart_constraints_ptr = nullptr;
    
    if (xml_constraints) {
        PartitionID half = target_parts / 2;
        PartitionID group0_start = base_partition_id;
        PartitionID group0_end = base_partition_id + half - 1;
        PartitionID group1_start = base_partition_id + half;
        PartitionID group1_end = base_partition_id + target_parts - 1;
        
        // Aggregate constraints: sum max capacities for each group
        for (size_t type_idx = 0; type_idx < NUM_NODE_TYPES; ++type_idx) {
            NodeType type = static_cast<NodeType>(type_idx);
            Weight max0 = 0, max1 = 0;
            bool unlimited0 = false, unlimited1 = false;
            
            for (PartitionID pid = group0_start; pid <= group0_end; ++pid) {
                CapacityConstraint c = xml_constraints->getCapacity(pid, type);
                if (c.max_capacity == std::numeric_limits<Weight>::max()) {
                    unlimited0 = true;
                } else {
                    max0 += c.max_capacity;
                }
            }
            for (PartitionID pid = group1_start; pid <= group1_end; ++pid) {
                CapacityConstraint c = xml_constraints->getCapacity(pid, type);
                if (c.max_capacity == std::numeric_limits<Weight>::max()) {
                    unlimited1 = true;
                } else {
                    max1 += c.max_capacity;
                }
            }
            
            bipart_constraints.setCapacity(0, type, 0, 
                unlimited0 ? std::numeric_limits<Weight>::max() : max0);
            bipart_constraints.setCapacity(1, type, 0, 
                unlimited1 ? std::numeric_limits<Weight>::max() : max1);
        }
        bipart_constraints.setXmlConstraintMode(true);
        bipart_constraints_ptr = &bipart_constraints;
        
        // DEBUG: Print resource counts for this bipartition
        std::cout << "[DEBUG] depth=" << depth 
                  << " parts=[" << base_partition_id << "," << (base_partition_id + target_parts - 1) << "]"
                  << " nodes=" << sub_hg.getNumNodes() << " resources: ";
        for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
            Weight w = sub_hg.getNodeWeightByType(static_cast<NodeType>(t));
            if (w > 0) {
                std::cout << nodeTypeToString(static_cast<NodeType>(t)) << "=" << w << " ";
            }
        }
        std::cout << std::endl;
    }
    
    // Run direct bipartition with statistics
    BipartitionStats stats;
    stats.depth = depth;
    stats.part_start = base_partition_id;
    stats.part_end = base_partition_id + target_parts - 1;
    
    Partition sub_partition = runDirectBipartition(std::move(sub_hg),
                                                   base_config,
                                                   coarsen_algo,
                                                   refine_algo,
                                                   init_mode,
                                                   use_type_constraints,
                                                   relaxed_multiplier,
                                                   bipart_constraints_ptr,
                                                   depth,
                                                   base_partition_id,
                                                   base_partition_id + target_parts - 1,
                                                   stats);
    
    // Collect statistics (thread-safe)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        all_stats.push_back(stats);
    }
    
    // Split nodes into two groups
    std::vector<NodeID> part_nodes[2];
    part_nodes[0].reserve(subset.size() / 2 + 1);
    part_nodes[1].reserve(subset.size() / 2 + 1);
    
    for (size_t new_id = 0; new_id < subset.size(); ++new_id) {
        NodeID original_node = subset[new_id];
        PartitionID part = sub_partition.getPartition(static_cast<NodeID>(new_id));
        if (part >= 2) part %= 2;
        part_nodes[part].push_back(original_node);
    }
    
    // Fallback if partition is degenerate
    if (part_nodes[0].empty() || part_nodes[1].empty()) {
        part_nodes[0].clear();
        part_nodes[1].clear();
        for (size_t i = 0; i < subset.size(); ++i) {
            part_nodes[i < subset.size() / 2 ? 0 : 1].push_back(subset[i]);
        }
    }
    
    PartitionID half = target_parts / 2;
    
    // Parallel execution for top levels
    constexpr int kMaxParallelDepth = 2;
    
    if (depth < kMaxParallelDepth && subset.size() > 1000) {
        std::future<void> left_future = std::async(std::launch::async, [&]() {
            recursiveBipartitionDirect(original_hg,
                                       part_nodes[0],
                                       base_partition_id,
                                       half,
                                       base_config,
                                       coarsen_algo,
                                       refine_algo,
                                       init_mode,
                                       use_type_constraints,
                                       relaxed_multiplier,
                                       final_partition,
                                       all_stats,
                                       xml_constraints,
                                       partition_mutex,
                                       stats_mutex,
                                       depth + 1);
        });
        
        recursiveBipartitionDirect(original_hg,
                                   part_nodes[1],
                                   base_partition_id + half,
                                   target_parts - half,
                                   base_config,
                                   coarsen_algo,
                                   refine_algo,
                                   init_mode,
                                   use_type_constraints,
                                   relaxed_multiplier,
                                   final_partition,
                                   all_stats,
                                   xml_constraints,
                                   partition_mutex,
                                   stats_mutex,
                                   depth + 1);
        
        left_future.wait();
    } else {
        // Sequential execution
        recursiveBipartitionDirect(original_hg,
                                   part_nodes[0],
                                   base_partition_id,
                                   half,
                                   base_config,
                                   coarsen_algo,
                                   refine_algo,
                                   init_mode,
                                   use_type_constraints,
                                   relaxed_multiplier,
                                   final_partition,
                                   all_stats,
                                   xml_constraints,
                                   partition_mutex,
                                   stats_mutex,
                                   depth + 1);
        recursiveBipartitionDirect(original_hg,
                                   part_nodes[1],
                                   base_partition_id + half,
                                   target_parts - half,
                                   base_config,
                                   coarsen_algo,
                                   refine_algo,
                                   init_mode,
                                   use_type_constraints,
                                   relaxed_multiplier,
                                   final_partition,
                                   all_stats,
                                   xml_constraints,
                                   partition_mutex,
                                   stats_mutex,
                                   depth + 1);
    }
}

// Print all bipartition statistics after recursive partitioning completes
void printRecursiveStats(const std::vector<BipartitionStats>& all_stats) {
    std::cout << "\n========== Partitioning Summary ==========" << std::endl;
    
    // Sort by depth then by part_start
    std::vector<BipartitionStats> sorted_stats = all_stats;
    std::sort(sorted_stats.begin(), sorted_stats.end(), 
              [](const BipartitionStats& a, const BipartitionStats& b) {
                  if (a.depth != b.depth) return a.depth < b.depth;
                  return a.part_start < b.part_start;
              });
    
    // Print header
    std::cout << std::left << std::setw(6) << "Depth"
              << std::setw(12) << "Parts"
              << std::right << std::setw(8) << "Nodes"
              << std::setw(8) << "Nets"
              << std::setw(7) << "Levels"
              << std::setw(8) << "Cut"
              << std::setw(10) << "Coarsen"
              << std::setw(10) << "Init"
              << std::setw(10) << "Refine"
              << std::setw(10) << "Total" << std::endl;
    std::cout << std::string(89, '-') << std::endl;
    
    double total_coarsen = 0, total_init = 0, total_refine = 0;
    
    for (const auto& stats : sorted_stats) {
        std::string parts_str = "[" + std::to_string(stats.part_start) + "," 
                              + std::to_string(stats.part_end) + "]";
        double total = stats.coarsen_time + stats.initial_time + stats.refine_time;
        
        std::cout << std::left << std::setw(6) << stats.depth
                  << std::setw(12) << parts_str
                  << std::right << std::setw(8) << stats.num_nodes
                  << std::setw(8) << stats.num_nets
                  << std::setw(7) << stats.num_levels
                  << std::setw(8) << stats.cut_size
                  << std::fixed << std::setprecision(3)
                  << std::setw(10) << stats.coarsen_time
                  << std::setw(10) << stats.initial_time
                  << std::setw(10) << stats.refine_time
                  << std::setw(10) << total << std::endl;
        
        total_coarsen += stats.coarsen_time;
        total_init += stats.initial_time;
        total_refine += stats.refine_time;
    }
    
    std::cout << std::string(89, '-') << std::endl;
    // Total row: aligned with the data columns
    std::cout << std::left << std::setw(6) << ""
              << std::setw(12) << "Total"
              << std::right << std::setw(8) << ""
              << std::setw(8) << ""
              << std::setw(7) << ""
              << std::setw(8) << ""
              << std::fixed << std::setprecision(3)
              << std::setw(10) << total_coarsen
              << std::setw(10) << total_init
              << std::setw(10) << total_refine
              << std::setw(10) << (total_coarsen + total_init + total_refine) << std::endl;
    std::cout << "===========================================" << std::endl;
}

// ========== Print Functions ==========

void printUsage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <hgr_file> [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  -k <num>          Number of partitions (default: 2)" << std::endl;
    std::cout << "  -imbalance <f>    Imbalance factor (default: 0.03)" << std::endl;
    std::cout << "  -coarsen <a>      Coarsening: heavy|first|cluster (default: cluster)" << std::endl;
    std::cout << "  -threshold <num>  Coarsening threshold (default: 100)" << std::endl;
    std::cout << "  -refine <a>       Refinement: fm|greedyfm|greedy (default: gfm)" << std::endl;
    std::cout << "  -passes <num>     Max refinement passes (default: 10)" << std::endl;
    std::cout << "  -seed <num>       Random seed (default: 42)" << std::endl;
    std::cout << "  -mode <direct|recursive> Partitioning strategy (default: direct)" << std::endl;
    std::cout << "  -init <rand|ghg|all>     Initial partitioning (default: rand)" << std::endl;
    std::cout << "  -types <file>     Node type file (one type per line: LUT/FF/MUX/CARRY/IO/DSP/BRAM/OTHER)" << std::endl;
    std::cout << "  -xml <file>       XML constraint file with per-SLR resource limits" << std::endl;
    std::cout << "  -relaxed <f>      Relaxed imbalance multiplier for DSP/BRAM/IO (default: 3.0)" << std::endl;
    std::cout << "  -output <file>    Output partition file" << std::endl;
}

void printHypergraphInfo(const Hypergraph& hg, bool use_types) {
    std::cout << "\n========== Hypergraph Statistics ==========" << std::endl;
    
    // Basic counts
    std::cout << "Nodes: " << hg.getNumNodes() 
              << ", Nets: " << hg.getNumNets()
              << ", Pins: " << hg.getNumPins() << std::endl;
    
    // Degree statistics
    double node_avg_degree = 0.0;
    double net_avg_degree = 0.0;
    
    for (NodeID i = 0; i < hg.getNumNodes(); ++i) {
        node_avg_degree += hg.getNodeDegree(i);
    }
    node_avg_degree /= hg.getNumNodes();
    
    for (EdgeID i = 0; i < hg.getNumNets(); ++i) {
        net_avg_degree += hg.getNetSize(i);
    }
    net_avg_degree /= hg.getNumNets();
    
    std::cout << "Node avg degree: " << std::fixed << std::setprecision(2) << node_avg_degree
              << ", Net avg degree: " << std::fixed << std::setprecision(2) << net_avg_degree << std::endl;
    
    // Resource distribution (types mode or xml mode shows type counts)
    if (use_types) {
        // Collect type names and counts
        std::vector<std::string> type_names;
        std::vector<Weight> type_counts;
        
        for (int t = 0; t < NUM_NODE_TYPES; ++t) {
            NodeType type = static_cast<NodeType>(t);
            Weight weight = hg.getNodeWeightByType(type);
            if (weight > 0) {
                type_names.push_back(nodeTypeToString(type));
                type_counts.push_back(weight);
            }
        }
        
        // Print header line (type names)
        std::cout << "Resources: ";
        for (size_t i = 0; i < type_names.size(); ++i) {
            std::cout << std::setw(10) << type_names[i];
        }
        std::cout << std::endl;
        
        // Print counts line
        std::cout << "           ";
        for (size_t i = 0; i < type_counts.size(); ++i) {
            std::cout << std::setw(10) << type_counts[i];
        }
        std::cout << std::endl;
    } else {
        // Balance mode: just show total weight
        std::cout << "Total weight: " << hg.getTotalNodeWeight() << std::endl;
    }
    
    std::cout << "===========================================" << std::endl;
}

void printCoarseningResults(const MultilevelCoarsener& coarsener, 
                            const HypergraphHierarchy& hierarchy,
                            double time_s) {
    std::cout << "\n========== Coarsening Results ==========" << std::endl;
    
    const auto& stats = coarsener.getStatistics();
    
    if (stats.empty()) {
        std::cout << "No coarsening performed." << std::endl;
    } else {
        std::cout << "Number of levels: " << hierarchy.getNumLevels() << std::endl;
        std::cout << "Coarsening time:  " << std::fixed << std::setprecision(3)
                  << time_s << " seconds\n" << std::endl;
        
        std::cout << std::setw(6) << "Level"
                  << std::setw(10) << "Nodes"
                  << std::setw(10) << "Nets"
                  << std::setw(12) << "Total Pins"
                  << std::setw(12) << "Avg Node Deg"
                  << std::setw(12) << "Avg Net Deg"
                  << std::setw(12) << "Max Cluster" << std::endl;
        std::cout << std::string(74, '-') << std::endl;
        
        uint32_t num_levels = hierarchy.getNumLevels();
        for (uint32_t level = 0; level < num_levels; ++level) {
            const auto& level_hg = hierarchy.getLevel(level).getHypergraph();
            NodeID num_nodes = level_hg.getNumNodes();
            EdgeID num_nets = level_hg.getNumNets();
            size_t total_pins = level_hg.getNumPins();
            
            // Calculate average node degree
            double avg_node_deg = (num_nodes > 0) ? 
                static_cast<double>(total_pins) / num_nodes : 0.0;
            
            // Calculate average net degree
            double avg_net_deg = (num_nets > 0) ?
                static_cast<double>(total_pins) / num_nets : 0.0;
            
            // Calculate max cluster size (number of finest nodes per coarse node)
            uint32_t max_cluster_size = 1;
            if (level > 0) {
                for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
                    auto finest_nodes = hierarchy.mapNodeToFinestLevel(node_id, level);
                    if (finest_nodes.size() > max_cluster_size) {
                        max_cluster_size = static_cast<uint32_t>(finest_nodes.size());
                    }
                }
            }
            
            std::cout << std::setw(6) << level
                      << std::setw(10) << num_nodes
                      << std::setw(10) << num_nets
                      << std::setw(12) << total_pins
                      << std::setw(12) << std::fixed << std::setprecision(2) << avg_node_deg
                      << std::setw(12) << std::fixed << std::setprecision(2) << avg_net_deg
                      << std::setw(12) << max_cluster_size << std::endl;
        }
        
        const auto& last = stats.back();
        double total_ratio = static_cast<double>(stats[0].original_nodes) /
                            last.coarse_nodes;
        
        std::cout << "\nOverall reduction: " << std::fixed << std::setprecision(1)
                  << (100.0 * (1.0 - 1.0/total_ratio)) << "% "
                  << "(ratio=" << std::fixed << std::setprecision(2) 
                  << total_ratio << ")" << std::endl;
    }
    
    std::cout << "========================================\n" << std::endl;
}

void printRefinementResults(const std::vector<RefinementStats>& stats, 
                           double time_s) {
    std::cout << "\n========== Refinement Results ==========" << std::endl;
    std::cout << "Refinement time: " << std::fixed << std::setprecision(3)
              << time_s << " seconds\n" << std::endl;
    
    if (stats.empty()) {
        std::cout << "No refinement performed." << std::endl;
    } else {
        std::cout << std::setw(6) << "Level"
                  << std::setw(12) << "Initial"
                  << std::setw(12) << "Final"
                  << std::setw(12) << "Improve"
                  << std::setw(10) << "Improve%"
                  << std::setw(8) << "Passes" << std::endl;
        std::cout << std::string(60, '-') << std::endl;
        
        Weight total_initial = 0;
        Weight total_improvement = 0;
        
        for (size_t i = 0; i < stats.size(); ++i) {
            const auto& s = stats[stats.size() - 1 - i];  // Reverse order
            
            if (i == 0) {
                total_initial = s.initial_cut;
            }
            total_improvement += s.improvement;
            
            double improve_pct = 0.0;
            if (s.initial_cut > 0) {
                improve_pct = 100.0 * s.improvement / s.initial_cut;
            }
            
            std::cout << std::setw(6) << i
                      << std::setw(12) << s.initial_cut
                      << std::setw(12) << s.final_cut
                      << std::setw(12) << s.improvement
                      << std::setw(9) << std::fixed << std::setprecision(1)
                      << improve_pct << "%"
                      << std::setw(8) << s.num_passes << std::endl;
        }
        
        std::cout << "\nTotal improvement: " << total_improvement;
        if (total_initial > 0) {
            std::cout << " (" << std::fixed << std::setprecision(1)
                      << (100.0 * total_improvement / total_initial) << "%)";
        }
        std::cout << std::endl;
    }
    
    std::cout << "========================================\n" << std::endl;
}

void printFinalResults(const Hypergraph& hg, const Partition& partition,
                      const Configuration& config) {
    std::cout << "\n========== Final Partition Results ==========" << std::endl;
    
    Weight cut_size = PartitionMetrics::calculateCutSize(hg, partition);
    Weight connectivity = PartitionMetrics::calculateConnectivity(hg, partition);
    Weight soed = PartitionMetrics::calculateSOED(hg, partition);
    double imbalance = PartitionMetrics::calculateImbalance(partition);
    
    std::cout << "Quality metrics:" << std::endl;
    std::cout << "  Cut size:     " << cut_size << std::endl;
    std::cout << "  Connectivity: " << connectivity << std::endl;
    std::cout << "  SOED:         " << soed << std::endl;
    std::cout << "  Imbalance:    " << std::fixed << std::setprecision(2)
              << (imbalance * 100) << "%" << std::endl;
    
    std::cout << "\nPartition distribution:" << std::endl;
    
    // Count nodes per type per partition
    constexpr int NUM_TYPES = static_cast<int>(NodeType::OTHER) + 1;
    std::vector<std::vector<NodeID>> type_counts(config.num_partitions, 
                                                  std::vector<NodeID>(NUM_TYPES, 0));
    
    // Check which types are present in the hypergraph
    std::vector<bool> type_present(NUM_TYPES, false);
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        PartitionID part = partition.getPartition(node_id);
        if (part != INVALID_PARTITION) {
            int type_idx = static_cast<int>(hg.getNodeType(node_id));
            type_counts[part][type_idx]++;
            type_present[type_idx] = true;
        }
    }
    
    // Build header with present types
    std::vector<int> present_types;
    for (int t = 0; t < NUM_TYPES; ++t) {
        if (type_present[t]) {
            present_types.push_back(t);
        }
    }
    
    // Print header
    std::cout << std::setw(6) << "Part"
              << std::setw(8) << "Nodes"
              << std::setw(10) << "Weight"
              << std::setw(9) << "Weight%";
    for (int t : present_types) {
        std::string type_name = nodeTypeToString(static_cast<NodeType>(t));
        std::cout << std::setw(8) << type_name;
    }
    std::cout << std::endl;
    
    // Calculate separator width
    size_t sep_width = 33 + present_types.size() * 8;
    std::cout << std::string(sep_width, '-') << std::endl;
    
    Weight total_weight = hg.getTotalNodeWeight();
    for (PartitionID part = 0; part < config.num_partitions; ++part) {
        Weight part_weight = partition.getPartitionWeight(part);
        double weight_pct = 100.0 * part_weight / total_weight;
        
        std::cout << std::setw(6) << part
                  << std::setw(8) << partition.getPartitionSize(part)
                  << std::setw(10) << part_weight
                  << std::setw(8) << std::fixed << std::setprecision(1)
                  << weight_pct << "%";
        
        // Print type counts
        for (int t : present_types) {
            std::cout << std::setw(8) << type_counts[part][t];
        }
        std::cout << std::endl;
    }
    
    // Cut size between each pair of partitions
    if (config.num_partitions <= 10) {
        std::cout << "\nCut size matrix (partition pairs):" << std::endl;
        
        // Compute cut between each pair
        std::vector<std::vector<Weight>> pair_cuts(config.num_partitions,
                                                   std::vector<Weight>(config.num_partitions, 0));
        
        for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
            if (hg.isNetGlobal(net_id)) continue;
            
            std::vector<bool> parts_present(config.num_partitions, false);
            auto nodes = hg.getNetNodes(net_id);
            
            for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
                PartitionID part = partition.getPartition(*it);
                if (part != INVALID_PARTITION) {
                    parts_present[part] = true;
                }
            }
            
            // Add to cut between all pairs
            Weight net_weight = hg.getNetWeight(net_id);
            for (PartitionID i = 0; i < config.num_partitions; ++i) {
                for (PartitionID j = i + 1; j < config.num_partitions; ++j) {
                    if (parts_present[i] && parts_present[j]) {
                        pair_cuts[i][j] += net_weight;
                    }
                }
            }
        }
        
        // Print matrix header
        std::cout << "     ";
        for (PartitionID j = 0; j < config.num_partitions; ++j) {
            std::cout << std::setw(6) << j;
        }
        std::cout << std::endl;
        
        // Print matrix
        for (PartitionID i = 0; i < config.num_partitions; ++i) {
            std::cout << std::setw(4) << i << " ";
            for (PartitionID j = 0; j < config.num_partitions; ++j) {
                if (i == j) {
                    std::cout << std::setw(6) << "-";
                } else if (i < j) {
                    std::cout << std::setw(6) << pair_cuts[i][j];
                } else {
                    std::cout << std::setw(6) << pair_cuts[j][i];
                }
            }
            std::cout << std::endl;
        }
    }
    
    std::cout << "============================================\n" << std::endl;
}

// ========== MultilevelPartitionerApp Implementation ==========

void MultilevelPartitionerApp::setLargeNetThreshold(Hypergraph& hg) {
    uint64_t large_net_count = 0;
    uint64_t avg_large_net_size = 0;
    uint64_t max_large_net_size = 0;
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        auto net_size = hg.getNetSize(net_id);
        if (net_size > 500) {
            large_net_count++;
            avg_large_net_size += net_size;
            if (net_size > max_large_net_size) {
                max_large_net_size = net_size;
            }
        }
    }
    if (large_net_count <= 10) {
        config_.large_net_threshold = 200;
        return;
    }
    avg_large_net_size /= large_net_count;
    config_.large_net_threshold = std::max(avg_large_net_size * 10, max_large_net_size / 2);
}

bool MultilevelPartitionerApp::parseArguments(int argc, char* argv[]) {
    if (argc < 2) {
        return false;
    }
    
    // Initialize defaults
    input_file_ = argv[1];
    output_file_ = "";
    type_file_ = "";
    xml_file_ = "";  // XML constraint file
    coarsen_algo_ = "cluster";
    refine_algo_ = "gfm";
    partition_mode_ = "direct";
    init_mode_ = "rand";
    relaxed_multiplier_ = 3.0;
    
    config_.num_partitions = 2;
    config_.imbalance_factor = 0.05;
    config_.coarsening_threshold = 100;
    config_.max_refinement_passes = 10;
    config_.seed = 42;
    
    // Parse options
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            config_.num_partitions = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-imbalance") == 0 && i + 1 < argc) {
            config_.imbalance_factor = atof(argv[++i]);
        } else if (strcmp(argv[i], "-coarsen") == 0 && i + 1 < argc) {
            coarsen_algo_ = argv[++i];
        } else if (strcmp(argv[i], "-threshold") == 0 && i + 1 < argc) {
            config_.coarsening_threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-refine") == 0 && i + 1 < argc) {
            refine_algo_ = argv[++i];
        } else if (strcmp(argv[i], "-passes") == 0 && i + 1 < argc) {
            config_.max_refinement_passes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-seed") == 0 && i + 1 < argc) {
            config_.seed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-output") == 0 && i + 1 < argc) {
            output_file_ = argv[++i];
        } else if (strcmp(argv[i], "-mode") == 0 && i + 1 < argc) {
            partition_mode_ = argv[++i];
        } else if (strcmp(argv[i], "-types") == 0 && i + 1 < argc) {
            type_file_ = argv[++i];
        } else if (strcmp(argv[i], "-relaxed") == 0 && i + 1 < argc) {
            relaxed_multiplier_ = atof(argv[++i]);
        } else if (strcmp(argv[i], "-coarsen_opt") == 0) {
            config_.coarsen_opt = true;
        } else if (strcmp(argv[i], "-init") == 0 && i + 1 < argc) {
            init_mode_ = argv[++i];
        } else if (strcmp(argv[i], "-xml") == 0 && i + 1 < argc) {
            xml_file_ = argv[++i];
            use_xml_constraints_ = true;
        }
    }
    
    // Normalize strings
    auto toLower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    toLower(coarsen_algo_);
    toLower(refine_algo_);
    toLower(partition_mode_);
    toLower(init_mode_);
    
    use_recursive_ = (partition_mode_ == "recursive");
    if (use_recursive_ && !isPowerOfTwo(config_.num_partitions)) {
        std::cout << "[Warning] Recursive mode requires k to be a power of two. "
                  << "Falling back to direct k-way partitioning." << std::endl;
        use_recursive_ = false;
        partition_mode_ = "direct";
    }
    
    use_type_constraints_ = !type_file_.empty();
    
    return true;
}

void MultilevelPartitionerApp::printConfiguration() const {
    std::cout << "\n========== Configuration ==========" << std::endl;
    
    // Algorithm modes
    std::cout << "Coarsen mode:    " << coarsen_algo_ << std::endl;
    std::cout << "Init mode:       " << init_mode_ << std::endl;
    std::cout << "Refine mode:     " << refine_algo_ << std::endl;
    std::cout << "Imbalance:       " << config_.imbalance_factor << std::endl;
    // Constraint type
    std::string constraint_type = "balance";
    if (use_xml_constraints_) {
        constraint_type = "xml";
    } else if (use_type_constraints_) {
        constraint_type = "types";
    }
    std::cout << "Constraint type: " << constraint_type << std::endl;
    
    // Parameters
    std::cout << "Partitions (k):  " << config_.num_partitions << std::endl;
    std::cout << "Refine passes:   " << config_.max_refinement_passes << std::endl;
    std::cout << "Init trials:     " << config_.initial_partition_runs << std::endl;
    std::cout << "Init depth:      " << config_.trial_refine_levels << std::endl;
    std::cout << "Random seed:     " << config_.seed << std::endl;
    std::cout << "====================================" << std::endl;
}

Hypergraph MultilevelPartitionerApp::parseHypergraph() {
    return use_type_constraints_ ?
        HgrParser::parseWithTypes(input_file_, type_file_) :
        HgrParser::parse(input_file_);
}

int MultilevelPartitionerApp::runRecursivePartitioning(Hypergraph& hg) {
    std::cout << "\n========== Recursive Partitioning ==========" << std::endl;
    std::cout << "Target partitions: " << config_.num_partitions << std::endl;
    
    Timer total_timer;
    total_timer.start();
    
    std::vector<NodeID> all_nodes(hg.getNumNodes());
    std::iota(all_nodes.begin(), all_nodes.end(), 0);
    
    Partition final_partition(hg.getNumNodes(), config_.num_partitions);
    std::vector<BipartitionStats> all_stats;
    std::mutex partition_mutex;
    std::mutex stats_mutex;
    
    // Use direct-based recursive bipartition
    recursiveBipartitionDirect(hg,
                               all_nodes,
                               0,
                               config_.num_partitions,
                               config_,
                               coarsen_algo_,
                               refine_algo_,
                               init_mode_,
                               use_type_constraints_,
                               relaxed_multiplier_,
                               final_partition,
                               all_stats,
                               (use_xml_constraints_ && xml_constraints_) ? xml_constraints_.get() : nullptr,
                               partition_mutex,
                               stats_mutex,
                               0);
    
    total_timer.stop();
    
    // Print partitioning summary (Part 2)
    printRecursiveStats(all_stats);
    
    // Print final results (Part 3)
    printFinalResults(hg, final_partition, config_);
    
    std::cout << "========== Timing Summary ==========" << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(3)
              << total_timer.elapsed() << " s" << std::endl;
    std::cout << "====================================" << std::endl;
    
    if (!output_file_.empty()) {
        std::ofstream file(output_file_);
        for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
            file << final_partition.getPartition(node_id) << std::endl;
        }
        file.close();
        std::cout << "Partition written to: " << output_file_ << std::endl;
    }
    
    return 0;
}

int MultilevelPartitionerApp::runDirectPartitioning(Hypergraph& hg, double parse_time) {
    Timer coarsen_timer;
    coarsen_timer.start();
    
    auto coarsener = createCoarsener(coarsen_algo_, config_);
    MultilevelCoarsener ml_coarsener(config_, std::move(coarsener));
    HypergraphHierarchy hierarchy = ml_coarsener.coarsen(std::move(hg));
    
    coarsen_timer.stop();
    printCoarseningResults(ml_coarsener, hierarchy, coarsen_timer.elapsed());
    
    // Initial partitioning
    Timer initial_timer;
    initial_timer.start();
    
    std::cout << "========== Initial Partitioning ==========" << std::endl;
    const Hypergraph& coarsest_hg = hierarchy.getCoarsestLevel().getHypergraph();
    int coarsest_level = hierarchy.getNumLevels() - 1;
    
    int num_trial_refine_levels = std::min(config_.trial_refine_levels, static_cast<int>(hierarchy.getNumLevels()));
    int trial_stop_level = std::max(0, coarsest_level - num_trial_refine_levels);
    
    std::cout << "Partitioning coarsest level (" << coarsest_hg.getNumNodes()
              << " nodes) with random trials..." << std::endl;
    std::cout << "Each trial will be refined through " << num_trial_refine_levels 
              << " levels before comparison" << std::endl;
    
    PartitionConstraints coarsest_constraints(config_.num_partitions, config_);
    if (use_xml_constraints_ && xml_constraints_) {
        // Use cached XML constraints (same capacity at all levels)
        coarsest_constraints = *xml_constraints_;
    } else if (use_type_constraints_) {
        coarsest_constraints.initializeBalancedWithTypes(coarsest_hg, config_.imbalance_factor, relaxed_multiplier_);
    } else {
        coarsest_constraints.initializeBalanced(coarsest_hg, config_.imbalance_factor);
    }
    
    int best_trial_level = coarsest_level;
    Partition partition = runInitialPartitioning(hierarchy, coarsest_level, coarsest_constraints,
                                                use_type_constraints_, relaxed_multiplier_,
                                                refine_algo_, init_mode_,
                                                best_trial_level);
    Weight initial_cut = PartitionMetrics::calculateCutSize(
        hierarchy.getLevel(best_trial_level).getHypergraph(), partition);
    
    initial_timer.stop();
    std::cout << "Best cut size after " << num_trial_refine_levels 
              << " level refinement: " << initial_cut << std::endl;
    std::cout << "Initial partitioning time: " << std::fixed << std::setprecision(3)
              << initial_timer.elapsed() << " s" << std::endl;
    std::cout << "Continuing refinement from level " << best_trial_level << std::endl;
    std::cout << "=========================================\n" << std::endl;
    
    // Refinement phase
    Timer refine_timer;
    refine_timer.start();
    
    std::vector<RefinementStats> refine_stats;
    auto refiner = createRefiner(refine_algo_, config_);
    
    for (int level = best_trial_level - 1; level >= 0; --level) {
        const Hypergraph& level_hg = hierarchy.getLevel(level).getHypergraph();
        
        Partition new_partition(level_hg.getNumNodes(), config_.num_partitions);
        for (NodeID node_id = 0; node_id < level_hg.getNumNodes(); ++node_id) {
            NodeID coarse_node = hierarchy.getLevel(level).getCoarserNode(node_id);
            if (coarse_node != INVALID_NODE) {
                PartitionID part = partition.getPartition(coarse_node);
                new_partition.setPartition(node_id, part, level_hg);
            }
        }
        partition = new_partition;
        
        PartitionConstraints level_constraints(config_.num_partitions, config_);
        if (use_xml_constraints_ && xml_constraints_) {
            // Use cached XML constraints (same capacity at all levels)
            level_constraints = *xml_constraints_;
        } else if (use_type_constraints_) {
            level_constraints.initializeBalancedWithTypes(level_hg, config_.imbalance_factor, relaxed_multiplier_);
        } else {
            level_constraints.initializeBalanced(level_hg, config_.imbalance_factor);
        }
        
        RefinementStats stats = refiner->refine(level_hg, partition, level_constraints);
        refine_stats.push_back(stats);
    }
    
    refine_timer.stop();
    printRefinementResults(refine_stats, refine_timer.elapsed());
    
    // Final results
    const Hypergraph& final_hg = hierarchy.getFinestLevel().getHypergraph();
    printFinalResults(final_hg, partition, config_);
    
    std::cout << "========== Timing Summary ==========" << std::endl;
    std::cout << "Parsing time:            " << std::fixed << std::setprecision(3)
              << std::setw(8) << parse_time << " s" << std::endl;
    std::cout << "Coarsening time:         " << std::fixed << std::setprecision(3)
              << std::setw(8) << coarsen_timer.elapsed() << " s" << std::endl;
    std::cout << "Initial partition time:  " << std::fixed << std::setprecision(3)
              << std::setw(8) << initial_timer.elapsed() << " s" << std::endl;
    std::cout << "Refinement time:         " << std::fixed << std::setprecision(3)
              << std::setw(8) << refine_timer.elapsed() << " s" << std::endl;
    std::cout << "====================================\n" << std::endl;
    
    if (!output_file_.empty()) {
        std::ofstream file(output_file_);
        for (NodeID i = 0; i < partition.getNumNodes(); ++i) {
            file << partition.getPartition(i) << std::endl;
        }
        file.close();
        std::cout << "Partition written to: " << output_file_ << std::endl;
    }
    
    return 0;
}

Partition MultilevelPartitionerApp::runInitialPartitioning(
        const HypergraphHierarchy& hierarchy,
        const int coarsest_level,
        const PartitionConstraints& coarsest_constraints,
        bool use_type_constraints,
        double relaxed_multiplier,
        const std::string& refine_algo,
        const std::string& init_mode,
        int& best_trial_level) {
    
    int num_trial_refine_levels = std::min(config_.trial_refine_levels, static_cast<int>(hierarchy.getNumLevels()));
    int trial_stop_level = std::max(0, coarsest_level - num_trial_refine_levels);
    const Hypergraph& coarsest_hg = hierarchy.getCoarsestLevel().getHypergraph();
    const Hypergraph& eval_hg = hierarchy.getLevel(trial_stop_level).getHypergraph();
    
    Partition best_partition(eval_hg.getNumNodes(), config_.num_partitions);
    Weight best_cut = std::numeric_limits<Weight>::max();
    best_trial_level = coarsest_level;
    
    // Pre-create level constraints
    std::vector<PartitionConstraints> level_constraints_cache;
    level_constraints_cache.reserve(num_trial_refine_levels + 1);
    for (int level = coarsest_level; level >= trial_stop_level; --level) {
        const Hypergraph& level_hg = hierarchy.getLevel(level).getHypergraph();
        PartitionConstraints constraints(config_.num_partitions, config_);
        if (coarsest_constraints.isXMLConstraintMode()) {
            // Use same XML constraints at all levels
            constraints = coarsest_constraints;
        } else if (use_type_constraints) {
            constraints.initializeBalancedWithTypes(level_hg, config_.imbalance_factor, relaxed_multiplier);
        } else {
            constraints.initializeBalanced(level_hg, config_.imbalance_factor);
        }
        level_constraints_cache.push_back(std::move(constraints));
    }
    
    // Get eval level constraints for balance checking
    PartitionConstraints eval_constraints(config_.num_partitions, config_);
    if (coarsest_constraints.isXMLConstraintMode()) {
        eval_constraints = coarsest_constraints;
    } else if (use_type_constraints) {
        eval_constraints.initializeBalancedWithTypes(eval_hg, config_.imbalance_factor, relaxed_multiplier);
    } else {
        eval_constraints.initializeBalanced(eval_hg, config_.imbalance_factor);
    }
    
    bool use_rand = (init_mode == "rand" || init_mode == "all");
    bool use_ghg = (init_mode == "ghg" || init_mode == "all");
    bool use_ghg_opt = (init_mode == "ghg_opt" || init_mode == "all");
    
    int valid_trials = 0;
    int discarded_trials = 0;
    
    // Create FM refiner for initial partition (more aggressive, multi-pass)
    // Use FM instead of GreedyFM because coarsest level is small
    Configuration init_refine_config = config_;
    init_refine_config.max_refinement_passes = 20;  // More passes for better quality
    auto trial_refiner = std::unique_ptr<Refiner>(new FMRefiner(init_refine_config));
    
    // For balance enforcement, use type-based balancing if using types OR xml mode
    bool use_type_balancing = use_type_constraints_ || coarsest_constraints.isXMLConstraintMode();
    
    // Helper lambda - returns true if trial is valid (balanced)
    auto runTrial = [&](Partition trial_partition, const std::string& method_name, int trial_idx) -> bool {
        // Try to enforce balance constraints if partition violates them
        if (!coarsest_constraints.isBalanced(trial_partition, coarsest_hg)) {
            if (!enforceBalanceConstraints(coarsest_hg, trial_partition, 
                                           coarsest_constraints, use_type_balancing)) {
                std::cout << method_name << " trial " << trial_idx << " discarded (initial imbalance)" << std::endl;
                coarsest_constraints.printConstraintViolations(trial_partition, coarsest_hg);
                return false;
            }
        }
        
        // Calculate initial cut size BEFORE refinement
        Weight initial_cut = PartitionMetrics::calculateCutSize(coarsest_hg, trial_partition);
        
        trial_refiner->refine(coarsest_hg, trial_partition, level_constraints_cache[0]);
        
        int constraint_idx = 1;
        for (int level = coarsest_level - 1; level >= trial_stop_level; --level, ++constraint_idx) {
            const Hypergraph& level_hg = hierarchy.getLevel(level).getHypergraph();
            
            Partition new_partition(level_hg.getNumNodes(), config_.num_partitions);
            for (NodeID node_id = 0; node_id < level_hg.getNumNodes(); ++node_id) {
                NodeID coarse_node = hierarchy.getLevel(level).getCoarserNode(node_id);
                if (coarse_node != INVALID_NODE) {
                    PartitionID part = trial_partition.getPartition(coarse_node);
                    new_partition.setPartition(node_id, part, level_hg);
                }
            }
            trial_partition = std::move(new_partition);
            trial_refiner->refine(level_hg, trial_partition, level_constraints_cache[constraint_idx]);
        }
        
        // Check final balance after refinement - try to fix if violates
        if (!eval_constraints.isBalanced(trial_partition, eval_hg)) {
            if (!enforceBalanceConstraints(eval_hg, trial_partition, 
                                           eval_constraints, use_type_balancing)) {
                std::cout << method_name << " trial " << trial_idx << " discarded (final imbalance)" << std::endl;
                eval_constraints.printConstraintViolations(trial_partition, eval_hg);
                return false;
            }
        }
        
        // Calculate final cut size AFTER refinement
        Weight final_cut = PartitionMetrics::calculateCutSize(eval_hg, trial_partition);
        std::cout << method_name << " trial " << trial_idx 
                  << " cut: " << initial_cut << " -> " << final_cut
                  << " (improve: " << (initial_cut - final_cut) << ")" << std::endl;
        if (final_cut < best_cut) {
            best_cut = final_cut;
            best_partition = trial_partition;
            best_trial_level = trial_stop_level;
        }
        return true;
    };
    
    if (use_rand) {
        int kNumRandomTrials = config_.initial_partition_runs;
        if (use_ghg || use_ghg_opt) 
            kNumRandomTrials = 5;
        for (int trial = 0; trial < kNumRandomTrials; ++trial) {
            Configuration trial_config = config_;
            trial_config.seed = config_.seed + trial;
            
            RandomPartitioner random_partitioner(trial_config);
            Partition trial_partition = random_partitioner.partition(coarsest_hg, coarsest_constraints);
            if (runTrial(std::move(trial_partition), "Random", trial)) {
                valid_trials++;
            } else {
                discarded_trials++;
            }
        }
    }
    
    if (use_ghg && config_.num_partitions == 2) {
        // Run GHG with 5 different seeds
        constexpr int kNumGHGTrials = 5;
        for (int trial = 0; trial < kNumGHGTrials; ++trial) {
            Configuration trial_config = config_;
            trial_config.seed = config_.seed + trial * 100;
            
            GHGPartitioner ghg_partitioner(trial_config);
            Partition ghg_partition = ghg_partitioner.partition(coarsest_hg, coarsest_constraints);
            if (runTrial(std::move(ghg_partition), "GHG", trial)) {
                valid_trials++;
            } else {
                discarded_trials++;
            }
        }
    }
    
    if (use_ghg_opt && config_.num_partitions == 2) {
        // Run GHGOpt with 5 different strategies
        constexpr int kNumGHGOptTrials = 5;
        for (int trial = 0; trial < kNumGHGOptTrials; ++trial) {
            Configuration trial_config = config_;
            trial_config.seed = trial;  // Each trial uses different seed selection strategy
            
            GHGOptPartitioner ghg_opt_partitioner(trial_config);
            Partition ghg_opt_partition = ghg_opt_partitioner.partition(coarsest_hg, coarsest_constraints);
            if (runTrial(std::move(ghg_opt_partition), "GHG_Opt", trial)) {
                valid_trials++;
            } else {
                discarded_trials++;
            }
        }
    }
    
    std::cout << "Valid trials: " << valid_trials << ", Discarded: " << discarded_trials << std::endl;
    
    return best_partition;
}

int MultilevelPartitionerApp::run() {
    
    Timer total_timer;
    Timer parse_timer;
    total_timer.start();
    
    try {
        // If using XML constraints, parse them first to get number of partitions
        if (use_xml_constraints_) {
            xml_constraints_.reset(new PartitionConstraints(1, config_));  // Temp, will be resized
            PartitionID num_parts = xml_constraints_->initializeFromXML(xml_file_);
            config_.num_partitions = num_parts;
        }
        
        // Part 1: Configuration Info
        printConfiguration();
        
        // Parse hypergraph
        parse_timer.start();
        Hypergraph hg = parseHypergraph();
        setLargeNetThreshold(hg);
        config_.total_node_weight = hg.getTotalNodeWeight();
        parse_timer.stop();
        
        // Part 1 continued: Hypergraph Statistics
        bool use_types = use_type_constraints_ || use_xml_constraints_;
        printHypergraphInfo(hg, use_types);
        
        // Print resource constraints if using xml mode
        if (use_xml_constraints_ && xml_constraints_) {
            xml_constraints_->printConstraintSummary(hg);
        }
        
        double parse_time = parse_timer.elapsed();
        
        int result = 0;
        if (use_recursive_ && config_.num_partitions > 2) {
            // Mode 3: Recursive bipartition (k > 2)
            result = runRecursivePartitioning(hg);
            // Timing summary is already printed in runRecursivePartitioning
        } else if (use_recursive_ && config_.num_partitions == 2) {
            // Mode 1: Direct bipartition (k = 2, recursive mode)
            result = runDirectPartitioning(hg, parse_time);
            total_timer.stop();
            std::cout << "Total time:              " << std::fixed << std::setprecision(3)
                      << std::setw(8) << total_timer.elapsed() << " s" << std::endl;
            std::cout << "====================================\n" << std::endl;
        } else {
            // Mode 2: Direct k-way partitioning
            result = runDirectPartitioning(hg, parse_time);
            total_timer.stop();
            std::cout << "Total time:              " << std::fixed << std::setprecision(3)
                      << std::setw(8) << total_timer.elapsed() << " s" << std::endl;
            std::cout << "====================================\n" << std::endl;
        }
        
        return result;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

} // namespace consmlp
