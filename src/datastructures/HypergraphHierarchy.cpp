#include "datastructures/HypergraphHierarchy.h"
#include <algorithm>
#include <cassert>

namespace consmlp {

// ========== HypergraphLevel Implementation ==========

HypergraphLevel::HypergraphLevel(uint32_t level)
    : level_(level)
    , hypergraph_()
{
}

void HypergraphLevel::addFinerNode(NodeID coarse_node_id, NodeID fine_node_id) {
    // Ensure temporary storage is large enough
    if (coarse_node_id >= temp_coarser_to_finer_.size()) {
        temp_coarser_to_finer_.resize(coarse_node_id + 1);
    }
    temp_coarser_to_finer_[coarse_node_id].push_back(fine_node_id);
}

void HypergraphLevel::finalizeMapping() {
    if (temp_coarser_to_finer_.empty()) {
        return;
    }
    
    // Build CSR structure for reverse mapping
    size_t num_coarse_nodes = temp_coarser_to_finer_.size();
    coarser_to_finer_offsets_.resize(num_coarse_nodes + 1);
    coarser_to_finer_offsets_[0] = 0;
    
    // Calculate offsets
    size_t total_fine_nodes = 0;
    for (size_t i = 0; i < num_coarse_nodes; ++i) {
        total_fine_nodes += temp_coarser_to_finer_[i].size();
        coarser_to_finer_offsets_[i + 1] = total_fine_nodes;
    }
    
    // Copy fine node IDs to flat array
    coarser_to_finer_nodes_.reserve(total_fine_nodes);
    for (size_t i = 0; i < num_coarse_nodes; ++i) {
        // Sort for better cache performance in queries
        std::sort(temp_coarser_to_finer_[i].begin(), 
                 temp_coarser_to_finer_[i].end());
        
        for (NodeID fine_node : temp_coarser_to_finer_[i]) {
            coarser_to_finer_nodes_.push_back(fine_node);
        }
    }
    
    // Clear temporary storage
    temp_coarser_to_finer_.clear();
    temp_coarser_to_finer_.shrink_to_fit();
}

// ========== HypergraphHierarchy Implementation ==========

HypergraphHierarchy::HypergraphHierarchy(Hypergraph&& finest_hypergraph) {
    setFinestHypergraph(std::move(finest_hypergraph));
}

void HypergraphHierarchy::setFinestHypergraph(Hypergraph&& hypergraph) {
    clear();
    auto level = std::unique_ptr<HypergraphLevel>(new HypergraphLevel(0));
    level->getHypergraph() = std::move(hypergraph);
    levels_.push_back(std::move(level));
}

HypergraphLevel& HypergraphHierarchy::addCoarserLevel() {
    uint32_t new_level = static_cast<uint32_t>(levels_.size());
    auto level = std::unique_ptr<HypergraphLevel>(new HypergraphLevel(new_level));
    levels_.push_back(std::move(level));
    return *levels_.back();
}

NodeID HypergraphHierarchy::mapNodeToLevel(NodeID node_id, 
                                           uint32_t target_level) const {
    assert(target_level < levels_.size());
    
    if (target_level == 0) {
        return node_id;  // Already at finest level
    }
    
    // Traverse from finest to target level
    NodeID current_node = node_id;
    for (uint32_t level = 0; level < target_level; ++level) {
        current_node = levels_[level]->getCoarserNode(current_node);
        if (current_node == INVALID_NODE) {
            return INVALID_NODE;
        }
    }
    
    return current_node;
}

std::vector<NodeID> HypergraphHierarchy::mapNodeToFinestLevel(
    NodeID node_id, uint32_t source_level) const {
    
    assert(source_level < levels_.size());
    
    if (source_level == 0) {
        return {node_id};  // Already at finest level
    }
    
    // Start with the node at source level
    std::vector<NodeID> current_nodes = {node_id};
    std::vector<NodeID> next_nodes;
    
    // Traverse from source level down to finest level
    for (int level = source_level - 1; level >= 0; --level) {
        next_nodes.clear();
        
        // For each node in current level, get its finer nodes
        for (NodeID node : current_nodes) {
            auto fine_nodes = levels_[level]->getFinerNodes(node);
            
            // Add all finer nodes
            for (const NodeID* it = fine_nodes.first; 
                 it != fine_nodes.second; ++it) {
                next_nodes.push_back(*it);
            }
        }
        
        current_nodes = std::move(next_nodes);
    }
    
    // Sort for consistent output
    std::sort(current_nodes.begin(), current_nodes.end());
    
    return current_nodes;
}

void HypergraphHierarchy::clear() {
    levels_.clear();
}

} // namespace consmlp

