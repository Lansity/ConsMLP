#ifndef ZXPART_NODE_TYPE_H
#define ZXPART_NODE_TYPE_H

#include <cstdint>
#include <string>

namespace consmlp {

/**
 * @brief Node types in the netlist
 * Represents different hardware components in FPGA design
 */
enum class NodeType : uint8_t {
    LUT = 0,   // Look-Up Table and Flip-Flop
    FF,       // Flip-Flop
    MUX,      // Multiplexer
    CARRY,    // Carry chain
    IO,        // Input/Output
    DSP,       // Digital Signal Processor
    BRAM,      // Block RAM
    OTHER    // Invalid/Unknown type
};

/**
 * @brief Convert NodeType to string
 * @param type Node type
 * @return String representation
 */
inline const char* nodeTypeToString(NodeType type) {
    switch (type) {
        case NodeType::LUT:  return "LUT";
        case NodeType::FF:   return "FF";
        case NodeType::MUX:  return "MUX";
        case NodeType::CARRY:  return "CARRY";
        case NodeType::IO:   return "IO";
        case NodeType::DSP:  return "DSP";
        case NodeType::BRAM: return "BRAM";
        default:             return "OTHER";
    }
}

/**
 * @brief Convert string to NodeType
 * @param str String representation (case-insensitive)
 * @return Node type
 */
inline NodeType stringToNodeType(const std::string& str) {
    // Convert to uppercase for comparison
    std::string upper = str;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }
    }
    
    if (upper == "LUT") {
        return NodeType::LUT;
    } else if (upper == "FF") {
        return NodeType::FF;
    } else if (upper == "IO") {
        return NodeType::IO;
    } else if (upper == "DSP") {
        return NodeType::DSP;
    } else if (upper == "MUX") {
        return NodeType::MUX;
    } else if (upper == "CARRY") {
        return NodeType::CARRY;
    } else if (upper == "BRAM" || upper == "RAM") {
        return NodeType::BRAM;
    } else {
        return NodeType::OTHER;
    }
}

/**
 * @brief Get number of node types
 */
constexpr int NUM_NODE_TYPES = static_cast<int>(NodeType::OTHER) + 1;

/**
 * @brief Check if node type requires relaxed imbalance constraint
 * DSP, BRAM, and IO can have more relaxed imbalance
 */
inline bool isRelaxedImbalanceType(NodeType type) {
    return type == NodeType::DSP || type == NodeType::BRAM || type == NodeType::IO;
}

} // namespace consmlp

#endif // ZXPART_NODE_TYPE_H

