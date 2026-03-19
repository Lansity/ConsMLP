#include "coarsening/Coarsener.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include <iostream>

namespace consmlp {

namespace {
constexpr Index kMaxRatingNetDegree = 50;
constexpr Index kMaxMatchingNetDegree = 50;
constexpr size_t kMaxCoarseNetSize = 100;  // Reduced for faster coarsening
}

// ========== Coarsener Base Class ==========

Coarsener::Coarsener(const Configuration& config)
    : config_(config)
{
}

bool Coarsener::shouldStopCoarsening(NodeID num_nodes) const {
    return num_nodes <= config_.coarsening_threshold;
}

bool Coarsener::canCoarsenNode(const Hypergraph& hg, NodeID node_id) const {
    // Cannot coarsen ignored nodes
    if (hg.isNodeIgnored(node_id)) {
        return false;
    }
    
    // IO nodes should not be coarsened
    if (hg.getNodeType(node_id) == NodeType::IO) {
        return false;
    }
    
    return true;
}

bool Coarsener::canMatchNodes(const Hypergraph& hg, 
                              NodeID node1, NodeID node2) const {
    // Both nodes must be coarsenable
    if (!canCoarsenNode(hg, node1) || !canCoarsenNode(hg, node2)) {
        return false;
    }
    
    // Fixed nodes: can only match if both fixed to same partition
    bool node1_fixed = hg.isNodeFixed(node1);
    bool node2_fixed = hg.isNodeFixed(node2);
    
    if (node1_fixed || node2_fixed) {
        // If one is fixed and the other is not, cannot match
        if (node1_fixed != node2_fixed) {
            return false;
        }
        // Both are fixed - would need partition info to check
        // For now, allow matching fixed nodes (refinement will handle it)
    }
    
    // NOTE: Different types CAN be matched now
    // Per-type weights are tracked in each coarsened node
    // This allows better coarsening quality while preserving type constraints
    
    return true;
}

// ========== HeavyEdgeMatching ==========

HeavyEdgeMatching::HeavyEdgeMatching(const Configuration& config)
    : Coarsener(config)
{
}

CoarseningStats HeavyEdgeMatching::coarsen(HypergraphHierarchy& hierarchy,
                                           uint32_t level_idx,
                                           Profiler* profiler) {
    CoarseningStats stats;
    
    HypergraphLevel& fine_level = hierarchy.getLevel(level_idx);
    const Hypergraph& fine_hg = fine_level.getHypergraph();
    
    stats.original_nodes = fine_hg.getNumNodes();
    stats.original_nets = fine_hg.getNumNets();
    
    // Compute node matching
    std::vector<NodeID> matching(stats.original_nodes, INVALID_NODE);
    stats.num_matched_pairs = computeMatching(fine_hg, matching);
    
    // Build coarse graph
    HypergraphLevel& coarse_level = hierarchy.addCoarserLevel();
    Hypergraph& coarse_hg = coarse_level.getHypergraph();
    
    std::vector<NodeID> node_mapping;
    NodeID num_coarse_nodes = 0;
    buildCoarseGraph(fine_hg, matching, coarse_hg, node_mapping, num_coarse_nodes, profiler);
    
    stats.coarse_nodes = num_coarse_nodes;
    stats.num_singletons = stats.coarse_nodes - stats.num_matched_pairs;
    stats.contraction_ratio = static_cast<double>(stats.original_nodes) / 
                              stats.coarse_nodes;
    
    // Finalize coarse hypergraph
    coarse_hg.finalize();
    stats.coarse_nets = coarse_hg.getNumNets();
    
    // Set up node mappings in hierarchy
    for (NodeID fine_node = 0; fine_node < stats.original_nodes; ++fine_node) {
        NodeID coarse_node = node_mapping[fine_node];
        fine_level.setCoarserNode(fine_node, coarse_node);
        fine_level.addFinerNode(coarse_node, fine_node);
    }
    fine_level.finalizeMapping();
    
    return stats;
}

uint32_t HeavyEdgeMatching::computeMatching(const Hypergraph& hg,
                                            std::vector<NodeID>& matching,
                                            Profiler* profiler) {
    constexpr double kMinContractionRatio = 1.67;
    uint32_t num_matched = 0;
    const NodeID num_nodes = hg.getNumNodes();
    
    // Calculate target number of matched pairs to achieve desired contraction ratio
    const uint32_t max_matched = static_cast<uint32_t>(num_nodes * (1.0 - 1.0 / kMinContractionRatio));
    
    std::vector<bool> is_matched(num_nodes, false);
    std::vector<double> match_scores(num_nodes, -1.0);
    
    // Collect matching candidates
    std::vector<MatchCandidate> candidates;
    candidates.reserve(num_nodes);
    
    for (NodeID node1 = 0; node1 < num_nodes; ++node1) {
        if (!canCoarsenNode(hg, node1)) {
            continue;
        }
        
        double best_rating = -1.0;
        NodeID best_neighbor = INVALID_NODE;
        
        auto nets = hg.getNodeNets(node1);
        std::unordered_set<NodeID> neighbors;
        neighbors.reserve(hg.getNodeDegree(node1) * 4);
        
        for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
            auto nodes = hg.getNetNodes(*net_it);
            if (nodes.second - nodes.first > kMaxMatchingNetDegree) continue;
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID node2 = *node_it;
                if (node2 != node1 && node2 > node1) {
                    neighbors.insert(node2);
                }
            }
        }
        
        for (NodeID node2 : neighbors) {
            if (!canMatchNodes(hg, node1, node2)) continue;
            double rating = computeRating(hg, node1, node2);
            if (rating > best_rating) {
                best_rating = rating;
                best_neighbor = node2;
                if (rating > 1000.0) break;
            }
        }
        
        if (best_neighbor != INVALID_NODE) {
            candidates.emplace_back(node1, best_neighbor, best_rating);
        }
    }
    
    auto match_pair = [&](NodeID a, NodeID b, double score) {
        matching[a] = b;
        matching[b] = a;
        is_matched[a] = true;
        is_matched[b] = true;
        match_scores[a] = score;
        match_scores[b] = score;
        ++num_matched;
    };
    
    auto unmatch_pair = [&](NodeID a, NodeID b) {
        matching[a] = INVALID_NODE;
        matching[b] = INVALID_NODE;
        is_matched[a] = false;
        is_matched[b] = false;
        match_scores[a] = -1.0;
        match_scores[b] = -1.0;
        --num_matched;
    };
    
    std::sort(candidates.begin(), candidates.end());
    
    // First pass: greedy matching until we reach target ratio
    for (const auto& candidate : candidates) {
        if (num_matched >= max_matched) {
            break;  // Stop early if we've reached target contraction ratio
        }
        NodeID node1 = candidate.node1;
        NodeID node2 = candidate.node2;
        if (is_matched[node1] || is_matched[node2]) {
            continue;
        }
        match_pair(node1, node2, candidate.rating);
    }
    
    // Second pass: local rematching for remaining nodes
    std::vector<NodeID> order(num_nodes);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](NodeID a, NodeID b) {
        Weight wa = hg.getNodeWeight(a);
        Weight wb = hg.getNodeWeight(b);
        if (wa == wb) {
            return hg.getNodeDegree(a) < hg.getNodeDegree(b);
        }
        return wa < wb;
    });
    
    std::vector<NodeID> pending;
    pending.reserve(num_nodes / 4);
    std::vector<bool> in_queue(num_nodes, false);
    
    auto tryLocalMatch = [&](NodeID node1) {
        if (is_matched[node1] || !canCoarsenNode(hg, node1)) {
            return;
        }
        
        std::unordered_map<NodeID, double> neighbor_scores;
        neighbor_scores.reserve(hg.getNodeDegree(node1) * 2);
        
        auto nets = hg.getNodeNets(node1);
        for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
            EdgeID net_id = *net_it;
            Index net_size = hg.getNetSize(net_id);
            if (net_size <= 1 || net_size > kMaxMatchingNetDegree) {
                continue;
            }
            double contribution = static_cast<double>(hg.getNetWeight(net_id)) /
                                  (net_size - 1);
            auto nodes = hg.getNetNodes(net_id);
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID node2 = *node_it;
                if (node2 == node1 || !canMatchNodes(hg, node1, node2)) {
                    continue;
                }
                neighbor_scores[node2] += contribution;
            }
        }
        
        if (neighbor_scores.empty()) {
            return;
        }
        
        NodeID best_neighbor = INVALID_NODE;
        double best_score = -1.0;
        for (const auto& entry : neighbor_scores) {
            NodeID candidate = entry.first;
            double score = entry.second;
            if (score > best_score || (score == best_score && candidate < best_neighbor)) {
                best_score = score;
                best_neighbor = candidate;
            }
        }
        
        if (best_neighbor == INVALID_NODE) {
            return;
        }
        
        if (!is_matched[best_neighbor]) {
            match_pair(node1, best_neighbor, best_score);
        } else if (best_score > match_scores[best_neighbor]) {
            NodeID prev_partner = matching[best_neighbor];
            unmatch_pair(best_neighbor, prev_partner);
            if (!in_queue[prev_partner]) {
                pending.push_back(prev_partner);
                in_queue[prev_partner] = true;
            }
            match_pair(node1, best_neighbor, best_score);
        }
    };
    
    // Second pass: local rematching for remaining nodes (only if below target)
    for (NodeID node1 : order) {
        if (num_matched >= max_matched) {
            break;  // Stop if we've reached target contraction ratio
        }
        tryLocalMatch(node1);
    }
    
    while (!pending.empty() && num_matched < max_matched) {
        NodeID node = pending.back();
        pending.pop_back();
        in_queue[node] = false;
        tryLocalMatch(node);
    }
    
    return num_matched;
}

double HeavyEdgeMatching::computeRating(const Hypergraph& hg,
                                        NodeID node1, NodeID node2) {
    double rating = 0.0;
    
    auto nets1 = hg.getNodeNets(node1);
    auto nets2 = hg.getNodeNets(node2);
    
    if (nets2.second - nets2.first < nets1.second - nets1.first) {
        std::swap(nets1, nets2);
    }
    
    std::unordered_set<EdgeID> nets1_set(nets1.first, nets1.second);
    
    for (const EdgeID* it = nets2.first; it != nets2.second; ++it) {
        EdgeID net_id = *it;
        if (nets1_set.find(net_id) == nets1_set.end()) {
            continue;
        }
        Index net_size = hg.getNetSize(net_id);
        if (net_size <= 1 || net_size > kMaxRatingNetDegree) {
            continue;
        }
        rating += static_cast<double>(hg.getNetWeight(net_id)) / (net_size - 1);
    }
    
    return rating;
}

void HeavyEdgeMatching::buildCoarseGraph(const Hypergraph& fine_hg,
                                        const std::vector<NodeID>& matching,
                                        Hypergraph& coarse_hg,
                                        std::vector<NodeID>& node_mapping,
                                        NodeID& num_coarse_nodes,
                                        Profiler* profiler) {
    NodeID num_fine_nodes = fine_hg.getNumNodes();
    node_mapping.resize(num_fine_nodes, INVALID_NODE);
    
    // Assign coarse node IDs and track which fine nodes belong to each coarse node
    num_coarse_nodes = 0;
    std::vector<std::vector<NodeID>> coarse_to_fine;  // Map coarse node to its fine nodes
    
    for (NodeID fine_node = 0; fine_node < num_fine_nodes; ++fine_node) {
        if (node_mapping[fine_node] != INVALID_NODE) {
            continue;  // Already processed as part of a pair
        }
        
        NodeID coarse_node = num_coarse_nodes++;
        NodeID mate = matching[fine_node];
        
        std::vector<NodeID> fine_nodes;
        fine_nodes.push_back(fine_node);
        node_mapping[fine_node] = coarse_node;
        
        if (mate != INVALID_NODE && mate > fine_node) {
            // Matched pair
            fine_nodes.push_back(mate);
            node_mapping[mate] = coarse_node;
        }
        
        coarse_to_fine.push_back(fine_nodes);
    }
    
    // Add coarse nodes with accumulated per-type weights
    for (NodeID i = 0; i < num_coarse_nodes; ++i) {
        TypeWeights type_weights = {};
        for (NodeID fine_node : coarse_to_fine[i]) {
            const TypeWeights& fine_weights = fine_hg.getNodeTypeWeights(fine_node);
            for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
                type_weights[t] += fine_weights[t];
            }
        }
        coarse_hg.addNodeWithTypeWeights(type_weights);
    }
    
    // Build coarse nets (with parallel net detection using hash map)
    // Hash function for vector of sorted node IDs
    struct VectorHash {
        size_t operator()(const std::vector<NodeID>& vec) const {
            size_t hash = vec.size();
            for (NodeID node : vec) {
                hash ^= std::hash<NodeID>()(node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
    
    // Map from sorted node set to weight
    std::unordered_map<std::vector<NodeID>, Weight, VectorHash> net_map;
    net_map.reserve(fine_hg.getNumNets() / 2);  // Pre-allocate for better performance
    std::vector<std::vector<NodeID>> net_keys;  // Keep order for adding nets
    net_keys.reserve(fine_hg.getNumNets() / 2);
    
    for (EdgeID fine_net = 0; fine_net < fine_hg.getNumNets(); ++fine_net) {
        // Collect coarse nodes for this net (using set to remove duplicates)
        std::unordered_set<NodeID> coarse_nodes_set;
        coarse_nodes_set.reserve(fine_hg.getNetSize(fine_net));
        
        auto fine_nodes = fine_hg.getNetNodes(fine_net);
        for (const NodeID* it = fine_nodes.first; it != fine_nodes.second; ++it) {
            NodeID coarse_node = node_mapping[*it];
            coarse_nodes_set.insert(coarse_node);
        }
        
        // Skip single-node nets
        if (coarse_nodes_set.size() <= 1) {
            continue;
        }
        
        // Convert to sorted vector for key
        std::vector<NodeID> key(coarse_nodes_set.begin(), coarse_nodes_set.end());
        std::sort(key.begin(), key.end());
        
        if (key.size() > kMaxCoarseNetSize) {
            continue;  // Skip overly large nets
        }
        
        // Check/add to map
        auto it = net_map.find(key);
        if (it != net_map.end()) {
            // Parallel net - merge weights
            it->second += fine_hg.getNetWeight(fine_net);
        } else {
            // New net
            net_map[key] = fine_hg.getNetWeight(fine_net);
            net_keys.push_back(std::move(key));
        }
    }
    
    // Add coarse nets to hypergraph
    for (const auto& key : net_keys) {
        auto it = net_map.find(key);
        if (it != net_map.end()) {
            EdgeID coarse_net = coarse_hg.addNet(it->second, false);
            
            for (NodeID coarse_node : key) {
                coarse_hg.addNodeToNet(coarse_net, coarse_node);
            }
        }
    }
}

bool HeavyEdgeMatching::areNeighbors(const Hypergraph& hg,
                                     NodeID node1, NodeID node2) const {
    return countCommonNets(hg, node1, node2) > 0;
}

Index HeavyEdgeMatching::countCommonNets(const Hypergraph& hg,
                                        NodeID node1, NodeID node2) const {
    auto nets1 = hg.getNodeNets(node1);
    std::unordered_set<EdgeID> nets1_set(nets1.first, nets1.second);
    
    auto nets2 = hg.getNodeNets(node2);
    Index count = 0;
    
    for (const EdgeID* it = nets2.first; it != nets2.second; ++it) {
        if (nets1_set.find(*it) != nets1_set.end()) {
            count++;
        }
    }
    
    return count;
}

// ========== FirstChoiceMatching ==========

FirstChoiceMatching::FirstChoiceMatching(const Configuration& config)
    : Coarsener(config)
{
}

CoarseningStats FirstChoiceMatching::coarsen(HypergraphHierarchy& hierarchy,
                                             uint32_t level_idx,
                                             Profiler* profiler) {
    CoarseningStats stats;
    
    HypergraphLevel& fine_level = hierarchy.getLevel(level_idx);
    const Hypergraph& fine_hg = fine_level.getHypergraph();
    
    stats.original_nodes = fine_hg.getNumNodes();
    stats.original_nets = fine_hg.getNumNets();
    
    // Compute node matching
    std::vector<NodeID> matching(stats.original_nodes, INVALID_NODE);
    stats.num_matched_pairs = computeMatching(fine_hg, matching);
    
    // Build coarse graph (similar to HeavyEdgeMatching)
    HypergraphLevel& coarse_level = hierarchy.addCoarserLevel();
    Hypergraph& coarse_hg = coarse_level.getHypergraph();
    
    std::vector<NodeID> node_mapping;
    NodeID num_coarse_nodes = 0;
    
    // Simple node mapping with per-type weight tracking
    node_mapping.resize(stats.original_nodes, INVALID_NODE);
    num_coarse_nodes = 0;
    std::vector<std::vector<NodeID>> coarse_to_fine;
    
    for (NodeID fine_node = 0; fine_node < stats.original_nodes; ++fine_node) {
        if (node_mapping[fine_node] != INVALID_NODE) {
            continue;
        }
        
        NodeID coarse_node = num_coarse_nodes++;
        NodeID mate = matching[fine_node];
        
        std::vector<NodeID> fine_nodes;
        fine_nodes.push_back(fine_node);
        node_mapping[fine_node] = coarse_node;
        
        if (mate != INVALID_NODE && mate > fine_node) {
            fine_nodes.push_back(mate);
            node_mapping[mate] = coarse_node;
        }
        
        coarse_to_fine.push_back(fine_nodes);
    }
    
    // Add coarse nodes with accumulated per-type weights
    for (NodeID i = 0; i < num_coarse_nodes; ++i) {
        TypeWeights type_weights = {};
        for (NodeID fine_node : coarse_to_fine[i]) {
            const TypeWeights& fine_weights = fine_hg.getNodeTypeWeights(fine_node);
            for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
                type_weights[t] += fine_weights[t];
            }
        }
        coarse_hg.addNodeWithTypeWeights(type_weights);
    }
    
    // Build coarse nets with parallel net merging and duplicate pin removal
    buildCoarseNets(fine_hg, node_mapping, coarse_hg);
    
    coarse_hg.finalize();
    stats.coarse_nodes = num_coarse_nodes;
    stats.coarse_nets = coarse_hg.getNumNets();
    stats.num_singletons = stats.coarse_nodes - stats.num_matched_pairs;
    stats.contraction_ratio = static_cast<double>(stats.original_nodes) / stats.coarse_nodes;
    
    // Set up mappings
    for (NodeID fine_node = 0; fine_node < stats.original_nodes; ++fine_node) {
        NodeID coarse_node = node_mapping[fine_node];
        fine_level.setCoarserNode(fine_node, coarse_node);
        fine_level.addFinerNode(coarse_node, fine_node);
    }
    fine_level.finalizeMapping();
    
    return stats;
}

uint32_t FirstChoiceMatching::computeMatching(const Hypergraph& hg,
                                              std::vector<NodeID>& matching) {
    constexpr double kMinContractionRatio = 1.67;
    
    uint32_t num_matched = 0;
    const NodeID num_nodes = hg.getNumNodes();
    
    // Calculate target number of matched pairs to achieve desired contraction ratio
    // contraction_ratio = num_nodes / (num_nodes - num_matched)
    // Solving for num_matched: num_matched = num_nodes * (1 - 1/ratio)
    const uint32_t max_matched = static_cast<uint32_t>(num_nodes * (1.0 - 1.0 / kMinContractionRatio));
    
    std::vector<bool> is_matched(num_nodes, false);
    std::vector<double> match_scores(num_nodes, -1.0);
    std::vector<NodeID> pending;
    pending.reserve(num_nodes / 4);
    
    // Visit nodes in ascending order of node size (weight, then degree)
    std::vector<NodeID> order(num_nodes);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](NodeID a, NodeID b) {
        Weight wa = hg.getNodeWeight(a);
        Weight wb = hg.getNodeWeight(b);
        if (wa == wb) {
            return hg.getNodeDegree(a) < hg.getNodeDegree(b);
        }
        return wa < wb;
    });
    
    auto match_pair = [&](NodeID a, NodeID b, double score) {
        matching[a] = b;
        matching[b] = a;
        is_matched[a] = true;
        is_matched[b] = true;
        match_scores[a] = score;
        match_scores[b] = score;
        ++num_matched;
    };
    
    auto unmatch_pair = [&](NodeID a, NodeID b) {
        matching[a] = INVALID_NODE;
        matching[b] = INVALID_NODE;
        is_matched[a] = false;
        is_matched[b] = false;
        match_scores[a] = -1.0;
        match_scores[b] = -1.0;
        --num_matched;
    };
    
    auto try_match = [&](NodeID node1) {
        if (is_matched[node1] || !canCoarsenNode(hg, node1)) {
            return;
        }
        
        std::unordered_map<NodeID, double> neighbor_scores;
        neighbor_scores.reserve(hg.getNodeDegree(node1) * 4);
        
        // Accumulate scores for each neighbor based on shared nets
        auto nets = hg.getNodeNets(node1);
        
        for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
            EdgeID net_id = *net_it;
            Index net_size = hg.getNetSize(net_id);
            auto net_weight = hg.getNetWeight(net_id);
            if (net_size <= 1) {
                continue;
            }
            
            if (static_cast<double>(net_weight) / (net_size - 1) < 0.005) {
                continue;
            }
            
            double contribution = static_cast<double>(net_weight) / (net_size - 1);
            auto nodes = hg.getNetNodes(net_id);
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID node2 = *node_it;
                if (node2 == node1 || !canMatchNodes(hg, node1, node2)) {
                    continue;
                }
                neighbor_scores[node2] += contribution;
            }
        }
        
        if (neighbor_scores.empty()) {
            return;
        }
        
        // Sort neighbors by score (descending) to try them in order
        std::vector<std::pair<NodeID, double>> sorted_neighbors;
        sorted_neighbors.reserve(neighbor_scores.size());
        for (const auto& entry : neighbor_scores) {
            sorted_neighbors.emplace_back(entry.first, entry.second);
        }
        std::sort(sorted_neighbors.begin(), sorted_neighbors.end(),
                  [](const std::pair<NodeID, double>& a, const std::pair<NodeID, double>& b) {
                      if (a.second != b.second) {
                          return a.second > b.second;  // Higher score first
                      }
                      return a.first < b.first;  // Tie-break by node ID
                  });
        
        // Try neighbors in order of score until we find one we can match with
        for (const auto& neighbor_entry : sorted_neighbors) {
            NodeID candidate = neighbor_entry.first;
            double score = neighbor_entry.second;
            
            if (!is_matched[candidate]) {
                // Candidate is free, match immediately
                match_pair(node1, candidate, score);
                return;
            } else {
                // Candidate is already matched, check if we can break the existing match
                double existing_score = match_scores[candidate];
                if (existing_score <= 1.0 && score > existing_score) {
                    NodeID prev_partner = matching[candidate];
                    unmatch_pair(candidate, prev_partner);
                    pending.push_back(prev_partner);
                    match_pair(node1, candidate, score);
                    return;
                }
                // Otherwise, try next candidate
            }
        }
    };
    
    // First pass: match nodes until we reach target ratio
    for (NodeID node1 : order) {
        if (num_matched >= max_matched) {
            break;  // Stop early if we've reached target contraction ratio
        }
        try_match(node1);
    }
    
    // Second pass: process pending nodes if we haven't reached minimum target
    while (!pending.empty() && num_matched < max_matched) {
        NodeID node = pending.back();
        pending.pop_back();
        try_match(node);
    }
    
    return num_matched;
}

void FirstChoiceMatching::buildCoarseNets(const Hypergraph& fine_hg,
                                         const std::vector<NodeID>& node_mapping,
                                         Hypergraph& coarse_hg) {
    // Hash function for vector of sorted node IDs
    struct VectorHash {
        size_t operator()(const std::vector<NodeID>& vec) const {
            size_t hash = vec.size();
            for (NodeID node : vec) {
                hash ^= std::hash<NodeID>()(node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
    
    // Map from sorted node set to accumulated weight (for parallel net merging)
    std::unordered_map<std::vector<NodeID>, Weight, VectorHash> net_map;
    net_map.reserve(fine_hg.getNumNets() / 2);  // Pre-allocate for better performance
    std::vector<std::vector<NodeID>> net_keys;  // Keep insertion order
    net_keys.reserve(fine_hg.getNumNets() / 2);
    
    for (EdgeID fine_net = 0; fine_net < fine_hg.getNumNets(); ++fine_net) {
        // Collect coarse nodes for this net (using set to remove duplicate pins)
        std::unordered_set<NodeID> coarse_nodes_set;
        coarse_nodes_set.reserve(fine_hg.getNetSize(fine_net));
        
        auto fine_nodes = fine_hg.getNetNodes(fine_net);
        for (const NodeID* it = fine_nodes.first; it != fine_nodes.second; ++it) {
            NodeID coarse_node = node_mapping[*it];
            coarse_nodes_set.insert(coarse_node);  // Automatically removes duplicates
        }
        
        // Skip single-node nets (self-loops)
        if (coarse_nodes_set.size() <= 1) {
            continue;
        }
        
        // Skip overly large nets
        if (coarse_nodes_set.size() > kMaxCoarseNetSize) {
            continue;
        }
        
        // Convert to sorted vector for use as key (for parallel net detection)
        std::vector<NodeID> key(coarse_nodes_set.begin(), coarse_nodes_set.end());
        std::sort(key.begin(), key.end());
        
        // Check if this net pattern already exists (parallel net)
        auto it = net_map.find(key);
        if (it != net_map.end()) {
            // Parallel net found - merge weights
            it->second += fine_hg.getNetWeight(fine_net);
        } else {
            // New unique net pattern
            net_map[key] = fine_hg.getNetWeight(fine_net);
            net_keys.push_back(std::move(key));
        }
    }
    
    // Add all unique coarse nets to hypergraph
    for (const auto& key : net_keys) {
        auto it = net_map.find(key);
        if (it != net_map.end()) {
            EdgeID coarse_net = coarse_hg.addNet(it->second, false);
            
            // Add each unique node to the net (no duplicates due to set usage above)
            for (NodeID coarse_node : key) {
                coarse_hg.addNodeToNet(coarse_net, coarse_node);
            }
        }
    }
}

// ========== ClusterMatching ==========

ClusterMatching::ClusterMatching(const Configuration& config)
    : Coarsener(config)
{
}

CoarseningStats ClusterMatching::coarsen(HypergraphHierarchy& hierarchy,
                                         uint32_t level_idx,
                                         Profiler* profiler) {
    CoarseningStats stats;
    curr_level_idx_ = level_idx;
    HypergraphLevel& fine_level = hierarchy.getLevel(curr_level_idx_);
    const Hypergraph& fine_hg = fine_level.getHypergraph();
    
    stats.original_nodes = fine_hg.getNumNodes();
    stats.original_nets = fine_hg.getNumNets();
    
    // Compute clustering (multi-node matching)
    // V2 considers accumulated cluster contribution for better quality
    std::vector<NodeID> cluster_id(stats.original_nodes, INVALID_NODE);
    NodeID num_clusters = computeClusteringV2(fine_hg, cluster_id);
    
    // Build coarse graph
    HypergraphLevel& coarse_level = hierarchy.addCoarserLevel();
    Hypergraph& coarse_hg = coarse_level.getHypergraph();
    
    // Compute cluster per-type weights (accumulate from all fine nodes in cluster)
    std::vector<TypeWeights> cluster_type_weights(num_clusters);
    for (NodeID i = 0; i < num_clusters; ++i) {
        for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
            cluster_type_weights[i][t] = 0;
        }
    }
    
    for (NodeID fine_node = 0; fine_node < stats.original_nodes; ++fine_node) {
        NodeID cid = cluster_id[fine_node];
        if (cid != INVALID_NODE) {
            // Accumulate per-type weights from fine node
            const TypeWeights& fine_weights = fine_hg.getNodeTypeWeights(fine_node);
            for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
                cluster_type_weights[cid][t] += fine_weights[t];
            }
        }
    }
    
    // Add coarse nodes with per-type weights
    for (NodeID i = 0; i < num_clusters; ++i) {
        coarse_hg.addNodeWithTypeWeights(cluster_type_weights[i]);
    }
    
    // Build coarse nets with parallel net merging
    buildCoarseNets(fine_hg, cluster_id, coarse_hg);
    
    coarse_hg.finalize();
    
    stats.coarse_nodes = num_clusters;
    stats.coarse_nets = coarse_hg.getNumNets();
    stats.contraction_ratio = static_cast<double>(stats.original_nodes) / stats.coarse_nodes;
    
    // Count matched pairs (approximate: nodes in clusters of size >= 2)
    std::vector<size_t> cluster_sizes(num_clusters, 0);
    for (NodeID fine_node = 0; fine_node < stats.original_nodes; ++fine_node) {
        NodeID cid = cluster_id[fine_node];
        if (cid != INVALID_NODE) {
            cluster_sizes[cid]++;
        }
    }
    
    stats.num_matched_pairs = 0;
    for (NodeID cid = 0; cid < num_clusters; ++cid) {
        if (cluster_sizes[cid] >= 2) {
            stats.num_matched_pairs += cluster_sizes[cid] - 1;
        }
    }
    stats.num_singletons = stats.coarse_nodes - stats.num_matched_pairs;
    
    // Set up mappings
    for (NodeID fine_node = 0; fine_node < stats.original_nodes; ++fine_node) {
        NodeID coarse_node = cluster_id[fine_node];
        fine_level.setCoarserNode(fine_node, coarse_node);
        fine_level.addFinerNode(coarse_node, fine_node);
    }
    fine_level.finalizeMapping();
    
    return stats;
}

NodeID ClusterMatching::computeClustering(const Hypergraph& hg,
                                          std::vector<NodeID>& cluster_id) {
    constexpr Index kMaxNetDegree = 50;  // Skip large nets for performance pre 50
    
    const NodeID num_nodes = hg.getNumNodes();
    
    // Track cluster information
    std::vector<bool> is_clustered(num_nodes, false);
    std::vector<NodeID> cluster_leader(num_nodes, INVALID_NODE);  // Leader of each cluster
    std::vector<size_t> cluster_size(num_nodes, 0);  // Size of cluster led by this node
    std::vector<Weight> cluster_weight(num_nodes, 0);  // Weight of cluster led by this node
    uint32_t max_cluster_size = 30;
    NodeID num_clusters = 0;
    
    // Visit nodes in ascending order of weight/degree
    std::vector<NodeID> order(num_nodes);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](NodeID a, NodeID b) {
        Weight wa = hg.getNodeWeight(a);
        Weight wb = hg.getNodeWeight(b);
        if (wa == wb) {
            return hg.getNodeDegree(a) < hg.getNodeDegree(b);
        }
        return wa < wb;
    });

    auto total_weight = config_.total_node_weight;
    auto admissible_weight = num_nodes < 300 ? total_weight / 30 : total_weight / 80;

    auto quan_weight_factor = [&](Weight weight) -> double {
        return static_cast<double>(weight) / (static_cast<double>(total_weight) / static_cast<double>(num_nodes));
    };

    for (NodeID node : order) {
        // Skip if already clustered
        if (is_clustered[node]) continue;
        
        // Skip if cannot coarsen this node
        if (!canCoarsenNode(hg, node)) {
            // Create singleton cluster
            cluster_id[node] = num_clusters;
            cluster_leader[node] = node;
            cluster_size[node] = 1;
            cluster_weight[node] = hg.getNodeWeight(node);
            is_clustered[node] = true;
            num_clusters++;
            continue;
        }
        
        // Collect candidate neighbors directly as vector (faster than unordered_map for small sizes)
        std::vector<std::pair<NodeID, double>> candidates;
        candidates.reserve(32);
        
        
        auto nets = hg.getNodeNets(node);
        for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) { // && nets_processed < kMaxNetsToProcess
            EdgeID net_id = *net_it;
            Index net_size = hg.getNetSize(net_id);
            // Skip very small or very large nets
            if (net_size <= 1 || net_size > kMaxNetDegree) continue;
            
            double contribution = static_cast<double>(hg.getNetWeight(net_id)) / net_size;
            auto nodes = hg.getNetNodes(net_id);
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID neighbor = *node_it;
                if (neighbor == node) continue;
                if (!canMatchNodes(hg, node, neighbor)) continue;
                
                if (is_clustered[neighbor]) {
                    NodeID leader = cluster_leader[neighbor];
                    if (leader != INVALID_NODE && cluster_weight[leader] + hg.getNodeWeight(node) > admissible_weight) {
                        continue;
                    }
                } else if (!adjMatchAreaJudging(hg, node, neighbor, admissible_weight)) {
                    continue;
                }
                
                // Add or update neighbor score
                bool found = false;
                double quan_weight = contribution / quan_weight_factor(hg.getNodeWeight(node));
                for (auto& p : candidates) {
                    if (p.first == neighbor) {
                        p.second += quan_weight; // maybe multiply by 2 or 3
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    candidates.emplace_back(neighbor, quan_weight);
                }
            }
        }
        
        if (candidates.empty()) {
            // No valid neighbors, create singleton cluster
            cluster_id[node] = num_clusters;
            cluster_leader[node] = node;
            cluster_size[node] = 1;
            cluster_weight[node] = hg.getNodeWeight(node);
            is_clustered[node] = true;
            num_clusters++;
            continue;
        }
        
        // Partial sort to get top candidates only
        size_t num_top = std::min(candidates.size(), static_cast<size_t>(20));
        std::partial_sort(candidates.begin(), 
                          candidates.begin() + num_top,
                          candidates.end(),
                          [](const std::pair<NodeID, double>& a, const std::pair<NodeID, double>& b) {
                              return a.second > b.second;
                          });
        candidates.resize(num_top);
        
        bool joined_cluster = false;
        Weight node_weight = hg.getNodeWeight(node);
        
        // Try to join existing cluster or create new one (only top candidates)
        for (const auto& neighbor_entry : candidates) {
            NodeID neighbor = neighbor_entry.first;
            if (!is_clustered[neighbor]) {
                // Neighbor is free, create new cluster with both nodes
                cluster_id[node] = num_clusters;
                cluster_id[neighbor] = num_clusters;
                cluster_leader[node] = node;  // node is the leader
                cluster_leader[neighbor] = node;  // neighbor points to node as leader
                cluster_size[node] = 2;
                cluster_weight[node] = node_weight + hg.getNodeWeight(neighbor);
                is_clustered[node] = true;
                is_clustered[neighbor] = true;
                num_clusters++;
                joined_cluster = true;
                break;
            } else {
                // Neighbor is already in a cluster, try to join it
                NodeID leader = cluster_leader[neighbor];
                
                // Safety check: leader must be valid
                if (leader == INVALID_NODE || leader >= num_nodes) {
                    continue;
                }
                
                // Safety check after chain following
                if (leader == INVALID_NODE || leader >= num_nodes) {
                    continue;
                }
                
                // NOTE: Type matching removed - per-type weights are tracked separately
                
                size_t curr_size = cluster_size[leader];
                
                // Check if we can join this cluster
                if (curr_size < max_cluster_size) {

                    cluster_id[node] = cluster_id[neighbor];
                    cluster_leader[node] = leader;
                    cluster_size[leader]++;
                    cluster_weight[leader] += node_weight;
                    is_clustered[node] = true;
                    joined_cluster = true;
                    break;
                }
                // Otherwise, try next neighbor
            }
        }
        
        if (!joined_cluster) {
            // Could not join any cluster, create singleton
            cluster_id[node] = num_clusters;
            cluster_leader[node] = node;
            cluster_size[node] = 1;
            cluster_weight[node] = node_weight;
            is_clustered[node] = true;
            num_clusters++;
        }
    }
    
    // Renumber clusters to be contiguous
    std::vector<NodeID> cluster_remap(num_clusters, INVALID_NODE);
    NodeID new_cluster_count = 0;
    
    for (NodeID node = 0; node < num_nodes; ++node) {
        NodeID old_cid = cluster_id[node];
        if (old_cid != INVALID_NODE) {
            if (cluster_remap[old_cid] == INVALID_NODE) {
                cluster_remap[old_cid] = new_cluster_count++;
            }
            cluster_id[node] = cluster_remap[old_cid];
        }
    }
    
    return new_cluster_count;
}

// ========== Compute Clustering V2 ==========
// This version accumulates connection contributions for nodes in the same cluster
NodeID ClusterMatching::computeClusteringV2(const Hypergraph& hg,
                                            std::vector<NodeID>& cluster_id) {
    constexpr Index kMaxNetDegree = 50;  // Skip large nets for performance
    
    const NodeID num_nodes = hg.getNumNodes();
    
    // Track cluster information
    std::vector<bool> is_clustered(num_nodes, false);
    std::vector<NodeID> cluster_leader(num_nodes, INVALID_NODE);  // Leader of each cluster
    std::vector<size_t> cluster_size(num_nodes, 0);  // Size of cluster led by this node
    std::vector<Weight> cluster_weight(num_nodes, 0);  // Weight of cluster led by this node
    uint32_t max_cluster_size = 12; 
    if (curr_level_idx_ < 3)
        max_cluster_size = 10;
    else if (curr_level_idx_ < 7)
        max_cluster_size = 5;
    else   
        max_cluster_size = 4;
    NodeID num_clusters = 0;
    
    // Visit nodes in ascending order of weight/degree
    std::vector<NodeID> order(num_nodes);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](NodeID a, NodeID b) {
        Weight wa = hg.getNodeWeight(a);
        Weight wb = hg.getNodeWeight(b);
        if (wa == wb) {
            return hg.getNodeDegree(a) < hg.getNodeDegree(b);
        }
        return wa < wb;
    });

    auto total_weight = config_.total_node_weight;
    auto admissible_weight = num_nodes < 300 ? total_weight / 40 : total_weight / 80;

    auto quan_weight_factor = [&](Weight weight) -> double {
        return static_cast<double>(weight) / (static_cast<double>(total_weight) / static_cast<double>(num_nodes));
    };

    double net_scale_factor = 1.0;
    for (NodeID node : order) {
        // Skip if already clustered
        if (is_clustered[node]) continue;
        
        // Skip if cannot coarsen this node
        if (!canCoarsenNode(hg, node)) {
            // Create singleton cluster
            cluster_id[node] = num_clusters;
            cluster_leader[node] = node;
            cluster_size[node] = 1;
            cluster_weight[node] = hg.getNodeWeight(node);
            is_clustered[node] = true;
            num_clusters++;
            continue;
        }
        
        // Key difference from V1: separate tracking for unclustered nodes and clusters
        // unclustered_candidates: neighbor node -> contribution
        // cluster_candidates: cluster leader -> accumulated contribution
        std::unordered_map<NodeID, double> unclustered_candidates;
        std::unordered_map<NodeID, double> cluster_candidates;
        auto nets = hg.getNodeNets(node);
        for (const EdgeID* net_it = nets.first; net_it != nets.second; ++net_it) {
            EdgeID net_id = *net_it;
            Index net_size = hg.getNetSize(net_id);
            // Skip very small or very large nets
            if (net_size <= 1 || net_size > kMaxNetDegree) continue;
            if (num_nodes < 10000) 
                net_scale_factor = 1.0;
            else
                net_scale_factor = static_cast<double>(net_size - 1);
            double contribution = static_cast<double>(hg.getNetWeight(net_id)) / net_scale_factor;
            auto nodes = hg.getNetNodes(net_id);
            for (const NodeID* node_it = nodes.first; node_it != nodes.second; ++node_it) {
                NodeID neighbor = *node_it;
                if (neighbor == node) continue;
                if (!canMatchNodes(hg, node, neighbor)) continue;
                
                
                if (is_clustered[neighbor]) {
                    // Neighbor is in a cluster - accumulate contribution to cluster leader
                    NodeID leader = cluster_leader[neighbor];
                    // double quan_weight = contribution / quan_weight_factor(hg.getNodeWeight(neighbor));
                    double quan_weight = contribution / quan_weight_factor(cluster_weight[leader]);

                    if (leader == INVALID_NODE || leader >= num_nodes) continue;
                    
                    // Check weight constraint
                    if (cluster_weight[leader] + hg.getNodeWeight(node) > admissible_weight) {
                        continue;
                    }
                    
                    // Accumulate contribution to this cluster
                    if (cluster_candidates.find(leader) == cluster_candidates.end()) {
                        cluster_candidates[leader] = quan_weight;
                    }
                    else {
                        cluster_candidates[leader] += quan_weight;
                    }
                } else {
                    // Neighbor is unclustered
                    double quan_weight = contribution / quan_weight_factor(hg.getNodeWeight(neighbor));

                    if (!adjMatchAreaJudging(hg, node, neighbor, admissible_weight)) {
                        continue;
                    }
                    if (unclustered_candidates.find(neighbor) == unclustered_candidates.end()) {
                        unclustered_candidates[neighbor] = quan_weight;
                    }
                    else {
                        unclustered_candidates[neighbor] += quan_weight;
                    }
                }
            }
        }
        
        // Find best unclustered candidate
        NodeID best_unclustered = INVALID_NODE;
        double best_unclustered_score = 0.0;
        for (const auto& p : unclustered_candidates) {
            if (p.second > best_unclustered_score) {
                best_unclustered_score = p.second;
                best_unclustered = p.first;
            }
        }
        
        // Find best cluster candidate
        NodeID best_cluster_leader = INVALID_NODE;
        double best_cluster_score = 0.0;
        for (const auto& p : cluster_candidates) {
            NodeID leader = p.first;
            // Check cluster size constraint
            if (cluster_size[leader] >= max_cluster_size) continue;
            // NOTE: Type matching removed - per-type weights are tracked separately
            
            if (p.second > best_cluster_score) {
                best_cluster_score = p.second;
                best_cluster_leader = leader;
            }
        }
        
        // No candidates found - create singleton
        if (best_unclustered == INVALID_NODE && best_cluster_leader == INVALID_NODE) {
            cluster_id[node] = num_clusters;
            cluster_leader[node] = node;
            cluster_size[node] = 1;
            cluster_weight[node] = hg.getNodeWeight(node);
            is_clustered[node] = true;
            num_clusters++;
            continue;
        }
        
        Weight node_weight = hg.getNodeWeight(node);
        
        // Compare: cluster accumulated score vs unclustered single node score
        if (best_cluster_score >= best_unclustered_score && best_cluster_leader != INVALID_NODE) {
            // Merge into existing cluster (cluster has higher or equal accumulated contribution)
            NodeID leader = best_cluster_leader;
            cluster_id[node] = cluster_id[leader];  // Use leader's cluster ID
            cluster_leader[node] = leader;
            cluster_size[leader]++;
            cluster_weight[leader] += node_weight;
            is_clustered[node] = true;
        } else if (best_unclustered != INVALID_NODE) {
            // Create new cluster with unclustered neighbor
            cluster_id[node] = num_clusters;
            cluster_id[best_unclustered] = num_clusters;
            cluster_leader[node] = node;  // node is the leader
            cluster_leader[best_unclustered] = node;
            cluster_size[node] = 2;
            cluster_weight[node] = node_weight + hg.getNodeWeight(best_unclustered);
            is_clustered[node] = true;
            is_clustered[best_unclustered] = true;
            num_clusters++;
        } else {
            // Fallback: create singleton (should not reach here)
            cluster_id[node] = num_clusters;
            cluster_leader[node] = node;
            cluster_size[node] = 1;
            cluster_weight[node] = node_weight;
            is_clustered[node] = true;
            num_clusters++;
        }
    }
    
    // Renumber clusters to be contiguous
    std::vector<NodeID> cluster_remap(num_clusters, INVALID_NODE);
    NodeID new_cluster_count = 0;
    
    for (NodeID node = 0; node < num_nodes; ++node) {
        NodeID old_cid = cluster_id[node];
        if (old_cid != INVALID_NODE) {
            if (cluster_remap[old_cid] == INVALID_NODE) {
                cluster_remap[old_cid] = new_cluster_count++;
            }
            cluster_id[node] = cluster_remap[old_cid];
        }
    }
    
    return new_cluster_count;
}

bool ClusterMatching::adjMatchAreaJudging(const Hypergraph& hg,
                                          size_t cluster_size,
                                          Weight cluster_weight,
                                          Weight node_weight) const {
    // If cluster has only 2 nodes, always allow joining
    if (cluster_size <= 2) {
        return true;
    }
    
    // For larger clusters, apply constraints
    // Limit cluster size to prevent too aggressive clustering
    constexpr size_t kMaxClusterSize = 4;
    if (cluster_size >= kMaxClusterSize) {
        return false;
    }
    else
    {
        return true;
    }
    
    // Limit cluster weight (prevent very heavy clusters)
    // Use average node weight as reference
    Weight avg_weight = hg.getTotalNodeWeight() / hg.getNumNodes();
    Weight max_cluster_weight = avg_weight * kMaxClusterSize;
    
    if (cluster_weight + node_weight > max_cluster_weight) {
        return false;
    }
    
    return true;
}

void ClusterMatching::buildCoarseNets(const Hypergraph& fine_hg,
                                      const std::vector<NodeID>& node_mapping,
                                      Hypergraph& coarse_hg) {
    uint64_t kMaxCoarseNetSize = std::max(config_.large_net_threshold, (uint64_t)300);

    // Hash function for vector of sorted node IDs
    struct VectorHash {
        size_t operator()(const std::vector<NodeID>& vec) const {
            size_t hash = vec.size();
            for (NodeID node : vec) {
                hash ^= std::hash<NodeID>()(node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
    
    // Use vector of pair to store nets with their weights (avoid second lookup)
    std::unordered_map<std::vector<NodeID>, size_t, VectorHash> net_to_idx;
    net_to_idx.reserve(fine_hg.getNumNets() / 2);
    
    std::vector<std::vector<NodeID>> net_keys;
    std::vector<Weight> net_weights;
    net_keys.reserve(fine_hg.getNumNets() / 2);
    net_weights.reserve(fine_hg.getNumNets() / 2);
    
    size_t parallel_net_count = 0;
    
    // Reusable buffer to avoid repeated allocations
    std::vector<NodeID> coarse_nodes_buf;
    coarse_nodes_buf.reserve(kMaxCoarseNetSize);
    
    int num_nodes = fine_hg.getNumNodes();
    for (EdgeID fine_net = 0; fine_net < fine_hg.getNumNets(); ++fine_net) {
        coarse_nodes_buf.clear();
        
        auto fine_nodes = fine_hg.getNetNodes(fine_net);
        for (const NodeID* it = fine_nodes.first; it != fine_nodes.second; ++it) {
            NodeID coarse_node = node_mapping[*it];
            if (coarse_node != INVALID_NODE) {
                coarse_nodes_buf.push_back(coarse_node);
            }
        }
        
        if (coarse_nodes_buf.size() <= 1) continue;
        if (coarse_nodes_buf.size() > kMaxCoarseNetSize) continue;
  
        // Sort and remove duplicates (faster than unordered_set for small sizes)
        std::sort(coarse_nodes_buf.begin(), coarse_nodes_buf.end());
        auto last = std::unique(coarse_nodes_buf.begin(), coarse_nodes_buf.end());
        
            coarse_nodes_buf.erase(last, coarse_nodes_buf.end());
        if (coarse_nodes_buf.size() <= 1) continue;
        // Check for parallel net
        auto it = net_to_idx.find(coarse_nodes_buf);
        if (it != net_to_idx.end()) {
            // Parallel net found, merge weights
            net_weights[it->second] += fine_hg.getNetWeight(fine_net);
            parallel_net_count++;
        } else {
            // New net
            size_t idx = net_keys.size();
            net_to_idx[coarse_nodes_buf] = idx;
            net_keys.push_back(coarse_nodes_buf);
            net_weights.push_back(fine_hg.getNetWeight(fine_net));
        }
    }
    
    // Add nets to coarse graph
    coarse_hg.reserveNets(fine_hg.getNumNets());
    for (size_t i = 0; i < net_keys.size(); ++i) {
        EdgeID coarse_net = coarse_hg.addNet(net_weights[i], false);
        for (NodeID coarse_node : net_keys[i]) {
            coarse_hg.addNodeToNet(coarse_net, coarse_node);
        }
    }
}

bool ClusterMatching::adjMatchAreaJudging(const Hypergraph& hg, NodeID node1, NodeID node2, Weight max_weight) const {
    Weight node1_weight = hg.getNodeWeight(node1);
    Weight node2_weight = hg.getNodeWeight(node2);
    if (node1_weight + node2_weight > max_weight) {
        return false;
    }
    return true;
}
} // namespace consmlp

