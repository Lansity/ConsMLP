#include "partitioning/PartitionConstraints.h"
#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <limits>

namespace consmlp {

PartitionConstraints::PartitionConstraints(PartitionID num_partitions,
                                          const Configuration& config)
    : num_partitions_(num_partitions)
    , capacity_constraints_(num_partitions, 
                           std::vector<CapacityConstraint>(NUM_TYPES))
{
}

void PartitionConstraints::initializeBalanced(const Hypergraph& hg,
                                             double imbalance_factor) {
    // In default mode (without types file), treat ALL nodes as LUT type
    // Calculate total weight (ignoring actual node types)
    Weight total_weight = 0;
    
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (hg.isNodeIgnored(node_id)) {
            continue;
        }
        total_weight += hg.getNodeWeight(node_id);
    }
    
    // Perfect balance: total_weight / num_partitions
    double perfect_balance = static_cast<double>(total_weight) / num_partitions_;
    
    // Apply imbalance factor
    Weight max_cap = static_cast<Weight>(
        std::ceil(perfect_balance * (1.0 + imbalance_factor))
    );
    Weight min_cap = static_cast<Weight>(
        std::floor(perfect_balance * (1.0 - imbalance_factor))
    );
    
    // Ensure valid range
    if (min_cap < 0) min_cap = 0;
    if (max_cap > total_weight) max_cap = total_weight;
    
    // Set constraints: only LUT type gets the balanced constraint
    // Other types get unlimited capacity (max Weight) to allow any assignment
    for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
        for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
            if (type_idx == static_cast<size_t>(NodeType::LUT)) {
                // LUT type uses balanced constraint
                capacity_constraints_[part_id][type_idx] = 
                    CapacityConstraint(min_cap, max_cap);
            } else {
                // Other types: no constraint (allow any weight)
                capacity_constraints_[part_id][type_idx] = 
                    CapacityConstraint(0, std::numeric_limits<Weight>::max());
            }
        }
    }
    
    // Handle fixed nodes
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (hg.isNodeFixed(node_id)) {
            addFixedNode(node_id, 0);
        }
    }
}

void PartitionConstraints::initializeBalancedWithTypes(const Hypergraph& hg,
                                                       double imbalance_factor,
                                                       double relaxed_multiplier) {
    // Calculate total weight per type (accounting for multi-type nodes)
    std::vector<Weight> total_weights(NUM_TYPES, 0);
    
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (hg.isNodeIgnored(node_id)) {
            continue;
        }
        
        const TypeWeights& tw = hg.getNodeTypeWeights(node_id);
        for (size_t t = 0; t < NUM_TYPES; ++t) {
            total_weights[t] += tw[t];
        }
    }
    
    // Set balanced capacity for each type and partition
    for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
        for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
            NodeType type = static_cast<NodeType>(type_idx);
            Weight total_weight = total_weights[type_idx];
            
            if (total_weight == 0) {
                // No nodes of this type - allow any weight (no constraint)
                capacity_constraints_[part_id][type_idx] = 
                    CapacityConstraint(0, std::numeric_limits<Weight>::max());
                continue;
            }
            
            // Perfect balance: total_weight / num_partitions
            double perfect_balance = static_cast<double>(total_weight) / num_partitions_;
            
            // Determine imbalance factor based on type
            // DSP, BRAM, IO get relaxed imbalance (multiplier * base)
            // LUT, FF, MUX, CARRY, OTHER get base imbalance
            double type_imbalance = imbalance_factor;
            if (isRelaxedImbalanceType(type)) {
                type_imbalance = imbalance_factor * relaxed_multiplier;
                // Cap at reasonable value (e.g., 50% imbalance max)
                // if (type_imbalance > 0.5) {
                //     type_imbalance = 0.5;
                // }
                if (type == NodeType::IO) {
                    type_imbalance = imbalance_factor * relaxed_multiplier * 3;
                }

            }
            
            // Apply imbalance factor
            Weight max_cap = static_cast<Weight>(
                std::ceil(perfect_balance * (1.0 + type_imbalance))
            );
            Weight min_cap = static_cast<Weight>(
                std::floor(perfect_balance * (1.0 - type_imbalance))
            );
            
            // Ensure valid range
            if (min_cap < 0) min_cap = 0;
            if (max_cap > total_weight) max_cap = total_weight;
            
            capacity_constraints_[part_id][type_idx] = 
                CapacityConstraint(min_cap, max_cap);
        }
    }
    
    // Handle fixed nodes
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (hg.isNodeFixed(node_id)) {
            addFixedNode(node_id, 0);
        }
    }
    // print Max resource constraints and min resource constraints for each type
    // for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
    //     NodeType type = static_cast<NodeType>(type_idx);
    //     std::cout << "Max resource constraints for " << nodeTypeToString(type) << ": " << capacity_constraints_[0][type_idx].max_capacity << std::endl;
    //     std::cout << "Min resource constraints for " << nodeTypeToString(type) << ": " << capacity_constraints_[0][type_idx].min_capacity << std::endl;
    // }
}

void PartitionConstraints::setCapacity(PartitionID partition_id, 
                                       NodeType type,
                                       Weight min_capacity, 
                                       Weight max_capacity) {
    assert(partition_id < num_partitions_);
    capacity_constraints_[partition_id][static_cast<size_t>(type)] = 
        CapacityConstraint(min_capacity, max_capacity);
}

CapacityConstraint PartitionConstraints::getCapacity(PartitionID partition_id,
                                                     NodeType type) const {
    assert(partition_id < num_partitions_);
    return capacity_constraints_[partition_id][static_cast<size_t>(type)];
}

bool PartitionConstraints::wouldViolateCapacity(
    PartitionID partition_id, NodeType node_type,
    Weight node_weight, Weight current_weight) const {
    
    CapacityConstraint constraint = getCapacity(partition_id, node_type);
    Weight new_weight = current_weight + node_weight;
    
    return new_weight > constraint.max_capacity;
}

bool PartitionConstraints::wouldViolateCapacityMultiType(
    PartitionID partition_id, 
    const TypeWeights& type_weights,
    const Partition& partition) const {
    
    // Check each type that has non-zero weight
    for (size_t t = 0; t < NUM_TYPES; ++t) {
        if (type_weights[t] > 0) {
            NodeType type = static_cast<NodeType>(t);
            Weight current_weight = partition.getPartitionWeightByType(partition_id, type);
            CapacityConstraint constraint = getCapacity(partition_id, type);
            Weight new_weight = current_weight + type_weights[t];
            
            if (new_weight > constraint.max_capacity) {
                return true;  // Would violate this type's constraint
            }
        }
    }
    
    return false;  // No constraints would be violated
}

bool PartitionConstraints::satisfiesMinimum(const Partition& partition,
                                           PartitionID partition_id,
                                           const Hypergraph& hg) const {
    // Check each node type
    for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
        NodeType type = static_cast<NodeType>(type_idx);
        CapacityConstraint constraint = 
            capacity_constraints_[partition_id][type_idx];
        
        Weight current_weight = partition.getPartitionWeightByType(
            partition_id, type);
        
        if (current_weight < constraint.min_capacity) {
            return false;
        }
    }
    
    return true;
}

bool PartitionConstraints::isBalanced(const Partition& partition,
                                     const Hypergraph& hg) const {
    // Check all partitions satisfy constraints
    for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
        for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
            NodeType type = static_cast<NodeType>(type_idx);
            CapacityConstraint constraint = capacity_constraints_[part_id][type_idx];
            
            Weight current_weight = partition.getPartitionWeightByType(part_id, type);
            
            // In XML constraint mode, only check max capacity (no minimum enforcement)
            if (xml_constraint_mode_) {
                if (current_weight > constraint.max_capacity) {
                    return false;
                }
            } else {
                // Check both min and max constraints
                if (current_weight < constraint.min_capacity ||
                    current_weight > constraint.max_capacity) {
                    return false;
                }
            }
        }
    }
    
    return true;
}

PartitionID PartitionConstraints::initializeFromXML(const std::string& xml_filename) {
    std::ifstream file(xml_filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open XML constraint file: " + xml_filename);
    }
    
    
    // Parse the XML file to count partitions and extract constraints
    std::string line;
    PartitionID current_partition = INVALID_PARTITION;
    PartitionID max_partition_id = 0;
    
    // Temporary storage for parsed constraints
    std::vector<std::vector<Weight>> partition_capacities;
    
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        
        // Check for SLR start tag: <SLRn>
        if (line.size() > 5 && line[0] == '<' && line[1] == 'S' && line[2] == 'L' && line[3] == 'R') {
            // Extract partition ID from <SLRn>
            size_t end_pos = line.find('>');
            if (end_pos != std::string::npos && end_pos > 4) {
                std::string id_str = line.substr(4, end_pos - 4);
                current_partition = static_cast<PartitionID>(std::stoi(id_str));
                
                // Ensure we have enough storage
                if (current_partition >= partition_capacities.size()) {
                    partition_capacities.resize(current_partition + 1, 
                                               std::vector<Weight>(NUM_TYPES, 0));
                }
                
                if (current_partition > max_partition_id) {
                    max_partition_id = current_partition;
                }
            }
            continue;
        }
        
        // Check for SLR end tag: </SLRn>
        if (line.size() > 6 && line[0] == '<' && line[1] == '/' && line[2] == 'S') {
            current_partition = INVALID_PARTITION;
            continue;
        }
        
        // Parse resource constraint: <TYPE> <VALUE>
        if (current_partition != INVALID_PARTITION && line[0] == '<') {
            // Find type name
            size_t type_end = line.find('>');
            if (type_end == std::string::npos) continue;
            
            std::string type_str = line.substr(1, type_end - 1);
            
            // Find value
            size_t value_start = line.find('<', type_end);
            size_t value_end = line.find('>', value_start);
            if (value_start == std::string::npos || value_end == std::string::npos) continue;
            
            std::string value_str = line.substr(value_start + 1, value_end - value_start - 1);
            Weight capacity = static_cast<Weight>(std::stol(value_str));
            
            // Convert type string to NodeType
            NodeType type = stringToNodeType(type_str);
            size_t type_idx = static_cast<size_t>(type);
            
            if (type_idx < NUM_TYPES) {
                partition_capacities[current_partition][type_idx] = capacity;
            }
        }
    }
    
    file.close();
    
    // Update the number of partitions
    PartitionID num_parsed_partitions = max_partition_id + 1;
    
    
    // Resize internal structures
    num_partitions_ = num_parsed_partitions;
    capacity_constraints_.resize(num_partitions_, 
                                 std::vector<CapacityConstraint>(NUM_TYPES));
    
    // Set capacity constraints from parsed values
    // In XML mode, min_capacity = 0 (no minimum enforcement)
    // Types not specified in XML get unlimited capacity
    for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
        for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
            Weight max_cap = partition_capacities[part_id][type_idx];
            
            if (max_cap == 0) {
                // Type not specified in XML - allow unlimited capacity
                capacity_constraints_[part_id][type_idx] = 
                    CapacityConstraint(0, std::numeric_limits<Weight>::max());
            } else {
                // Type specified in XML - use parsed capacity
                capacity_constraints_[part_id][type_idx] = 
                    CapacityConstraint(0, max_cap);  // min=0 for XML mode
            }
        }
    }
    
    // Set XML constraint mode flag
    xml_constraint_mode_ = true;
    
    return num_partitions_;
}

void PartitionConstraints::printConstraintSummary(const Hypergraph& hg) const {
    std::cout << "\n========== Partition Constraints ==========" << std::endl;
    
    if (xml_constraint_mode_) {
        // XML mode: print per-partition capacity constraints
        // Collect types that have constraints
        std::vector<size_t> constrained_types;
        for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
            bool has_constraint = false;
            for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
                if (capacity_constraints_[part_id][type_idx].max_capacity != std::numeric_limits<Weight>::max()) {
                    has_constraint = true;
                    break;
                }
            }
            if (has_constraint) constrained_types.push_back(type_idx);
        }
        
        // Print header
        std::cout << "SLR  ";
        for (size_t type_idx : constrained_types) {
            std::cout << std::setw(10) << nodeTypeToString(static_cast<NodeType>(type_idx));
        }
        std::cout << std::endl;
        
        // Print separator
        std::cout << "---- ";
        for (size_t i = 0; i < constrained_types.size(); ++i) {
            std::cout << "----------";
        }
        std::cout << std::endl;
        
        // Print each partition's constraints
        for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
            std::cout << std::setw(4) << part_id << " ";
            for (size_t type_idx : constrained_types) {
                Weight max_cap = capacity_constraints_[part_id][type_idx].max_capacity;
                if (max_cap == std::numeric_limits<Weight>::max()) {
                    std::cout << std::setw(10) << "-";
                } else {
                    std::cout << std::setw(10) << max_cap;
                }
            }
            std::cout << std::endl;
        }
    }
    std::cout << "===========================================" << std::endl;
}

void PartitionConstraints::printConstraintViolations(const Partition& partition,
                                                     const Hypergraph& hg) const {
    std::cout << "[Constraint Violation Details]" << std::endl;
    
    bool has_violation = false;
    
    for (PartitionID part_id = 0; part_id < num_partitions_; ++part_id) {
        std::cout << "  Partition " << part_id << ":" << std::endl;
        
        for (size_t type_idx = 0; type_idx < NUM_TYPES; ++type_idx) {
            NodeType type = static_cast<NodeType>(type_idx);
            CapacityConstraint constraint = capacity_constraints_[part_id][type_idx];
            Weight current_weight = partition.getPartitionWeightByType(part_id, type);
            
            // Skip types with no weight and unlimited constraint
            if (current_weight == 0 && constraint.max_capacity == std::numeric_limits<Weight>::max()) {
                continue;
            }
            
            bool violated = false;
            std::string reason;
            
            if (xml_constraint_mode_) {
                // XML mode: only check max
                if (current_weight > constraint.max_capacity) {
                    violated = true;
                    reason = "exceeds max";
                }
            } else {
                // Balance mode: check min and max
                if (current_weight < constraint.min_capacity) {
                    violated = true;
                    reason = "below min";
                } else if (current_weight > constraint.max_capacity) {
                    violated = true;
                    reason = "exceeds max";
                }
            }
            
            if (violated || current_weight > 0) {
                std::cout << "    " << std::left << std::setw(8) << nodeTypeToString(type)
                          << ": " << std::right << std::setw(8) << current_weight;
                
                if (constraint.max_capacity == std::numeric_limits<Weight>::max()) {
                    std::cout << " / [0, INF]";
                } else {
                    std::cout << " / [" << constraint.min_capacity << ", " << constraint.max_capacity << "]";
                }
                
                if (violated) {
                    std::cout << " <-- VIOLATED (" << reason << ")";
                    has_violation = true;
                }
                std::cout << std::endl;
            }
        }
    }
    
    if (!has_violation) {
        std::cout << "  No constraint violations found" << std::endl;
    }
    std::cout << std::endl;
}

} // namespace consmlp

