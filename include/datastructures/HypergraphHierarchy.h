#ifndef ZXPART_HYPERGRAPH_HIERARCHY_H
#define ZXPART_HYPERGRAPH_HIERARCHY_H

#include "Hypergraph.h"
#include "utils/Types.h"
#include <vector>
#include <memory>

namespace consmlp {

/**
 * @brief Represents a single level in the hypergraph hierarchy
 * 
 * Maintains mapping between current level and coarser level:
 * - Forward mapping: node in current level -> node in coarser level
 * - Backward mapping: node in coarser level -> nodes in current level
 */
class HypergraphLevel {
public:
    /**
     * @brief Constructor
     * @param level Level index (0 = finest, increasing = coarser)
     */
    explicit HypergraphLevel(uint32_t level);
    
    /**
     * @brief Get the hypergraph at this level
     * @return Reference to hypergraph
     */
    inline Hypergraph& getHypergraph() { return hypergraph_; }
    inline const Hypergraph& getHypergraph() const { return hypergraph_; }
    
    /**
     * @brief Set forward mapping (current node -> coarser node)
     * @param node_id Node ID in current level
     * @param coarse_node_id Node ID in coarser level
     */
    inline void setCoarserNode(NodeID node_id, NodeID coarse_node_id) {
        if (node_id >= node_to_coarser_.size()) {
            node_to_coarser_.resize(node_id + 1, INVALID_NODE);
        }
        node_to_coarser_[node_id] = coarse_node_id;
    }
    
    /**
     * @brief Get coarser node mapping
     * @param node_id Node ID in current level
     * @return Node ID in coarser level
     */
    inline NodeID getCoarserNode(NodeID node_id) const {
        return node_id < node_to_coarser_.size() ? 
               node_to_coarser_[node_id] : INVALID_NODE;
    }
    
    /**
     * @brief Add a fine node to coarse node's mapping
     * @param coarse_node_id Node ID in coarser level
     * @param fine_node_id Node ID in current (finer) level
     */
    void addFinerNode(NodeID coarse_node_id, NodeID fine_node_id);
    
    /**
     * @brief Get all fine nodes that were merged into a coarse node
     * @param coarse_node_id Node ID in coarser level
     * @return Pair of iterators (begin, end) pointing to fine node IDs
     */
    inline std::pair<const NodeID*, const NodeID*> 
    getFinerNodes(NodeID coarse_node_id) const {
        if (coarse_node_id >= coarser_to_finer_offsets_.size() - 1) {
            return {nullptr, nullptr};
        }
        Index start = coarser_to_finer_offsets_[coarse_node_id];
        Index end = coarser_to_finer_offsets_[coarse_node_id + 1];
        return {&coarser_to_finer_nodes_[start], &coarser_to_finer_nodes_[end]};
    }
    
    /**
     * @brief Finalize the reverse mapping (coarse -> fine nodes)
     * Must be called after all mappings are set
     */
    void finalizeMapping();
    
    /**
     * @brief Get level index
     * @return Level index
     */
    inline uint32_t getLevel() const { return level_; }
    
    /**
     * @brief Check if this level has a coarser level
     * @return True if coarser level exists
     */
    inline bool hasCoarserLevel() const { 
        return !node_to_coarser_.empty(); 
    }

private:
    uint32_t level_;                            // Level index
    Hypergraph hypergraph_;                     // Hypergraph at this level
    
    // Forward mapping: node in this level -> node in coarser level
    std::vector<NodeID> node_to_coarser_;
    
    // Reverse mapping (CSR format): node in coarser level -> nodes in this level
    std::vector<Index> coarser_to_finer_offsets_;  // Size: #coarse_nodes + 1
    std::vector<NodeID> coarser_to_finer_nodes_;   // Fine node IDs
    
    // Temporary storage for building reverse mapping
    std::vector<std::vector<NodeID>> temp_coarser_to_finer_;
};

/**
 * @brief Manages the hierarchy of hypergraphs for multilevel partitioning
 * 
 * Performance design:
 * - Stores multiple levels of hypergraphs
 * - Maintains efficient mapping between levels
 * - Supports both coarsening and uncoarsening phases
 */
class HypergraphHierarchy {
public:
    /**
     * @brief Constructor with the finest level hypergraph
     * @param finest_hypergraph The original (finest) hypergraph
     */
    explicit HypergraphHierarchy(Hypergraph&& finest_hypergraph);
    
    /**
     * @brief Default constructor
     */
    HypergraphHierarchy() = default;
    
    /**
     * @brief Get number of levels in the hierarchy
     * @return Number of levels
     */
    inline uint32_t getNumLevels() const { 
        return static_cast<uint32_t>(levels_.size()); 
    }
    
    /**
     * @brief Get a specific level
     * @param level Level index (0 = finest)
     * @return Reference to the level
     */
    inline HypergraphLevel& getLevel(uint32_t level) {
        assert(level < levels_.size());
        return *levels_[level];
    }
    
    inline const HypergraphLevel& getLevel(uint32_t level) const {
        assert(level < levels_.size());
        return *levels_[level];
    }
    
    /**
     * @brief Get the finest (original) level
     * @return Reference to finest level
     */
    inline HypergraphLevel& getFinestLevel() {
        assert(!levels_.empty());
        return *levels_[0];
    }
    
    inline const HypergraphLevel& getFinestLevel() const {
        assert(!levels_.empty());
        return *levels_[0];
    }
    
    /**
     * @brief Get the coarsest level
     * @return Reference to coarsest level
     */
    inline HypergraphLevel& getCoarsestLevel() {
        assert(!levels_.empty());
        return *levels_.back();
    }
    
    inline const HypergraphLevel& getCoarsestLevel() const {
        assert(!levels_.empty());
        return *levels_.back();
    }
    
    /**
     * @brief Add a new coarser level to the hierarchy
     * @return Reference to the newly created level
     */
    HypergraphLevel& addCoarserLevel();
    
    /**
     * @brief Set the finest hypergraph
     * @param hypergraph The finest hypergraph
     */
    void setFinestHypergraph(Hypergraph&& hypergraph);
    
    /**
     * @brief Map node from finest level to a specific level
     * @param node_id Node ID in finest level
     * @param target_level Target level
     * @return Node ID in target level
     */
    NodeID mapNodeToLevel(NodeID node_id, uint32_t target_level) const;
    
    /**
     * @brief Map node from a specific level to finest level
     * This returns all nodes in the finest level that were merged into this node
     * @param node_id Node ID in the source level
     * @param source_level Source level
     * @return Vector of node IDs in finest level
     */
    std::vector<NodeID> mapNodeToFinestLevel(NodeID node_id, 
                                             uint32_t source_level) const;
    
    /**
     * @brief Clear the hierarchy
     */
    void clear();

private:
    // Levels of hypergraphs (0 = finest, back = coarsest)
    std::vector<std::unique_ptr<HypergraphLevel>> levels_;
};

} // namespace consmlp

#endif // ZXPART_HYPERGRAPH_HIERARCHY_H

