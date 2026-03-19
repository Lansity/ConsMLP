#include "partitioning/Partitioner.h"
#include <random>
#include <algorithm>
#include <queue>
#include <unordered_set>
#include <limits>

namespace consmlp {

// ========== Partitioner Base Class ==========

Partitioner::Partitioner(const Configuration& config)
    : config_(config)
{
}

void Partitioner::applyFixedConstraints(const Hypergraph& hg,
                                       const PartitionConstraints& constraints,
                                       Partition& partition) {
    // Apply fixed node constraints
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (constraints.isNodeFixed(node_id)) {
            PartitionID fixed_partition = constraints.getFixedPartition(node_id);
            partition.setPartition(node_id, fixed_partition, hg);
        }
    }
}

// ========== GreedyPartitioner ==========

GreedyPartitioner::GreedyPartitioner(const Configuration& config)
    : Partitioner(config)
{
}

Partition GreedyPartitioner::partition(const Hypergraph& hg,
                                      const PartitionConstraints& constraints) {
    Partition result(hg.getNumNodes(), config_.num_partitions);
    
    // First, apply fixed constraints
    applyFixedConstraints(hg, constraints, result);
    
    // Greedy assignment: assign each non-fixed node to least-loaded partition
    // that doesn't violate capacity constraints
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        // Skip ignored nodes
        if (hg.isNodeIgnored(node_id)) {
            continue;
        }
        
        // Skip fixed nodes (already assigned)
        if (constraints.isNodeFixed(node_id)) {
            continue;
        }
        
        const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
        
        // Find best partition (one with minimum weight that doesn't violate capacity)
        PartitionID best_partition = INVALID_PARTITION;
        Weight min_weight = std::numeric_limits<Weight>::max();
        
        for (PartitionID part_id = 0; part_id < config_.num_partitions; ++part_id) {
            // Check capacity constraint for all types
            if (constraints.wouldViolateCapacityMultiType(part_id, type_weights, result)) {
                continue;
            }
            
            // Choose partition with minimum weight
            Weight total_weight = result.getPartitionWeight(part_id);
            if (total_weight < min_weight) {
                min_weight = total_weight;
                best_partition = part_id;
            }
        }
        
        // Assign to best partition (or partition 0 if no valid partition found)
        if (best_partition == INVALID_PARTITION) {
            best_partition = 0;  // Fallback
        }
        
        result.setPartition(node_id, best_partition, hg);
    }
    
    return result;
}

// ========== RandomPartitioner ==========

RandomPartitioner::RandomPartitioner(const Configuration& config)
    : Partitioner(config)
{
}

Partition RandomPartitioner::partition(const Hypergraph& hg,
                                      const PartitionConstraints& constraints) {
    Partition result(hg.getNumNodes(), config_.num_partitions);
    
    // Apply fixed constraints
    applyFixedConstraints(hg, constraints, result);
    
    // Random generator
    std::mt19937 rng(config_.seed);
    
    // Create list of non-fixed, non-ignored nodes
    std::vector<NodeID> free_nodes;
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (!hg.isNodeIgnored(node_id) && !constraints.isNodeFixed(node_id)) {
            free_nodes.push_back(node_id);
        }
    }
    
    // Sort nodes by weight in descending order (large nodes first)
    // This helps ensure large nodes get placed before partitions fill up
    std::sort(free_nodes.begin(), free_nodes.end(), [&hg](NodeID a, NodeID b) {
        return hg.getNodeWeight(a) > hg.getNodeWeight(b);
    });
    
    // Group nodes by type for better type-aware distribution
    std::vector<std::vector<NodeID>> nodes_by_type(static_cast<int>(NodeType::OTHER) + 1);
    for (NodeID node_id : free_nodes) {
        NodeType type = hg.getNodeType(node_id);
        nodes_by_type[static_cast<int>(type)].push_back(node_id);
    }
    
    // Shuffle within each type group to add randomness while maintaining weight order
    for (auto& type_nodes : nodes_by_type) {
        if (type_nodes.size() > 1) {
            // Partial shuffle: swap adjacent pairs randomly to maintain rough weight order
            for (size_t i = 0; i + 1 < type_nodes.size(); i += 2) {
                if (rng() % 2 == 0) {
                    std::swap(type_nodes[i], type_nodes[i + 1]);
                }
            }
        }
    }
    
    // Interleave nodes from different types for balanced distribution
    std::vector<NodeID> interleaved_nodes;
    interleaved_nodes.reserve(free_nodes.size());
    
    bool has_more = true;
    size_t max_idx = 0;
    while (has_more) {
        has_more = false;
        for (auto& type_nodes : nodes_by_type) {
            if (max_idx < type_nodes.size()) {
                interleaved_nodes.push_back(type_nodes[max_idx]);
                has_more = true;
            }
        }
        max_idx++;
    }
    
    // Track current weights per partition per type for balance checking
    std::vector<std::vector<Weight>> partition_type_weights(
        config_.num_partitions,
        std::vector<Weight>(static_cast<int>(NodeType::OTHER) + 1, 0)
    );
    
    // Initialize with fixed node weights
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (constraints.isNodeFixed(node_id)) {
            PartitionID part = result.getPartition(node_id);
            NodeType type = hg.getNodeType(node_id);
            Weight weight = hg.getNodeWeight(node_id);
            partition_type_weights[part][static_cast<int>(type)] += weight;
        }
    }
    
    // Assign nodes with balance-aware random selection
    for (NodeID node_id : interleaved_nodes) {
        const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
        Weight node_weight = hg.getNodeWeight(node_id);
        NodeType primary_type = hg.getNodeType(node_id);
        int type_idx = static_cast<int>(primary_type);
        
        // Collect valid partitions that don't violate capacity for all types
        std::vector<PartitionID> valid_partitions;
        std::vector<Weight> partition_loads;  // Current load for primary type
        
        for (PartitionID part_id = 0; part_id < config_.num_partitions; ++part_id) {
            // Check multi-type constraints using Partition object (which tracks correctly)
            if (!constraints.wouldViolateCapacityMultiType(part_id, type_weights, result)) {
                valid_partitions.push_back(part_id);
                partition_loads.push_back(partition_type_weights[part_id][type_idx]);
            }
        }
        
        PartitionID chosen_partition = 0;
        
        if (!valid_partitions.empty()) {
            // Find partition with minimum load for this type (greedy balance)
            // Then add randomness among partitions with similar load
            Weight min_load = *std::min_element(partition_loads.begin(), partition_loads.end());
            Weight load_threshold = min_load + node_weight;  // Allow some slack
            
            std::vector<PartitionID> best_partitions;
            for (size_t i = 0; i < valid_partitions.size(); ++i) {
                if (partition_loads[i] <= load_threshold) {
                    best_partitions.push_back(valid_partitions[i]);
                }
            }
            
            // Randomly select from best partitions
            if (!best_partitions.empty()) {
                std::uniform_int_distribution<size_t> dist(0, best_partitions.size() - 1);
                chosen_partition = best_partitions[dist(rng)];
            } else {
                // Fallback: random from valid partitions
                std::uniform_int_distribution<size_t> dist(0, valid_partitions.size() - 1);
                chosen_partition = valid_partitions[dist(rng)];
            }
        } else {
            // No valid partition found, choose least loaded partition for this type
            Weight min_load = std::numeric_limits<Weight>::max();
            for (PartitionID part_id = 0; part_id < config_.num_partitions; ++part_id) {
                Weight load = partition_type_weights[part_id][type_idx];
                if (load < min_load) {
                    min_load = load;
                    chosen_partition = part_id;
                }
            }
        }
        
        // Assign node
        result.setPartition(node_id, chosen_partition, hg);
        partition_type_weights[chosen_partition][type_idx] += node_weight;
    }
    
    return result;
}

// ========== GHGPartitioner (Greedy Hypergraph Growing) ==========

GHGPartitioner::GHGPartitioner(const Configuration& config)
    : Partitioner(config)
{
}

Partition GHGPartitioner::partition(const Hypergraph& hg,
                                    const PartitionConstraints& constraints) {
    // GHG only works for bipartitioning (k=2)
    if (config_.num_partitions != 2) {
        // Fall back to greedy partitioner for k > 2
        GreedyPartitioner greedy(config_);
        return greedy.partition(hg, constraints);
    }
    
    Partition result(hg.getNumNodes(), 2);
    
    // Apply fixed constraints first
    applyFixedConstraints(hg, constraints, result);
    
    // Always use per-type constraint checking
    // All modes now set proper per-type constraints:
    // - Default mode: only LUT type has balance constraint, others unlimited
    // - Types mode: each type has its own balance constraint
    // - XML mode: each type has capacity constraint from XML
    
    // Track which nodes are assigned
    std::vector<bool> assigned(hg.getNumNodes(), false);
    
    // Mark fixed nodes as assigned
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (constraints.isNodeFixed(node_id) || hg.isNodeIgnored(node_id)) {
            assigned[node_id] = true;
        }
    }
    
    // Find seed node: choose node with highest degree (most connections)
    NodeID seed_node = INVALID_NODE;
    Index max_degree = 0;
    
    std::mt19937 rng(config_.seed);
    std::vector<NodeID> candidates;
    
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (assigned[node_id]) continue;
        
        Index degree = hg.getNodeDegree(node_id);
        if (degree > max_degree) {
            max_degree = degree;
            candidates.clear();
            candidates.push_back(node_id);
        } else if (degree == max_degree) {
            candidates.push_back(node_id);
        }
    }
    
    if (!candidates.empty()) {
        std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
        seed_node = candidates[dist(rng)];
    }
    
    if (seed_node == INVALID_NODE) {
        // No free nodes, return current partition
        return result;
    }
    
    // Initialize partition 0 with seed node
    result.setPartition(seed_node, 0, hg);
    assigned[seed_node] = true;
    Weight part0_weight = hg.getNodeWeight(seed_node);
    
    // Priority queue: (gain, node_id) - max heap by gain
    using GainPair = std::pair<int, NodeID>;
    std::priority_queue<GainPair> pq;
    
    // Track nodes in the frontier (neighbors of partition 0)
    std::unordered_set<NodeID> in_frontier;
    
    // Add neighbors of seed node to frontier
    auto seed_nets = hg.getNodeNets(seed_node);
    for (const EdgeID* net_it = seed_nets.first; net_it != seed_nets.second; ++net_it) {
        auto nodes = hg.getNetNodes(*net_it);
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            NodeID neighbor = *node_it;
            if (!assigned[neighbor] && in_frontier.find(neighbor) == in_frontier.end()) {
                int gain = computeGain(hg, result, neighbor);
                pq.push(std::make_pair(gain, neighbor));
                in_frontier.insert(neighbor);
            }
        }
    }
    
    // Calculate target weight for partition 0 (used for stopping condition)
    // In XML constraint mode, use the actual constraint max_capacity
    // Otherwise, use imbalance_factor based calculation
    Weight approx_max_weight;
    if (constraints.isXMLConstraintMode()) {
        // Use XML constraint max_capacity for partition 0 (LUT type as primary)
        approx_max_weight = constraints.getCapacity(0, NodeType::LUT).max_capacity;
    } else {
        Weight total_weight = hg.getTotalNodeWeight();
        Weight target_weight = total_weight / 2;
        approx_max_weight = static_cast<Weight>(target_weight * (1.0 + config_.imbalance_factor));
    }
    
    // Greedily grow partition 0
    while (!pq.empty() && part0_weight < approx_max_weight) {
        // Get node with highest gain
        GainPair top = pq.top();
        pq.pop();
        
        NodeID node_id = top.second;
        
        // Skip if already assigned
        if (assigned[node_id]) {
            continue;
        }
        
        // Recompute gain (may have changed)
        int current_gain = computeGain(hg, result, node_id);
        
        // Check if gain changed significantly (lazy update)
        if (current_gain < top.first - 1) {
            // Re-insert with updated gain
            pq.push(std::make_pair(current_gain, node_id));
            continue;
        }
        
        Weight node_weight = hg.getNodeWeight(node_id);
        const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
        
        // Check per-type capacity constraints for all types
        if (constraints.wouldViolateCapacityMultiType(0, type_weights, result)) {
            continue;  // Skip this node, would violate type capacity
        }
        
        // Add node to partition 0
        result.setPartition(node_id, 0, hg);
        assigned[node_id] = true;
        part0_weight += node_weight;
        
        // Add new neighbors to frontier
        auto node_nets = hg.getNodeNets(node_id);
        for (const EdgeID* net_it = node_nets.first; net_it != node_nets.second; ++net_it) {
            auto nodes = hg.getNetNodes(*net_it);
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID neighbor = *node_it;
                if (!assigned[neighbor] && in_frontier.find(neighbor) == in_frontier.end()) {
                    int gain = computeGain(hg, result, neighbor);
                    pq.push(std::make_pair(gain, neighbor));
                    in_frontier.insert(neighbor);
                }
            }
        }
    }
    
    // Assign remaining nodes, checking constraints for all types
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (!assigned[node_id]) {
            const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
            
            // Try partition 1 first (since partition 0 was grown greedily)
            if (!constraints.wouldViolateCapacityMultiType(1, type_weights, result)) {
                result.setPartition(node_id, 1, hg);
            } else {
                // Try partition 0
                if (!constraints.wouldViolateCapacityMultiType(0, type_weights, result)) {
                    result.setPartition(node_id, 0, hg);
                } else {
                    // Fallback: assign to partition 1 anyway (best effort)
                    result.setPartition(node_id, 1, hg);
                }
            }
        }
    }
    
    return result;
}

int GHGPartitioner::computeGain(const Hypergraph& hg,
                                 const Partition& partition,
                                 NodeID node_id) const {
    int gain = 0;
    
    auto nets = hg.getNodeNets(node_id);
    for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
        EdgeID net_id = *net_it;
        Weight net_weight = hg.getNetWeight(net_id);
        
        // Count nodes in partition 0 and partition 1
        Index count_in_0 = 0;
        Index count_in_1 = 0;
        
        auto nodes = hg.getNetNodes(net_id);
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            if (*node_it == node_id) continue;
            
            PartitionID part = partition.getPartition(*node_it);
            if (part == 0) count_in_0++;
            else if (part == 1) count_in_1++;
            // Unassigned nodes are implicitly in partition 1
            else count_in_1++;
        }
        
        // If all other nodes are in partition 0, moving this node there
        // makes the net internal (reduces cut)
        if (count_in_1 == 0 && count_in_0 > 0) {
            gain += net_weight;  // Net becomes internal
        }
        // If this is the first node from partition 0 in this net,
        // moving it there creates a cut
        else if (count_in_0 == 0 && count_in_1 > 0) {
            gain -= net_weight;  // Net becomes cut
        }
    }
    
    return gain;
}

// ========== GHGOptPartitioner (Optimized GHG with cut-aware BFS) ==========

GHGOptPartitioner::GHGOptPartitioner(const Configuration& config)
    : Partitioner(config)
{
}

NodeID GHGOptPartitioner::selectSeedNode(const Hypergraph& hg,
                                          const std::vector<bool>& assigned,
                                          int strategy) const {
    std::mt19937 rng(config_.seed + strategy * 1000);
    
    std::vector<NodeID> candidates;
    
    switch (strategy) {
        case 0: {
            // Strategy 0: Highest degree node
            Index max_degree = 0;
            for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
                if (assigned[node_id]) continue;
                Index degree = hg.getNodeDegree(node_id);
                if (degree > max_degree) {
                    max_degree = degree;
                    candidates.clear();
                    candidates.push_back(node_id);
                } else if (degree == max_degree) {
                    candidates.push_back(node_id);
                }
            }
            break;
        }
        case 1: {
            // Strategy 1: Lowest degree node (more balanced growth)
            Index min_degree = std::numeric_limits<Index>::max();
            for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
                if (assigned[node_id]) continue;
                Index degree = hg.getNodeDegree(node_id);
                if (degree > 0 && degree < min_degree) {
                    min_degree = degree;
                    candidates.clear();
                    candidates.push_back(node_id);
                } else if (degree == min_degree) {
                    candidates.push_back(node_id);
                }
            }
            break;
        }
        case 2: {
            // Strategy 2: Heaviest node
            Weight max_weight = 0;
            for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
                if (assigned[node_id]) continue;
                Weight weight = hg.getNodeWeight(node_id);
                if (weight > max_weight) {
                    max_weight = weight;
                    candidates.clear();
                    candidates.push_back(node_id);
                } else if (weight == max_weight) {
                    candidates.push_back(node_id);
                }
            }
            break;
        }
        case 3: {
            // Strategy 3: Random node
            for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
                if (!assigned[node_id]) {
                    candidates.push_back(node_id);
                }
            }
            break;
        }
        case 4:
        default: {
            // Strategy 4: Node with median degree
            std::vector<std::pair<Index, NodeID>> degree_nodes;
            for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
                if (!assigned[node_id]) {
                    degree_nodes.emplace_back(hg.getNodeDegree(node_id), node_id);
                }
            }
            if (!degree_nodes.empty()) {
                std::sort(degree_nodes.begin(), degree_nodes.end());
                size_t mid = degree_nodes.size() / 2;
                candidates.push_back(degree_nodes[mid].second);
            }
            break;
        }
    }
    
    if (candidates.empty()) {
        return INVALID_NODE;
    }
    
    std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
    return candidates[dist(rng)];
}

int GHGOptPartitioner::computeCutDelta(const Hypergraph& hg,
                                        const Partition& partition,
                                        NodeID node_id) const {
    int delta = 0;
    
    auto nets = hg.getNodeNets(node_id);
    for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
        EdgeID net_id = *net_it;
        Weight net_weight = hg.getNetWeight(net_id);
        
        // Count nodes in each partition
        Index count_in_0 = 0;
        Index count_in_1 = 0;
        Index total_nodes = 0;
        
        auto nodes = hg.getNetNodes(net_id);
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            total_nodes++;
            if (*node_it == node_id) continue;
            
            PartitionID part = partition.getPartition(*node_it);
            if (part == 0) count_in_0++;
            else count_in_1++;
        }
        
        // Calculate cut delta
        // Before: node is in partition 1 (implicitly)
        // After: node moves to partition 0
        if (count_in_1 == 0 && count_in_0 > 0) {
            // Net was cut (because this node was in part 1), now becomes internal
            delta -= net_weight;  // Cut reduction
        } else if (count_in_0 == 0 && count_in_1 > 0) {
            // Net was internal (all in part 1), now becomes cut
            delta += net_weight;  // Cut increase
        }
        // Otherwise: net status doesn't change
    }
    
    return delta;
}

Partition GHGOptPartitioner::partition(const Hypergraph& hg,
                                        const PartitionConstraints& constraints) {
    if (config_.num_partitions != 2) {
        GreedyPartitioner greedy(config_);
        return greedy.partition(hg, constraints);
    }
    
    Partition result(hg.getNumNodes(), 2);
    applyFixedConstraints(hg, constraints, result);
    
    // Always use per-type constraint checking (same as GHGPartitioner)
    Weight total_weight = hg.getTotalNodeWeight();
    Weight target_weight = total_weight / 2;
    Weight approx_max_weight = static_cast<Weight>(target_weight * (1.0 + config_.imbalance_factor));
    
    std::vector<bool> assigned(hg.getNumNodes(), false);
    
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (constraints.isNodeFixed(node_id) || hg.isNodeIgnored(node_id)) {
            assigned[node_id] = true;
        }
    }
    
    // Use different seed strategies based on config seed
    int strategy = config_.seed % 5;
    NodeID seed_node = selectSeedNode(hg, assigned, strategy);
    
    if (seed_node == INVALID_NODE) {
        return result;
    }
    
    result.setPartition(seed_node, 0, hg);
    assigned[seed_node] = true;
    Weight part0_weight = hg.getNodeWeight(seed_node);
    
    // Priority queue: (cut_delta, node_id) - min heap by cut delta
    // We want to add nodes that increase cut the least (or decrease it most)
    using CutDeltaPair = std::pair<int, NodeID>;
    auto compare = [](const CutDeltaPair& a, const CutDeltaPair& b) {
        return a.first > b.first;  // Min heap by cut delta
    };
    std::priority_queue<CutDeltaPair, std::vector<CutDeltaPair>, decltype(compare)> pq(compare);
    
    std::unordered_set<NodeID> in_frontier;
    
    // Add neighbors of seed node to frontier
    auto seed_nets = hg.getNodeNets(seed_node);
    for (const EdgeID* net_it = seed_nets.first; net_it != seed_nets.second; ++net_it) {
        auto nodes = hg.getNetNodes(*net_it);
        for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
            NodeID neighbor = *node_it;
            if (!assigned[neighbor] && in_frontier.find(neighbor) == in_frontier.end()) {
                int cut_delta = computeCutDelta(hg, result, neighbor);
                pq.push(std::make_pair(cut_delta, neighbor));
                in_frontier.insert(neighbor);
            }
        }
    }
    
    // Greedily grow partition 0, prioritizing nodes that minimize cut increase
    while (!pq.empty() && part0_weight < approx_max_weight) {
        CutDeltaPair top = pq.top();
        pq.pop();
        
        NodeID node_id = top.second;
        
        if (assigned[node_id]) {
            continue;
        }
        
        // Recompute cut delta (lazy update)
        int current_delta = computeCutDelta(hg, result, node_id);
        
        if (current_delta > top.first + 1) {
            pq.push(std::make_pair(current_delta, node_id));
            continue;
        }
        
        Weight node_weight = hg.getNodeWeight(node_id);
        const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
        
        // Check per-type capacity constraints for all types
        if (constraints.wouldViolateCapacityMultiType(0, type_weights, result)) {
            continue;  // Skip this node, would violate type capacity
        }
        
        result.setPartition(node_id, 0, hg);
        assigned[node_id] = true;
        part0_weight += node_weight;
        
        // Add new neighbors to frontier
        auto node_nets = hg.getNodeNets(node_id);
        for (const EdgeID* net_it = node_nets.first; net_it != node_nets.second; ++net_it) {
            auto nodes = hg.getNetNodes(*net_it);
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID neighbor = *node_it;
                if (!assigned[neighbor] && in_frontier.find(neighbor) == in_frontier.end()) {
                    int cut_delta = computeCutDelta(hg, result, neighbor);
                    pq.push(std::make_pair(cut_delta, neighbor));
                    in_frontier.insert(neighbor);
                }
            }
        }
    }
    
    // Assign remaining nodes, checking constraints for all types
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (!assigned[node_id]) {
            const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
            
            // Try partition 1 first (since partition 0 was grown greedily)
            if (!constraints.wouldViolateCapacityMultiType(1, type_weights, result)) {
                result.setPartition(node_id, 1, hg);
            } else {
                // Try partition 0
                if (!constraints.wouldViolateCapacityMultiType(0, type_weights, result)) {
                    result.setPartition(node_id, 0, hg);
                } else {
                    // Fallback: assign to partition 1 anyway (best effort)
                    result.setPartition(node_id, 1, hg);
                }
            }
        }
    }
    
    return result;
}

// ========== Balance Enforcement ==========

bool enforceBalanceConstraints(const Hypergraph& hg,
                               Partition& partition,
                               const PartitionConstraints& constraints,
                               bool use_types) {
    PartitionID num_partitions = partition.getNumPartitions();
    
    // Check if already balanced
    if (constraints.isBalanced(partition, hg)) {
        return true;
    }
    
    // Try to fix balance by moving nodes
    constexpr int MAX_ITERATIONS = 1000;
    
    for (int iter = 0; iter < MAX_ITERATIONS; ++iter) {
        // Check if balanced
        if (constraints.isBalanced(partition, hg)) {
            return true;
        }
        
        // Find overloaded and underloaded partitions
        PartitionID overloaded_part = INVALID_PARTITION;
        PartitionID underloaded_part = INVALID_PARTITION;
        Weight max_excess = 0;
        Weight max_room = 0;
        
        for (PartitionID part = 0; part < num_partitions; ++part) {
            Weight current = partition.getPartitionWeight(part);
            Weight max_capacity = constraints.getCapacity(part, NodeType::LUT).max_capacity;
            
            if (current > max_capacity) {
                Weight excess = current - max_capacity;
                if (excess > max_excess) {
                    max_excess = excess;
                    overloaded_part = part;
                }
            } else {
                Weight room = max_capacity - current;
                if (room > max_room) {
                    max_room = room;
                    underloaded_part = part;
                }
            }
        }
        
        if (overloaded_part == INVALID_PARTITION) {
            // No overloaded partition, might need type-specific balancing
            if (use_types) {
                // Check each type
                for (int type_idx = 0; type_idx < static_cast<int>(NodeType::OTHER) + 1; ++type_idx) {
                    NodeType type = static_cast<NodeType>(type_idx);
                    
                    for (PartitionID part = 0; part < num_partitions; ++part) {
                        Weight current = partition.getPartitionWeightByType(part, type);
                        Weight max_cap = constraints.getCapacity(part, type).max_capacity;
                        
                        if (current > max_cap) {
                            overloaded_part = part;
                            
                            // Find underloaded partition for this type
                            for (PartitionID other = 0; other < num_partitions; ++other) {
                                if (other == part) continue;
                                Weight other_current = partition.getPartitionWeightByType(other, type);
                                Weight other_max = constraints.getCapacity(other, type).max_capacity;
                                if (other_current < other_max) {
                                    underloaded_part = other;
                                    break;
                                }
                            }
                            
                            if (underloaded_part != INVALID_PARTITION) {
                                goto found_type_imbalance;
                            }
                        }
                    }
                }
            }
            
            // No imbalance found
            break;
        }
        
        found_type_imbalance:
        
        if (underloaded_part == INVALID_PARTITION) {
            return false;  // Cannot fix balance
        }
        
        // Find best node to move from overloaded to underloaded
        NodeID best_node = INVALID_NODE;
        Weight best_weight_diff = std::numeric_limits<Weight>::max();
        
        for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
            if (partition.getPartition(node_id) != overloaded_part) continue;
            if (hg.isNodeFixed(node_id)) continue;
            
            Weight node_weight = hg.getNodeWeight(node_id);
            const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
            
            // Check if moving this node would violate underloaded partition's capacity
            if (constraints.wouldViolateCapacityMultiType(underloaded_part, type_weights, partition)) {
                continue;
            }
            
            // Prefer smaller nodes to minimize disruption
            if (node_weight < best_weight_diff) {
                best_weight_diff = node_weight;
                best_node = node_id;
            }
        }
        
        if (best_node == INVALID_NODE) {
            return false;  // Cannot find movable node
        }
        
        // Move the node
        partition.moveNode(best_node, underloaded_part, hg);
    }
    
    return constraints.isBalanced(partition, hg);
}

} // namespace consmlp

