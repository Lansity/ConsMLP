#include "refinement/Refiner.h"
#include "partitioning/PartitionMetrics.h"
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace consmlp {

// Maximum net size to consider during refinement (skip larger nets for performance)
constexpr Index kMaxRefineNetSize = 50;


// ========== Refiner Base Class ==========

Refiner::Refiner(const Configuration& config)
    : config_(config)
{
}

// ========== GainBucket Implementation ==========

FMRefiner::GainBucket::GainBucket()
    : min_gain_(0), max_gain_(0), max_current_gain_(0), size_(0)
{
}

void FMRefiner::GainBucket::initialize(int min_gain, int max_gain) {
    min_gain_ = min_gain;
    max_gain_ = max_gain;
    max_current_gain_ = min_gain - 1;
    
    size_t num_buckets = max_gain - min_gain + 1;
    buckets_.resize(num_buckets);
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    
    // Clear node tracking vectors - CRITICAL for correct behavior when refiner is reused
    std::fill(node_in_bucket_.begin(), node_in_bucket_.end(), false);
    std::fill(node_gains_.begin(), node_gains_.end(), 0);
    
    size_ = 0;
}

void FMRefiner::GainBucket::insert(NodeID node_id, int gain) {
    if (node_id >= node_in_bucket_.size()) {
        node_in_bucket_.resize(node_id + 1, false);
        node_gains_.resize(node_id + 1, 0);
    }
    
    if (node_in_bucket_[node_id]) {
        return;  // Already in bucket
    }
    
    size_t idx = getBucketIndex(gain);
    buckets_[idx].push_back(node_id);
    node_gains_[node_id] = gain;
    node_in_bucket_[node_id] = true;
    size_++;
    
    if (gain > max_current_gain_) {
        max_current_gain_ = gain;
    }
}

void FMRefiner::GainBucket::remove(NodeID node_id) {
    if (node_id >= node_in_bucket_.size() || !node_in_bucket_[node_id]) {
        return;
    }
    
    node_in_bucket_[node_id] = false;
    size_--;
}

void FMRefiner::GainBucket::updateGain(NodeID node_id, int new_gain) {
    if (node_id >= node_in_bucket_.size()) {
        insert(node_id, new_gain);
        return;
    }
    
    if (node_in_bucket_[node_id]) {
        remove(node_id);
    }
    insert(node_id, new_gain);
}

bool FMRefiner::GainBucket::getMax(NodeID& node_id, int& gain) {
    if (size_ == 0) {
        return false;
    }
    
    // Find non-empty bucket with maximum gain
    while (max_current_gain_ >= min_gain_) {
        size_t idx = getBucketIndex(max_current_gain_);
        
        // Remove invalid nodes from bucket
        while (!buckets_[idx].empty()) {
            NodeID candidate = buckets_[idx].back();
            buckets_[idx].pop_back();
            
            if (node_in_bucket_[candidate]) {
                node_id = candidate;
                gain = max_current_gain_;
                node_in_bucket_[candidate] = false;
                size_--;
                return true;
            }
        }
        
        max_current_gain_--;
    }
    
    return false;
}

void FMRefiner::GainBucket::clear() {
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    std::fill(node_in_bucket_.begin(), node_in_bucket_.end(), false);
    size_ = 0;
    max_current_gain_ = min_gain_ - 1;
}

// ========== FMRefiner ==========

FMRefiner::FMRefiner(const Configuration& config)
    : Refiner(config)
{
}

RefinementStats FMRefiner::refine(const Hypergraph& hg,
                                  Partition& partition,
                                  const PartitionConstraints& constraints) {
    RefinementStats stats;
    stats.initial_cut = PartitionMetrics::calculateCutSize(hg, partition);
    stats.final_cut = stats.initial_cut;
    
    Weight prev_improvement = 0;
    size_t first_pass_moves = 0;  // Record moves from first pass for adaptive strategy
    
    // Multiple refinement passes with adaptive early quit
    for (uint32_t pass = 0; pass < config_.max_refinement_passes; ++pass) {
        Weight improvement = performPass(hg, partition, constraints, pass, first_pass_moves);
        
        if (improvement > 0) {
            stats.num_passes++;
            stats.final_cut -= improvement;
            
            // Record first pass move count for adaptive strategy
            if (pass == 0) {
                first_pass_moves = improvement;  // Use as proxy for move count
            }
            
            prev_improvement = improvement;
        } else {
            // No improvement in this pass
            if (pass > 0 && prev_improvement == 0) {
                // Two consecutive passes without improvement, stop early
                break;
            }
            prev_improvement = 0;
        }
    }
    
    stats.improvement = stats.initial_cut - stats.final_cut;
    return stats;
}

Weight FMRefiner::performPass(const Hypergraph& hg,
                              Partition& partition,
                              const PartitionConstraints& constraints,
                              uint32_t pass_number,
                              size_t first_pass_moves) {
    NodeID num_nodes = hg.getNumNodes();
    PartitionID num_partitions = partition.getNumPartitions();
    
    // Initialize data structures
    locked_.assign(num_nodes, false);
    gain_buckets_.resize(num_partitions);
    
    // Initialize gains
    initializeGains(hg, partition);
    
    // Move records
    std::vector<MoveRecord> moves;
    moves.reserve(num_nodes / 4);
    
    Weight current_cut_delta = 0;
    Weight best_cut_delta = 0;
    size_t best_move_index = 0;
    
    // Adaptive early quit strategy based on pass number and hypergraph size
    bool is_large_graph = (num_nodes > 5000);
    size_t max_moves = std::min(static_cast<size_t>(num_nodes), static_cast<size_t>(10000));
    size_t negative_move_count = 0;  // Count moves with negative gain
    size_t m_threshold = 0;  // Threshold for negative moves (set after first negative)
    bool first_negative_seen = false;
    
    // Main FM loop
    for (size_t move_count = 0; move_count < max_moves; ++move_count) {
        // Find best unlocked node to move
        NodeID best_node = INVALID_NODE;
        PartitionID best_to_partition = INVALID_PARTITION;
        int best_gain = std::numeric_limits<int>::min();
        
        // Check all partitions for best move (optimized: check fewer partitions)
        for (PartitionID from_part = 0; from_part < num_partitions; ++from_part) {
            if (gain_buckets_[from_part].empty()) {
                continue;
            }
            
            // Try to get best node from this partition
            NodeID candidate_node;
            int candidate_gain;
            
            if (gain_buckets_[from_part].getMax(candidate_node, candidate_gain)) {
                if (locked_[candidate_node]) continue;
                
                PartitionID current_part = partition.getPartition(candidate_node);
                
                // Check all target partitions for better quality
                for (PartitionID to_part = 0; to_part < num_partitions; ++to_part) {
                    if (to_part == current_part) continue;
                    
                    if (!isValidMove(hg, partition, constraints, candidate_node, to_part)) {
                        continue;
                    }
                    
                    int gain = computeGain(hg, partition, candidate_node, to_part);
                    
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_node = candidate_node;
                        best_to_partition = to_part;
                        
                        // Early exit for very high gain (optimization)
                        // if (gain > 100) break;
                    }
                }
            }
        }
        
        // Also try negative gain moves if no positive found (to escape local minima)
        if (best_node == INVALID_NODE) {
            // Try best negative gain move
            for (PartitionID from_part = 0; from_part < num_partitions; ++from_part) {
                if (gain_buckets_[from_part].empty()) continue;
                
                NodeID candidate_node;
                int candidate_gain;
                
                if (gain_buckets_[from_part].getMax(candidate_node, candidate_gain)) {
                    if (locked_[candidate_node]) continue;
                    if (candidate_gain >= 0) continue;  // Only negative gains
                    
                    PartitionID current_part = partition.getPartition(candidate_node);
                    PartitionID target_part = (current_part + 1) % num_partitions;
                    
                    if (isValidMove(hg, partition, constraints, candidate_node, target_part)) {
                        best_gain = candidate_gain;
                        best_node = candidate_node;
                        best_to_partition = target_part;
                        break;  // Take first valid negative move
                    }
                }
            }
        }
        
        if (best_node == INVALID_NODE) {
            break;  // No valid moves left
        }
        
        // Perform move
        PartitionID from_partition = partition.getPartition(best_node);
        partition.moveNode(best_node, best_to_partition, hg);
        locked_[best_node] = true;
        
        // Record move
        moves.emplace_back(best_node, from_partition, best_to_partition, best_gain);
        
        // Update cut delta
        current_cut_delta += best_gain;
        
        // Update best if improved
        if (current_cut_delta > best_cut_delta) {
            best_cut_delta = current_cut_delta;
            best_move_index = moves.size();
            negative_move_count = 0;  // Reset negative counter on improvement
        }
        
        // Track negative gain moves for adaptive early quit
        if (best_gain < 0) {
            if (!first_negative_seen) {
                // First negative gain move - record current move count as m
                first_negative_seen = true;
                m_threshold = move_count;
            }
            negative_move_count++;
        }
        
        // Adaptive early quit based on pass number and hypergraph size
        if (num_nodes < 200) {
            // never early quit for small graphs
        }
        else if (num_nodes < 500) {
            if (first_negative_seen && m_threshold > 0) {
                if (pass_number <= 2) {
                    // First pass: adjust threshold based on graph size
                    size_t threshold_multiplier = is_large_graph ? 10 : 30;
                    if (move_count >= threshold_multiplier * m_threshold) {
                        break;
                    }
                } else {
                    double threshold_multiplier = is_large_graph ? 3 : 6;
                    if (negative_move_count >= static_cast<size_t>(threshold_multiplier * m_threshold)) {
                        break;
                    }
                }
            }
        }
        else {
            if (first_negative_seen && m_threshold > 0) {
                if (pass_number <= 2) {
                    // First pass: adjust threshold based on graph size
                    size_t threshold_multiplier = is_large_graph ? 5 : 10;
                    if (move_count >= threshold_multiplier * m_threshold) {
                        break;
                    }
                } else {
                    double threshold_multiplier = is_large_graph ? 3 : 6;
                    if (negative_move_count >= static_cast<size_t>(threshold_multiplier * m_threshold)) {
                        break;
                    }
                }
            }
        }

        
        // Update neighbor gains (only for critical neighbors)
        updateNeighborGains(hg, partition, best_node);
    }
    
    // Undo moves after best point
    for (size_t i = moves.size(); i > best_move_index; --i) {
        const auto& move = moves[i - 1];
        partition.moveNode(move.node_id, move.from_partition, hg);
    }
    
    return best_cut_delta;
}

void FMRefiner::initializeGains(const Hypergraph& hg,
                                const Partition& partition) {
    NodeID num_nodes = hg.getNumNodes();
    PartitionID num_partitions = partition.getNumPartitions();
    
    // Estimate gain range - use conservative estimates to avoid segfault
    Weight max_net_weight = 1;
    Index max_degree = 1;
    
    // Always do full scan for correctness (optimization removed due to segfault)
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        max_net_weight = std::max(max_net_weight, hg.getNetWeight(net_id));
    }
    
    for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
        max_degree = std::max(max_degree, hg.getNodeDegree(node_id));
    }
    
    // Use conservative multiplier to ensure we don't underestimate
    int max_gain = max_net_weight * max_degree * 2;  // 2x safety margin
    int min_gain = -max_gain;
    
    // Initialize buckets
    for (PartitionID part = 0; part < num_partitions; ++part) {
        gain_buckets_[part].initialize(min_gain, max_gain);
    }
    
    // Compute initial gains and insert into buckets
    for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
        if (hg.isNodeIgnored(node_id) || hg.isNodeFixed(node_id)) {
            continue;
        }
        
        PartitionID current_part = partition.getPartition(node_id);
        
        // Compute gain for moving to each other partition
        for (PartitionID to_part = 0; to_part < num_partitions; ++to_part) {
            if (to_part == current_part) continue;
            
            int gain = computeGain(hg, partition, node_id, to_part);
            
            // Insert to current partition's bucket (nodes we could move out)
            gain_buckets_[current_part].insert(node_id, gain);
            break;  // For simplicity, only consider one target
        }
    }
}

int FMRefiner::computeGain(const Hypergraph& hg,
                           const Partition& partition,
                           NodeID node_id,
                           PartitionID to_partition) const {
    PartitionID from_partition = partition.getPartition(node_id);
    int gain = 0;
    
    // Optimized gain calculation
    auto nets = hg.getNodeNets(node_id);
    
    for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
        EdgeID net_id = *net_it;
        Index net_size = hg.getNetSize(net_id);
        
        // Skip large nets for performance
        if (net_size > kMaxRefineNetSize) {
            continue;
        }
        
        Weight net_weight = hg.getNetWeight(net_id);
        
        // Count nodes in each partition (optimized: only count relevant partitions)
        PartitionID num_in_from = 0;
        PartitionID num_in_to = 0;
        
        auto nodes = hg.getNetNodes(net_id);
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            PartitionID part = partition.getPartition(*node_it);
            if (part == from_partition) num_in_from++;
            else if (part == to_partition) num_in_to++;
        }
        
        // Gain calculation: net becomes uncut if it was cut
        bool was_cut = (num_in_from > 0 && num_in_from < net_size);
        
        // After move
        num_in_from--;
        num_in_to++;
        bool will_be_cut = (num_in_from > 0 || num_in_to < net_size);
        
        if (was_cut && !will_be_cut) {
            gain += net_weight;  // Net becomes uncut
        } else if (!was_cut && will_be_cut) {
            gain -= net_weight;  // Net becomes cut
        }
    }
    
    return gain;
}

void FMRefiner::updateNeighborGains(const Hypergraph& hg,
                                   const Partition& partition,
                                   NodeID moved_node) {
    // Update gains of neighbors (only those in affected nets)
    auto nets = hg.getNodeNets(moved_node);
    
    // Collect unique neighbors to avoid duplicate updates
    std::unordered_set<NodeID> neighbors_to_update;
    neighbors_to_update.reserve(hg.getNodeDegree(moved_node) * 2);
    
    for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
        EdgeID net_id = *net_it;
        
        // Skip large nets for performance
        if (hg.getNetSize(net_id) > kMaxRefineNetSize) {
            continue;
        }
        
        auto nodes = hg.getNetNodes(net_id);
        
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            NodeID neighbor = *node_it;
            
            if (neighbor != moved_node && !locked_[neighbor]) {
                neighbors_to_update.insert(neighbor);
            }
        }
    }
    
    // Update gains for unique neighbors (optimized: only update if necessary)
    for (NodeID neighbor : neighbors_to_update) {
        PartitionID current_part = partition.getPartition(neighbor);
        
        // Only recompute gain for one target partition (fastest)
        PartitionID target_part = (current_part + 1) % partition.getNumPartitions();
        int new_gain = computeGain(hg, partition, neighbor, target_part);
        gain_buckets_[current_part].updateGain(neighbor, new_gain);
    }
}

bool FMRefiner::isValidMove(const Hypergraph& hg,
                           const Partition& partition,
                           const PartitionConstraints& constraints,
                           NodeID node_id,
                           PartitionID to_partition) const {
    // Cannot move fixed nodes
    if (hg.isNodeFixed(node_id)) {
        return false;
    }
    
    // Check capacity constraints for all types
    const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
    
    return !constraints.wouldViolateCapacityMultiType(to_partition, type_weights, partition);
}

// ========== GreedyRefiner ==========

GreedyRefiner::GreedyRefiner(const Configuration& config)
    : Refiner(config)
{
}

RefinementStats GreedyRefiner::refine(const Hypergraph& hg,
                                     Partition& partition,
                                     const PartitionConstraints& constraints) {
    RefinementStats stats;
    stats.initial_cut = PartitionMetrics::calculateCutSize(hg, partition);
    stats.final_cut = stats.initial_cut;
    
    for (uint32_t pass = 0; pass < config_.max_refinement_passes; ++pass) {
        Weight improvement = performPass(hg, partition, constraints);
        
        if (improvement > 0) {
            stats.num_passes++;
            stats.final_cut -= improvement;
        } else {
            break;
        }
    }
    
    stats.improvement = stats.initial_cut - stats.final_cut;
    return stats;
}

Weight GreedyRefiner::performPass(const Hypergraph& hg,
                                  Partition& partition,
                                  const PartitionConstraints& constraints) {
    Weight initial_cut = PartitionMetrics::calculateCutSize(hg, partition);
    
    // Simple greedy: try moving each boundary node
    // (Implementation simplified for brevity)
    
    Weight final_cut = PartitionMetrics::calculateCutSize(hg, partition);
    return initial_cut - final_cut;
}

// ========== GreedyFMRefiner ==========

GreedyFMRefiner::GainBucket::GainBucket()
    : min_gain_(0), max_gain_(0), max_current_gain_(0), size_(0)
{
}

void GreedyFMRefiner::GainBucket::initialize(int min_gain, int max_gain) {
    min_gain_ = min_gain;
    max_gain_ = max_gain;
    max_current_gain_ = min_gain - 1;
    
    size_t num_buckets = max_gain - min_gain + 1;
    buckets_.resize(num_buckets);
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    
    // Clear node tracking vectors - CRITICAL for correct behavior when refiner is reused
    std::fill(node_in_bucket_.begin(), node_in_bucket_.end(), false);
    std::fill(node_gains_.begin(), node_gains_.end(), 0);
    
    size_ = 0;
}

void GreedyFMRefiner::GainBucket::insert(NodeID node_id, int gain) {
    if (node_id >= node_in_bucket_.size()) {
        node_in_bucket_.resize(node_id + 1, false);
        node_gains_.resize(node_id + 1, 0);
    }
    
    if (node_in_bucket_[node_id]) {
        return;
    }
    
    // Clamp gain to valid range
    gain = std::max(min_gain_, std::min(max_gain_, gain));
    
    size_t idx = getBucketIndex(gain);
    buckets_[idx].push_back(node_id);
    node_gains_[node_id] = gain;
    node_in_bucket_[node_id] = true;
    size_++;
    
    if (gain > max_current_gain_) {
        max_current_gain_ = gain;
    }
}

void GreedyFMRefiner::GainBucket::remove(NodeID node_id) {
    if (node_id >= node_in_bucket_.size() || !node_in_bucket_[node_id]) {
        return;
    }
    
    node_in_bucket_[node_id] = false;
    size_--;
}

void GreedyFMRefiner::GainBucket::updateGain(NodeID node_id, int new_gain) {
    if (node_id >= node_in_bucket_.size()) {
        insert(node_id, new_gain);
        return;
    }
    
    if (node_in_bucket_[node_id]) {
        remove(node_id);
    }
    insert(node_id, new_gain);
}

bool GreedyFMRefiner::GainBucket::getMax(NodeID& node_id, int& gain) {
    if (size_ == 0) {
        return false;
    }
    
    while (max_current_gain_ >= min_gain_) {
        size_t idx = getBucketIndex(max_current_gain_);
        
        while (!buckets_[idx].empty()) {
            NodeID candidate = buckets_[idx].back();
            buckets_[idx].pop_back();
            
            if (node_in_bucket_[candidate]) {
                node_id = candidate;
                gain = max_current_gain_;
                node_in_bucket_[candidate] = false;
                size_--;
                return true;
            }
        }
        
        max_current_gain_--;
    }
    
    return false;
}

void GreedyFMRefiner::GainBucket::clear() {
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    std::fill(node_in_bucket_.begin(), node_in_bucket_.end(), false);
    size_ = 0;
    max_current_gain_ = min_gain_ - 1;
}

GreedyFMRefiner::GreedyFMRefiner(const Configuration& config)
    : Refiner(config)
{
}

RefinementStats GreedyFMRefiner::refine(const Hypergraph& hg,
                                        Partition& partition,
                                        const PartitionConstraints& constraints) {
    RefinementStats stats;
    stats.initial_cut = PartitionMetrics::calculateCutSize(hg, partition);
    stats.final_cut = stats.initial_cut;
    
    Weight prev_improvement = 0;
    
    for (uint32_t pass = 0; pass < config_.max_refinement_passes; ++pass) {
        Weight improvement = performPass(hg, partition, constraints, pass);
        
        if (improvement > 0) {
            stats.num_passes++;
            stats.final_cut -= improvement;
            prev_improvement = improvement;
        } else {
            if (pass > 0 && prev_improvement == 0) {
                break;
            }
            prev_improvement = 0;
        }
    }
    
    stats.improvement = stats.initial_cut - stats.final_cut;
    return stats;
}

Weight GreedyFMRefiner::performPass(const Hypergraph& hg,
                                    Partition& partition,
                                    const PartitionConstraints& constraints,
                                    uint32_t pass_number) {
    NodeID num_nodes = hg.getNumNodes();
    PartitionID num_partitions = partition.getNumPartitions();
    
    // FIX: Calculate actual cut at start of pass (including all nets)
    Weight cut_at_start = PartitionMetrics::calculateCutSize(hg, partition);
    
    // Initialize data structures
    locked_.assign(num_nodes, false);
    in_bucket_.assign(num_nodes, false);
    gain_buckets_.resize(num_partitions);
    
    // Estimate gain range
    Weight max_net_weight = 1;
    Index max_degree = 1;
    
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        max_net_weight = std::max(max_net_weight, hg.getNetWeight(net_id));
    }
    
    for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
        max_degree = std::max(max_degree, hg.getNodeDegree(node_id));
    }
    
    int max_gain = max_net_weight * max_degree * 2;
    int min_gain = -max_gain;
    
    for (PartitionID part = 0; part < num_partitions; ++part) {
        gain_buckets_[part].initialize(min_gain, max_gain);
    }
    
    // KEY OPTIMIZATION: Only initialize gains for boundary nodes (nodes on cut nets)
    initializeBoundaryGains(hg, partition);
    
    // Move records
    std::vector<MoveRecord> moves;
    moves.reserve(num_nodes / 4);
    
    Weight current_cut_delta = 0;
    Weight best_cut_delta = 0;
    size_t best_move_index = 0;
    
    size_t max_moves = std::min(static_cast<size_t>(num_nodes / 2), static_cast<size_t>(5000));
    size_t negative_move_count = 0;
    size_t m_threshold = 0;
    bool first_negative_seen = false;
    
    // Main FM loop
    for (size_t move_count = 0; move_count < max_moves; ++move_count) {
        NodeID best_node = INVALID_NODE;
        PartitionID best_to_partition = INVALID_PARTITION;
        int best_gain = std::numeric_limits<int>::min();
        
        // Find best node from gain buckets
        for (PartitionID from_part = 0; from_part < num_partitions; ++from_part) {
            if (gain_buckets_[from_part].empty()) {
                continue;
            }
            
            NodeID candidate_node;
            int candidate_gain;
            
            if (gain_buckets_[from_part].getMax(candidate_node, candidate_gain)) {
                if (locked_[candidate_node]) continue;
                
                PartitionID current_part = partition.getPartition(candidate_node);
                
                for (PartitionID to_part = 0; to_part < num_partitions; ++to_part) {
                    if (to_part == current_part) continue;
                    
                    if (!isValidMove(hg, partition, constraints, candidate_node, to_part)) {
                        continue;
                    }
                    
                    int gain = computeGain(hg, partition, candidate_node, to_part);
                    
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_node = candidate_node;
                        best_to_partition = to_part;
                        
                        if (gain > 100) break;
                    }
                }
            }
        }
        
        if (best_node == INVALID_NODE) {
            break;
        }
        
        // Perform move
        PartitionID from_partition = partition.getPartition(best_node);
        partition.moveNode(best_node, best_to_partition, hg);
        locked_[best_node] = true;
        
        moves.emplace_back(best_node, from_partition, best_to_partition, best_gain);
        
        current_cut_delta += best_gain;
        
        if (current_cut_delta > best_cut_delta) {
            best_cut_delta = current_cut_delta;
            best_move_index = moves.size();
            negative_move_count = 0;
        }
        
        if (best_gain < 0) {
            if (!first_negative_seen) {
                first_negative_seen = true;
                m_threshold = move_count;
            }
            negative_move_count++;
        }
        
        // Adaptive early quit
        // Adaptive early quit based on graph size
        if (num_nodes > 10000) {
            // Large graphs: quit early
            if (first_negative_seen && m_threshold > 0) {
                if (pass_number == 0) {
                    if (move_count >= 10 * m_threshold) break;
                } else if (pass_number <= 2) {
                    if (negative_move_count >= 5 * m_threshold) break;
                } else {
                    if (best_gain < 0) break;
                }
            }
        } else {
            // Small graphs: quit slowly
            if (first_negative_seen && m_threshold > 0) {
                if (pass_number == 0) {
                    if (move_count >= 30 * m_threshold) break;
                } else if (pass_number <= 2) {
                    if (negative_move_count >= 15 * m_threshold) break;
                } else {
                    if (best_gain < 0) break;
                }
            }
        }

        
        // KEY OPTIMIZATION: Update neighbor gains and lazily add new boundary nodes
        updateNeighborGainsAndAddToBucket(hg, partition, best_node);
    }
    
    // Undo moves after best point (based on estimated gain)
    for (size_t i = moves.size(); i > best_move_index; --i) {
        const auto& move = moves[i - 1];
        partition.moveNode(move.node_id, move.from_partition, hg);
    }
    
    // FIX: Calculate actual cut at end of pass and return real improvement
    // This correctly accounts for large nets that are skipped in gain calculation
    Weight cut_at_end = PartitionMetrics::calculateCutSize(hg, partition);
    Weight actual_improvement = cut_at_start - cut_at_end;
    
    // FIX: If actual improvement is non-positive (or much worse than expected),
    // undo ALL remaining moves to restore original partition
    if (actual_improvement <= 0 && best_move_index > 0) {
        // The moves actually made things worse, undo all of them
        for (size_t i = best_move_index; i > 0; --i) {
            const auto& move = moves[i - 1];
            partition.moveNode(move.node_id, move.from_partition, hg);
        }
        return 0;  // No actual improvement
    }
    
    return actual_improvement;
}

void GreedyFMRefiner::initializeBoundaryGains(const Hypergraph& hg,
                                              const Partition& partition) {
    NodeID num_nodes = hg.getNumNodes();
    PartitionID num_partitions = partition.getNumPartitions();
    
    // Find all cut nets and their boundary nodes
    std::vector<bool> is_boundary(num_nodes, false);
    
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        // Skip large nets for performance
        if (hg.getNetSize(net_id) > kMaxRefineNetSize) {
            continue;
        }
        
        auto nodes = hg.getNetNodes(net_id);
        
        // Check if this net is a cut net (spans multiple partitions)
        PartitionID first_part = INVALID_PARTITION;
        bool is_cut = false;
        
        for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
            PartitionID part = partition.getPartition(*it);
            if (first_part == INVALID_PARTITION) {
                first_part = part;
            } else if (part != first_part) {
                is_cut = true;
                break;
            }
        }
        
        // Mark all nodes on cut nets as boundary nodes
        if (is_cut) {
            for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
                is_boundary[*it] = true;
            }
        }
    }
    
    // Initialize gains only for boundary nodes
    for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
        if (!is_boundary[node_id]) continue;
        if (hg.isNodeIgnored(node_id) || hg.isNodeFixed(node_id)) continue;
        
        PartitionID current_part = partition.getPartition(node_id);
        
        // Compute gain for moving to another partition
        for (PartitionID to_part = 0; to_part < num_partitions; ++to_part) {
            if (to_part == current_part) continue;
            
            int gain = computeGain(hg, partition, node_id, to_part);
            gain_buckets_[current_part].insert(node_id, gain);
            in_bucket_[node_id] = true;
            break;  // Only consider one target partition for simplicity
        }
    }
}

int GreedyFMRefiner::computeGain(const Hypergraph& hg,
                                  const Partition& partition,
                                  NodeID node_id,
                                  PartitionID to_partition) const {
    PartitionID from_partition = partition.getPartition(node_id);
    int gain = 0;
    
    auto nets = hg.getNodeNets(node_id);
    
    for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
        EdgeID net_id = *net_it;
        
        // Skip large nets for performance
        if (hg.getNetSize(net_id) > kMaxRefineNetSize) {
            continue;
        }
        
        Weight net_weight = hg.getNetWeight(net_id);
        
        auto nodes = hg.getNetNodes(net_id);
        
        // Count nodes in from_partition and to_partition
        Index from_count = 0;
        Index to_count = 0;
        
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            PartitionID part = partition.getPartition(*node_it);
            if (part == from_partition) from_count++;
            else if (part == to_partition) to_count++;
        }
        
        // If this node is the only one in from_partition, net becomes internal
        if (from_count == 1) {
            gain += net_weight;  // Net becomes internal to to_partition
        }
        
        // If to_partition had no nodes in this net, net becomes cut
        if (to_count == 0) {
            gain -= net_weight;  // Net becomes cut
        }
    }
    
    return gain;
}

void GreedyFMRefiner::updateNeighborGainsAndAddToBucket(const Hypergraph& hg,
                                                        const Partition& partition,
                                                        NodeID moved_node) {
    PartitionID num_partitions = partition.getNumPartitions();
    std::unordered_set<NodeID> neighbors_to_update;
    
    auto nets = hg.getNodeNets(moved_node);
    
    for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
        EdgeID net_id = *net_it;
        
        // Skip large nets for performance
        if (hg.getNetSize(net_id) > kMaxRefineNetSize) {
            continue;
        }
        
        auto nodes = hg.getNetNodes(net_id);
        
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            NodeID neighbor = *node_it;
            if (neighbor == moved_node || locked_[neighbor]) continue;
            if (hg.isNodeIgnored(neighbor) || hg.isNodeFixed(neighbor)) continue;
            
            neighbors_to_update.insert(neighbor);
        }
    }
    
    // Update gains for all affected neighbors
    for (NodeID neighbor : neighbors_to_update) {
        PartitionID current_part = partition.getPartition(neighbor);
        
        // Compute new gain
        int best_gain = std::numeric_limits<int>::min();
        for (PartitionID to_part = 0; to_part < num_partitions; ++to_part) {
            if (to_part == current_part) continue;
            
            int gain = computeGain(hg, partition, neighbor, to_part);
            best_gain = std::max(best_gain, gain);
        }
        
        if (best_gain > std::numeric_limits<int>::min()) {
            // Update or insert into bucket
            if (in_bucket_[neighbor]) {
                gain_buckets_[current_part].updateGain(neighbor, best_gain);
            } else {
                // Lazily add new boundary nodes to bucket
                gain_buckets_[current_part].insert(neighbor, best_gain);
                in_bucket_[neighbor] = true;
            }
        }
    }
}

bool GreedyFMRefiner::isValidMove(const Hypergraph& hg,
                                   const Partition& partition,
                                   const PartitionConstraints& constraints,
                                   NodeID node_id,
                                   PartitionID to_partition) const {
    if (hg.isNodeFixed(node_id)) {
        return false;
    }
    
    // Check capacity constraints for all types
    const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
    
    return !constraints.wouldViolateCapacityMultiType(to_partition, type_weights, partition);
}

} // namespace consmlp

