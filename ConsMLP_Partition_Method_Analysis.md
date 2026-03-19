# ConsMLP 分区方法局限性分析报告

## 一、节点类型硬编码问题

### 1.1 当前实现
节点类型在 `include/datastructures/NodeType.h` 中通过枚举硬编码定义：

```cpp
enum class NodeType : uint8_t {
    LUT = 0,   // Look-Up Table
    FF,        // Flip-Flop
    MUX,       // Multiplexer
    CARRY,     // Carry chain
    IO,        // Input/Output
    DSP,       // Digital Signal Processor
    BRAM,      // Block RAM
    OTHER      // Invalid/Unknown type
};
```

类型转换通过硬编码的字符串匹配实现：
```cpp
inline NodeType stringToNodeType(const std::string& str) {
    // 仅支持 LUT, FF, IO, DSP, MUX, CARRY, BRAM/RAM
    // 其他所有字符串都映射为 OTHER
}
```

### 1.2 局限性分析

| 问题 | 影响 |
|------|------|
| 无法动态扩展类型 | 新增资源类型（如 URAM, MMCM, PLL）需修改源代码并重新编译 |
| FPGA 架构绑定 | 仅针对 Xilinx 7 系列架构设计，不适用于其他厂商或新型号 |
| 类型语义固定 | LUT/FF 等类型的资源权重和约束行为在代码中硬编码 |
| 无法支持异构资源 | 同一类型无法区分不同规格（如不同容量的 BRAM） |

### 1.3 改进建议
- 采用配置文件定义资源类型
- 支持用户自定义类型和权重
- 类型系统与约束系统解耦

---

## 二、递归分区的 k 值限制问题

### 2.1 当前实现
递归分区 (`recursive` 模式) 要求 k 必须是 2 的幂次：

```cpp
bool isPowerOfTwo(PartitionID value) {
    return value > 0 && (value & (value - 1)) == 0;
}

// 在参数解析中检查
if (use_recursive_ && !isPowerOfTwo(config_.num_partitions)) {
    std::cout << "[Warning] Recursive mode requires k to be a power of two. "
              << "Falling back to direct k-way partitioning." << std::endl;
    use_recursive_ = false;
}
```

### 2.2 算法原理
递归分区采用**二叉树结构**进行二分：
```
Layer 0:  [0,1,2,3,4,5,6,7] (k=8)
            /            \
Layer 1: [0,1,2,3]    [4,5,6,7] (各 k=4)
          /    \
Layer 2: [0,1] [2,3] (各 k=2)
         /  \
Layer 3: [0] [1] (各 k=1, 终止)
```

### 2.3 非 2 的幂次 k 值的支持方案

#### 方案 A：伪分区填充（Virtual Partitioning）
当 k=5 时，向上取整到最近的 2 的幂次（8），然后合并多余分区：
```
实际划分: [0] [1] [2] [3] [4] [5] [6] [7]
合并后:   [0] [1] [2,3] [4,5,6,7]  
最终:     [0] [1] [2]   [3]
          ↓   ↓   ↓     ↓
         Part0 Part1 Part2 Part3 (k=4) ← 仍不足 k=5
```
**问题**：此方法无法精确控制分区数量。

#### 方案 B：不均等递归划分（推荐）
修改递归策略，允许非均等分割：
```cpp
// 当前实现：均等分割
PartitionID half = target_parts / 2;  // k=5 → 2.5，截断为 2

// 改进方案：按比例分配
// k=5: 分成 2 和 3 两组
PartitionID left_parts = target_parts / 2;           // 2
PartitionID right_parts = target_parts - left_parts; // 3

// 递归继续：
// - 2 → 1,1 (可二分)
// - 3 → 需要特殊处理 → 1,2 → 2 继续分成 1,1
```

**实现挑战**：
1. XML 约束在非均等分割下的分配复杂度增加
2. 需要修改二分终止条件（从 `target_parts == 1` 扩展到支持多种情况）
3. 负载均衡难以保证（2 vs 3 的划分质量可能差异大）

#### 方案 C：混合策略
- 使用 direct k-way 分区作为基础
- 仅在顶层使用递归二分进行粗粒度划分
- 底层使用 direct 模式精化

---

## 三、XML 约束在递归分区中的迭代问题

### 3.1 问题场景描述
**用户描述的场景**：
- XML 定义 4 个分区，每分区容量上限 100 节点
- 设计共 300 节点
- 第 1 次迭代：Part0=200 节点，Part1=100 节点
- 第 2 次迭代：Part0 需再次二分，但每子分区只能有 100 节点，无优化空间

### 3.2 当前约束聚合机制
在 `recursiveBipartitionDirect()` 中，XML 约束通过**求和**方式聚合：

```cpp
// 将 k 个目标分区的约束聚合为当前二分的 2 个约束
for (每个资源类型 type) {
    // 左组 (0 ~ half-1) 的约束求和
    for (pid = group0_start; pid <= group0_end; ++pid) {
        max0 += xml_constraints->getCapacity(pid, type).max_capacity;
    }
    // 右组 (half ~ k-1) 的约束求和
    for (pid = group1_start; pid <= group1_end; ++pid) {
        max1 += xml_constraints->getCapacity(pid, type).max_capacity;
    }
    
    // 当前二分的约束为聚合后的容量
    bipart_constraints.setCapacity(0, type, 0, max0);
    bipart_constraints.setCapacity(1, type, 0, max1);
}
```

### 3.3 问题分析

| 层级 | 目标分区 | 聚合约束 (假设) | 实际节点分布 | 问题 |
|------|---------|----------------|-------------|------|
| L0 | [0,1,2,3] | Part0+Part1 vs Part2+Part3 (各200) | 平分 150 vs 150 | 正常 |
| L1 | [0,1] | Part0 vs Part1 (各100) | 得到 100 vs 50 | **Part1 欠载** |
| L1 | [2,3] | Part2 vs Part3 (各100) | 得到 100 vs 50 | **Part3 欠载** |
| L2 | [1] (细划分) | 约束100，但父分区只有50节点 | 无法填满 | **资源浪费** |

**核心问题**：
1. **约束传递失真**：父层的不均衡分配导致子层约束无法有效利用
2. **欠载分区累积**：早期迭代的欠载会逐层传递，最终无法达到 XML 定义的容量
3. **优化空间不足**：当子分区约束容量 > 实际可分配节点时，算法失去优化动力

### 3.4 改进方案讨论

#### 方案 A：动态约束调整
在每次二分后，根据实际分配结果动态调整下层约束：

```cpp
// 伪代码
void adjustConstraints(parent_allocation, original_constraints) {
    // 计算父分区的实际资源使用率
    usage_ratio = actual_nodes / max_capacity;
    
    // 按比例调整子分区约束
    for (每个子分区) {
        child_constraint = original_constraint * usage_ratio;
    }
}
```

**缺点**：违反 XML 约束的绝对性（用户定义的是硬性上限，不应被调整）。

#### 方案 B：前瞻式划分（Look-ahead Partitioning）
在每次二分前，预估下层划分可行性：

```cpp
// 伪代码
bool checkFeasibility(subgraph, target_constraints) {
    // 检查子图是否能填满目标约束
    for (每个子分区约束 c) {
        if (c.max_capacity > subgraph.total_nodes) {
            return false;  // 不可行
        }
    }
    return true;
}
```

**缺点**：
- 增加计算开销
- 可能需要回滚机制，实现复杂

#### 方案 C：约束松弛与惩罚
允许临时超出约束，但在最终验证时检查：

```cpp
// 伪代码
// 1. 划分时暂时放宽约束
relaxed_constraints = original_constraints * 1.2;  // 放宽20%

// 2. 运行划分
result = partition(subgraph, relaxed_constraints);

// 3. 验证最终约束
if (!checkFinalConstraints(result, original_constraints)) {
    // 触发修复机制
    result = repairPartition(result);
}
```

**缺点**：
- 修复机制复杂
- 无法保证收敛

#### 方案 D：混合模式（推荐）
结合 `direct` 和 `recursive` 的优势：

```
阶段 1 (Recursive): 使用宽松约束进行粗划分
  └── 仅保证负载大致均衡，不考虑严格 XML 约束

阶段 2 (Direct): 在粗划分基础上精化
  └── 使用完整 XML 约束进行局部优化
  └── 允许跨粗分区边界的节点移动
```

### 3.5 临时解决方案
对于当前代码，建议用户：
1. **使用 direct 模式** 替代 recursive 模式处理 XML 约束
2. **放宽 XML 约束** 以预留优化空间（如将 100 提高到 120）
3. **调整 imbalance_factor** 允许更大的不均衡度

---

## 四、总结与建议

| 问题 | 严重程度 | 短期方案 | 长期方案 |
|------|---------|---------|---------|
| 节点类型硬编码 | 中 | 使用 LUT 类型作为通用容器 | 重构为配置文件驱动的类型系统 |
| 递归 k 值限制 | 高 | 使用 direct 模式处理非 2^n k 值 | 实现非均等递归划分算法 |
| XML 约束迭代失真 | 高 | 使用 direct 模式 | 设计约束感知的递归划分策略 |

### 推荐配置
- **k = 2^n (如 4, 8, 16)**：使用 `recursive + all` 模式，兼顾质量与效率
- **k ≠ 2^n (如 3, 5, 6)**：使用 `direct` 模式
- **使用 XML 约束**：优先使用 `direct` 模式，避免递归的约束聚合问题
