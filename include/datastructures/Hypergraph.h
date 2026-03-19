#ifndef ZXPART_HYPERGRAPH_H
#define ZXPART_HYPERGRAPH_H

#include "utils/Types.h"
#include "NodeType.h"
#include <vector>
#include <array>
#include <cassert>
#include <cstddef>

namespace consmlp {

// Type alias for per-type weight storage
using TypeWeights = std::array<Weight, NUM_NODE_TYPES>;

/**
 * @brief High-performance hypergraph data structure
 * 
 * Design principles:
 * - Cache-friendly memory layout
 * - CSR (Compressed Sparse Row) format for adjacency
 * - Index-based access (no pointer chasing)
 * - Compact data structures
 */
class Hypergraph {
public:
    /**
     * @brief Constructor
     * @param num_nodes Number of nodes to pre-allocate
     * @param num_nets Number of nets to pre-allocate
     */
    Hypergraph(NodeID num_nodes = 0, EdgeID num_nets = 0);
    
    /**
     * @brief Destructor
     */
    ~Hypergraph() = default;
    
    // ========== Node Operations ==========
    
    /**
     * @brief Add a node to the hypergraph (single type, backward compatible)
     * @param type Node type (LUT, FF, MUX, CARRY, IO, DSP, BRAM, OTHER)
     * @param weight Node weight
     * @return NodeID of the added node
     */
    NodeID addNode(NodeType type = NodeType::LUT, Weight weight = 1);
    
    /**
     * @brief Add a node with per-type weights (for coarsened nodes)
     * @param type_weights Array of weights for each type
     * @return NodeID of the added node
     */
    NodeID addNodeWithTypeWeights(const TypeWeights& type_weights);
    
    /**
     * @brief Set node attributes (single type, backward compatible)
     * @param node_id Node ID
     * @param type Node type
     * @param weight Node weight
     * @param ignore Whether to ignore this node
     * @param fixed Whether this node is fixed
     */
    void setNodeAttributes(NodeID node_id, NodeType type, Weight weight, 
                          bool ignore = false, bool fixed = false);
    
    /**
     * @brief Get node total weight (sum of all type weights)
     * @param node_id Node ID
     * @return Node weight
     */
    inline Weight getNodeWeight(NodeID node_id) const {
        assert(node_id < num_nodes_);
        return node_weights_[node_id];
    }
    
    /**
     * @brief Get node weight for a specific type
     * @param node_id Node ID
     * @param type Node type
     * @return Weight for that type
     */
    inline Weight getNodeTypeWeight(NodeID node_id, NodeType type) const {
        assert(node_id < num_nodes_);
        return node_type_weights_[node_id][static_cast<size_t>(type)];
    }
    
    /**
     * @brief Get all type weights for a node
     * @param node_id Node ID
     * @return Reference to type weights array
     */
    inline const TypeWeights& getNodeTypeWeights(NodeID node_id) const {
        assert(node_id < num_nodes_);
        return node_type_weights_[node_id];
    }
    
    /**
     * @brief Get node primary type (type with highest weight, for backward compatibility)
     * @param node_id Node ID
     * @return Node primary type
     */
    inline NodeType getNodeType(NodeID node_id) const {
        assert(node_id < num_nodes_);
        return node_primary_types_[node_id];
    }
    
    /**
     * @brief Check if node is ignored
     * @param node_id Node ID
     * @return True if ignored
     */
    inline bool isNodeIgnored(NodeID node_id) const {
        assert(node_id < num_nodes_);
        return node_ignored_[node_id];
    }
    
    /**
     * @brief Check if node is fixed
     * @param node_id Node ID
     * @return True if fixed
     */
    inline bool isNodeFixed(NodeID node_id) const {
        assert(node_id < num_nodes_);
        return node_fixed_[node_id];
    }
    
    /**
     * @brief Set node ignore flag
     * @param node_id Node ID
     * @param ignore Ignore flag
     */
    inline void setNodeIgnored(NodeID node_id, bool ignore) {
        assert(node_id < num_nodes_);
        node_ignored_[node_id] = ignore;
    }
    
    /**
     * @brief Set node fixed flag
     * @param node_id Node ID
     * @param fixed Fixed flag
     */
    inline void setNodeFixed(NodeID node_id, bool fixed) {
        assert(node_id < num_nodes_);
        node_fixed_[node_id] = fixed;
    }
    
    // ========== Net Operations ==========
    
    /**
     * @brief Add a net to the hypergraph
     * @param weight Net weight
     * @param is_global Whether this is a global net
     * @return EdgeID of the added net
     */
    EdgeID addNet(Weight weight = 1, bool is_global = false);
    
    /**
     * @brief Set net attributes
     * @param net_id Net ID
     * @param weight Net weight
     * @param is_global Whether this is a global net
     */
    void setNetAttributes(EdgeID net_id, Weight weight, bool is_global);
    
    /**
     * @brief Get net weight
     * @param net_id Net ID
     * @return Net weight
     */
    inline Weight getNetWeight(EdgeID net_id) const {
        assert(net_id < num_nets_);
        return net_weights_[net_id];
    }
    
    /**
     * @brief Check if net is global
     * @param net_id Net ID
     * @return True if global
     */
    inline bool isNetGlobal(EdgeID net_id) const {
        assert(net_id < num_nets_);
        return net_global_[net_id];
    }
    
    /**
     * @brief Set net global flag
     * @param net_id Net ID
     * @param is_global Global flag
     */
    inline void setNetGlobal(EdgeID net_id, bool is_global) {
        assert(net_id < num_nets_);
        net_global_[net_id] = is_global;
    }
    
    // ========== Connectivity Operations ==========
    
    /**
     * @brief Add a node to a net (create connection)
     * @param net_id Net ID
     * @param node_id Node ID
     */
    void addNodeToNet(EdgeID net_id, NodeID node_id);
    
    /**
     * @brief Finalize the hypergraph structure
     * Must be called after all nodes and nets are added
     * This builds the CSR adjacency structures
     */
    void finalize();
    
    /**
     * @brief Get nodes connected to a net
     * @param net_id Net ID
     * @return Pair of iterators (begin, end) pointing to node IDs
     */
    inline std::pair<const NodeID*, const NodeID*> getNetNodes(EdgeID net_id) const {
        assert(net_id < num_nets_);
        assert(finalized_);
        Index start = net_offsets_[net_id];
        Index end = net_offsets_[net_id + 1];
        return {&net_nodes_[start], &net_nodes_[end]};
    }
    
    /**
     * @brief Get nets connected to a node
     * @param node_id Node ID
     * @return Pair of iterators (begin, end) pointing to net IDs
     */
    inline std::pair<const EdgeID*, const EdgeID*> getNodeNets(NodeID node_id) const {
        assert(node_id < num_nodes_);
        assert(finalized_);
        Index start = node_offsets_[node_id];
        Index end = node_offsets_[node_id + 1];
        return {&node_nets_[start], &node_nets_[end]};
    }
    
    /**
     * @brief Get number of nets connected to a node
     * @param node_id Node ID
     * @return Number of nets (degree)
     */
    inline Index getNodeDegree(NodeID node_id) const {
        assert(node_id < num_nodes_);
        assert(finalized_);
        return node_offsets_[node_id + 1] - node_offsets_[node_id];
    }
    
    /**
     * @brief Get number of nodes in a net
     * @param net_id Net ID
     * @return Number of nodes (net size)
     */
    inline Index getNetSize(EdgeID net_id) const {
        assert(net_id < num_nets_);
        assert(finalized_);
        return net_offsets_[net_id + 1] - net_offsets_[net_id];
    }
    
    // ========== Query Operations ==========
    
    /**
     * @brief Get total number of nodes
     * @return Number of nodes
     */
    inline NodeID getNumNodes() const { return num_nodes_; }
    
    /**
     * @brief Get total number of nets
     * @return Number of nets
     */
    inline EdgeID getNumNets() const { return num_nets_; }
    
    /**
     * @brief Get total number of pins (sum of all net sizes)
     * @return Number of pins
     */
    inline size_t getNumPins() const { return net_nodes_.size(); }
    
    /**
     * @brief Check if hypergraph is finalized
     * @return True if finalized
     */
    inline bool isFinalized() const { return finalized_; }
    
    /**
     * @brief Get total weight of all nodes
     * @return Total node weight
     */
    Weight getTotalNodeWeight() const;
    
    /**
     * @brief Get total weight of nodes by type
     * @param type Node type
     * @return Total weight
     */
    Weight getNodeWeightByType(NodeType type) const;

    inline void reserveNets(EdgeID num_nets) {
        net_weights_.reserve(num_nets);
        net_global_.reserve(num_nets);
        net_offsets_.reserve(num_nets + 1);
        net_nodes_.reserve(num_nets * 5);
    }

    inline void reserveTmpNets(EdgeID num_nets) {
        temp_net_to_nodes_.reserve(num_nets);
    }


private:
    // Allow HgrParser to directly construct CSR for optimal performance
    friend class HgrParser;
    // ========== Node Data (Structure of Arrays) ==========
    NodeID num_nodes_;                          // Number of nodes
    std::vector<TypeWeights> node_type_weights_; // Per-type weights for each node
    std::vector<NodeType> node_primary_types_;   // Primary type (type with highest weight)
    std::vector<Weight> node_weights_;           // Total node weights (sum of type weights)
    std::vector<bool> node_ignored_;             // Ignore flags
    std::vector<bool> node_fixed_;               // Fixed flags
    
    // ========== Net Data (Structure of Arrays) ==========
    EdgeID num_nets_;                       // Number of nets
    std::vector<Weight> net_weights_;       // Net weights
    std::vector<bool> net_global_;          // Global flags
    
    // ========== CSR Adjacency Structure ==========
    // Node -> Nets (which nets each node belongs to)
    std::vector<Index> node_offsets_;       // Offset array (size: num_nodes + 1)
    std::vector<EdgeID> node_nets_;         // Net IDs array
    
    // Net -> Nodes (which nodes each net contains)
    std::vector<Index> net_offsets_;        // Offset array (size: num_nets + 1)
    std::vector<NodeID> net_nodes_;         // Node IDs array
    
    // ========== Temporary Data for Construction ==========
    std::vector<std::vector<NodeID>> temp_net_to_nodes_;  // Temporary storage
    std::vector<std::vector<EdgeID>> temp_node_to_nets_;  // Temporary storage
    
    bool finalized_;                        // Whether the structure is finalized
};

} // namespace consmlp

#endif // ZXPART_HYPERGRAPH_H

