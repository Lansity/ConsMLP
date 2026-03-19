#include "datastructures/Hypergraph.h"
#include <algorithm>

namespace consmlp {

Hypergraph::Hypergraph(NodeID num_nodes, EdgeID num_nets)
    : num_nodes_(0)
    , num_nets_(0)
    , finalized_(false)
{
    // Pre-allocate space for better performance
    if (num_nodes > 0) {
        node_type_weights_.reserve(num_nodes);
        node_primary_types_.reserve(num_nodes);
        node_weights_.reserve(num_nodes);
        node_ignored_.reserve(num_nodes);
        node_fixed_.reserve(num_nodes);
        temp_node_to_nets_.reserve(num_nodes);
    }
    
    if (num_nets > 0) {
        net_weights_.reserve(num_nets);
        net_global_.reserve(num_nets);
        temp_net_to_nodes_.reserve(num_nets);
    }
}

NodeID Hypergraph::addNode(NodeType type, Weight weight) {
    assert(!finalized_ && "Cannot add nodes after finalization");
    
    NodeID node_id = num_nodes_++;
    
    // Create type weights array with only the specified type having weight
    TypeWeights type_weights = {};  // Zero-initialize all
    type_weights[static_cast<size_t>(type)] = weight;
    
    // Add node attributes
    node_type_weights_.push_back(type_weights);
    node_primary_types_.push_back(type);
    node_weights_.push_back(weight);
    node_ignored_.push_back(false);
    node_fixed_.push_back(false);
    
    // Initialize temporary storage
    temp_node_to_nets_.emplace_back();
    
    return node_id;
}

NodeID Hypergraph::addNodeWithTypeWeights(const TypeWeights& type_weights) {
    assert(!finalized_ && "Cannot add nodes after finalization");
    
    NodeID node_id = num_nodes_++;
    
    // Calculate total weight and find primary type
    Weight total_weight = 0;
    NodeType primary_type = NodeType::LUT;
    Weight max_type_weight = 0;
    
    for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
        total_weight += type_weights[t];
        if (type_weights[t] > max_type_weight) {
            max_type_weight = type_weights[t];
            primary_type = static_cast<NodeType>(t);
        }
    }
    
    // Add node attributes
    node_type_weights_.push_back(type_weights);
    node_primary_types_.push_back(primary_type);
    node_weights_.push_back(total_weight);
    node_ignored_.push_back(false);
    node_fixed_.push_back(false);
    
    // Initialize temporary storage
    temp_node_to_nets_.emplace_back();
    
    return node_id;
}

void Hypergraph::setNodeAttributes(NodeID node_id, NodeType type, Weight weight,
                                   bool ignore, bool fixed) {
    assert(node_id < num_nodes_ && "Invalid node ID");
    
    // Reset type weights and set only the specified type
    TypeWeights& type_weights = node_type_weights_[node_id];
    for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
        type_weights[t] = 0;
    }
    type_weights[static_cast<size_t>(type)] = weight;
    
    node_primary_types_[node_id] = type;
    node_weights_[node_id] = weight;
    node_ignored_[node_id] = ignore;
    node_fixed_[node_id] = fixed;
}

EdgeID Hypergraph::addNet(Weight weight, bool is_global) {
    assert(!finalized_ && "Cannot add nets after finalization");
    
    EdgeID net_id = num_nets_++;
    
    // Add net attributes
    net_weights_.push_back(weight);
    net_global_.push_back(is_global);
    
    // Initialize temporary storage
    temp_net_to_nodes_.emplace_back();
    
    return net_id;
}

void Hypergraph::setNetAttributes(EdgeID net_id, Weight weight, bool is_global) {
    assert(net_id < num_nets_ && "Invalid net ID");
    
    net_weights_[net_id] = weight;
    net_global_[net_id] = is_global;
}

void Hypergraph::addNodeToNet(EdgeID net_id, NodeID node_id) {
    assert(!finalized_ && "Cannot add connections after finalization");
    assert(net_id < num_nets_ && "Invalid net ID");
    assert(node_id < num_nodes_ && "Invalid node ID");
    
    // Add to temporary storage (both directions)
    temp_net_to_nodes_[net_id].push_back(node_id);
    temp_node_to_nets_[node_id].push_back(net_id);
}

void Hypergraph::finalize() {
    if (finalized_) {
        return;
    }
    
    // Build CSR structure for Net -> Nodes
    net_offsets_.resize(num_nets_ + 1);
    net_offsets_[0] = 0;
    
    size_t total_pins = 0;
    for (EdgeID net_id = 0; net_id < num_nets_; ++net_id) {
        total_pins += temp_net_to_nodes_[net_id].size();
        net_offsets_[net_id + 1] = total_pins;
    }
    
    net_nodes_.reserve(total_pins);
    for (EdgeID net_id = 0; net_id < num_nets_; ++net_id) {
        // Sort nodes for better cache performance
        std::sort(temp_net_to_nodes_[net_id].begin(), 
                 temp_net_to_nodes_[net_id].end());
        
        // Copy to CSR array
        for (NodeID node_id : temp_net_to_nodes_[net_id]) {
            net_nodes_.push_back(node_id);
        }
    }
    
    // Build CSR structure for Node -> Nets
    node_offsets_.resize(num_nodes_ + 1);
    node_offsets_[0] = 0;
    
    size_t total_connections = 0;
    for (NodeID node_id = 0; node_id < num_nodes_; ++node_id) {
        total_connections += temp_node_to_nets_[node_id].size();
        node_offsets_[node_id + 1] = total_connections;
    }
    
    node_nets_.reserve(total_connections);
    for (NodeID node_id = 0; node_id < num_nodes_; ++node_id) {
        // Sort nets for better cache performance
        std::sort(temp_node_to_nets_[node_id].begin(),
                 temp_node_to_nets_[node_id].end());
        
        // Copy to CSR array
        for (EdgeID net_id : temp_node_to_nets_[node_id]) {
            node_nets_.push_back(net_id);
        }
    }
    
    // Clear temporary storage to free memory
    temp_net_to_nodes_.clear();
    temp_net_to_nodes_.shrink_to_fit();
    temp_node_to_nets_.clear();
    temp_node_to_nets_.shrink_to_fit();
    
    finalized_ = true;
}

Weight Hypergraph::getTotalNodeWeight() const {
    Weight total = 0;
    for (NodeID node_id = 0; node_id < num_nodes_; ++node_id) {
        if (!node_ignored_[node_id]) {
            total += node_weights_[node_id];
        }
    }
    return total;
}

Weight Hypergraph::getNodeWeightByType(NodeType type) const {
    Weight total = 0;
    size_t type_idx = static_cast<size_t>(type);
    for (NodeID node_id = 0; node_id < num_nodes_; ++node_id) {
        if (!node_ignored_[node_id]) {
            total += node_type_weights_[node_id][type_idx];
        }
    }
    return total;
}

} // namespace consmlp
