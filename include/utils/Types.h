#ifndef ZXPART_TYPES_H
#define ZXPART_TYPES_H

#include <cstdint>
#include <vector>
#include <limits>

namespace consmlp {

// Basic types for hypergraph partitioning
using NodeID = uint32_t;      // Node/Vertex ID
using EdgeID = uint32_t;      // Hyperedge ID
using PartitionID = uint32_t; // Partition ID
using Weight = int32_t;       // Weight type
using Index = uint32_t;       // Array index type

// Constants
constexpr NodeID INVALID_NODE = std::numeric_limits<NodeID>::max();
constexpr EdgeID INVALID_EDGE = std::numeric_limits<EdgeID>::max();
constexpr PartitionID INVALID_PARTITION = std::numeric_limits<PartitionID>::max();

} // namespace consmlp

#endif // ZXPART_TYPES_H

