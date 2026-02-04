# Node ID Format Specification

## Overview

COWBtree uses a 64-bit node ID with a 3-piece structure that encodes the B-tree ordinal, overflow flag, and node number.

## Bit Layout

```
┌─────────────────┬──────┬─────────────────────────────────────────────┐
│   Ordinal (16)  │ OF(1)│         Node Number (47)                    │
│   [63:48]       │ [47] │         [46:0]                              │
└─────────────────┴──────┴─────────────────────────────────────────────┘

Total: 64 bits

Where:
- Ordinal: B-tree ordinal (0-65535) for multi-tree support
- OF: Overflow flag (0=regular/leaf/interior, 1=overflow)
- Node Number: Unique node counter within this tree (0-140,737,488,355,327)
```

## Field Details

### 1. B-tree Ordinal (16 bits, [63:48])
- **Range**: 0 to 65,535 (2^16 - 1)
- **Purpose**: Identifies which B-tree this node belongs to
- **Allows**: Up to 65,536 independent B-trees in the system
- **Location**: Upper 16 bits

### 2. Overflow Flag (1 bit, [47])
- **Values**: 
  - `0` = Regular node (interior or leaf)
  - `1` = Overflow node
- **Purpose**: Distinguishes overflow nodes from regular nodes
- **Location**: Bit 47

### 3. Node Number (47 bits, [46:0])
- **Range**: 0 to 140,737,488,355,327 (2^47 - 1)
- **Purpose**: Unique sequential counter for nodes within this B-tree
- **Allows**: ~140 trillion nodes per B-tree
- **Location**: Lower 47 bits

## Constants

```rust
const BTREE_ORDINAL_BITS: u64 = 16;
const BTREE_OVERFLOW_BIT: u64 = 1;
const BTREE_NODE_NUMBER_BITS: u64 = 47;

const BTREE_ORDINAL_SHIFT: u64 = 48;
const BTREE_OVERFLOW_SHIFT: u64 = 47;

const BTREE_NODE_NUMBER_MASK: u64 = (1u64 << 47) - 1;  // 0x0000_7FFF_FFFF_FFFF
const BTREE_OVERFLOW_MASK: u64 = 1u64 << 47;            // 0x0000_8000_0000_0000
const BTREE_ORDINAL_MASK: u64 = 0xFFFF << 48;           // 0xFFFF_0000_0000_0000
```

## Precomputed Prefixes

To optimize node ID generation, COWBtree precomputes two prefixes during initialization:

### Regular ID Prefix
```rust
regular_id_prefix = (ordinal as u64) << 48
```
Format: `[ordinal(16)][0(1)][0000...0000(47)]`

Example for ordinal=5:
```
0x0005_0000_0000_0000
  ││││ │             
  ││││ └─ Overflow bit = 0 (regular)
  │││└─── Node number = 0
  ││└──── Reserved for node number
  │└───── Reserved for node number  
  └────── Ordinal = 5
```

### Overflow ID Prefix
```rust
overflow_id_prefix = ((ordinal as u64) << 48) | (1u64 << 47)
```
Format: `[ordinal(16)][1(1)][0000...0000(47)]`

Example for ordinal=5:
```
0x0005_8000_0000_0000
  ││││ │             
  ││││ └─ Overflow bit = 1 (overflow)
  │││└─── Node number = 0
  ││└──── Reserved for node number
  │└───── Reserved for node number  
  └────── Ordinal = 5
```

## Node ID Generation

### Regular Node
```rust
fn generate_node_id(&self, is_overflow: bool) -> BNodeId {
    let node_number = self.next_node_id.fetch_add(1, Ordering::AcqRel) as u64;
    
    if is_overflow {
        self.overflow_id_prefix | node_number
    } else {
        self.regular_id_prefix | node_number
    }
}
```

### Example Generations

For B-tree with ordinal=5, generating nodes sequentially:

| Call | Type | Node# | Generated ID | Binary Representation |
|------|------|-------|--------------|----------------------|
| 1st | Regular | 0 | 0x0005_0000_0000_0000 | `0000 0000 0000 0101 0000 ...` |
| 2nd | Regular | 1 | 0x0005_0000_0000_0001 | `0000 0000 0000 0101 0000 ... 0001` |
| 3rd | Overflow | 2 | 0x0005_8000_0000_0002 | `0000 0000 0000 0101 1000 ... 0010` |
| 4th | Regular | 3 | 0x0005_0000_0000_0003 | `0000 0000 0000 0101 0000 ... 0011` |

## Extraction Operations

### Extract Ordinal
```rust
fn extract_ordinal(node_id: BNodeId) -> u16 {
    ((node_id & BTREE_ORDINAL_MASK) >> BTREE_ORDINAL_SHIFT) as u16
}
```

Example:
```
Node ID: 0x0005_8000_0000_0002
Mask:    0xFFFF_0000_0000_0000
Result:  0x0005_0000_0000_0000
Shift:   >> 48
Final:   0x0005 (5)
```

### Check Overflow
```rust
fn is_overflow_node(node_id: BNodeId) -> bool {
    (node_id & BTREE_OVERFLOW_MASK) != 0
}
```

Example:
```
Regular:  0x0005_0000_0000_0001 & 0x0000_8000_0000_0000 = 0 (false)
Overflow: 0x0005_8000_0000_0002 & 0x0000_8000_0000_0000 ≠ 0 (true)
```

### Extract Node Number (Compact ID)
```rust
fn to_compact_nodeid(node_id: BNodeId) -> CompactNodeId {
    (node_id & BTREE_NODE_NUMBER_MASK) as CompactNodeId
}
```

Example:
```
Node ID: 0x0005_8000_0000_0042
Mask:    0x0000_7FFF_FFFF_FFFF
Result:  0x0000_0000_0000_0042 (66)
```

## Node Size Selection

Nodes have different sizes based on type:

```rust
pub fn create_node(&self, is_leaf: bool, is_overflow: bool, cp_ctx: &CPContext) -> BtreeNodePtr {
    let node_id = self.generate_node_id(is_overflow);
    
    let node_size = if is_overflow {
        self.overflow_node_size
    } else if is_leaf {
        self.leaf_node_size
    } else {
        self.interior_node_size
    };
    
    // ... create node with appropriate size
}
```

## Metadata Storage

In `COWBtreeMetadata`, the ordinal is stored as u32 for alignment, but only the lower 16 bits are used:

```rust
pub struct COWBtreeMetadata {
    pub ordinal: u32,  // Only lower 16 bits used
    // ...
}
```

Conversion during create/load:
```rust
// Create: u16 → u32
let meta_data = COWBtreeMetadata::new(
    &name,
    ordinal as u32,  // Convert u16 to u32
    // ...
);

// Load: u32 → u16
let ordinal = meta_data.ordinal as u16;  // Convert u32 to u16
```

## Benefits

### 1. Multi-Tree Support
- 65,536 independent B-trees (16-bit ordinal)
- Each tree has isolated node ID space
- No collision between trees

### 2. Overflow Node Identification
- Single bit flag distinguishes overflow nodes
- No need to read node metadata to determine type
- Fast overflow detection: single bit test

### 3. Large Node Capacity
- 47 bits = 140 trillion nodes per tree
- More than sufficient for any practical B-tree
- Allows very large indexes

### 4. Fast Node ID Generation
- Precomputed prefixes eliminate shifts
- Single OR operation to combine prefix + node_number
- Atomic counter for node_number

### 5. Efficient Extraction
- All extractions are simple bit masking operations
- No expensive arithmetic
- Inline-able for hot paths

## Comparison with Old Format

### Old Format (32-bit ordinal + 32-bit compact ID)
```
[31:0] Ordinal | [31:0] Compact ID
- 2^32 B-trees (4 billion)
- 2^32 nodes per tree (4 billion)
- No overflow distinction
```

### New Format (16-bit ordinal + 1-bit overflow + 47-bit node number)
```
[15:0] Ordinal | [1] Overflow | [46:0] Node Number
- 2^16 B-trees (65K) - more reasonable
- 2^47 nodes per tree (140 trillion) - more than enough
- Explicit overflow flag
```

**Trade-offs**:
- ✅ Added overflow flag (critical feature)
- ✅ More nodes per tree (47 vs 32 bits)
- ❌ Fewer total trees (16 vs 32 bits)
  - Still sufficient (65K B-trees is generous)

## Usage Examples

### Creating Nodes

```rust
// Create regular leaf node
let leaf = btree.create_node(true, false, &cp_ctx);
// Node ID: 0x0005_0000_0000_0001 (ordinal=5, overflow=0, node#=1)

// Create overflow node
let overflow = btree.create_node(false, true, &cp_ctx);
// Node ID: 0x0005_8000_0000_0002 (ordinal=5, overflow=1, node#=2)

// Create interior node
let interior = btree.create_node(false, false, &cp_ctx);
// Node ID: 0x0005_0000_0000_0003 (ordinal=5, overflow=0, node#=3)
```

### Inspecting Node IDs

```rust
let node_id: BNodeId = 0x0005_8000_0000_0042;

let ordinal = COWBtree::extract_ordinal(node_id);
// ordinal = 5

let is_overflow = COWBtree::is_overflow_node(node_id);
// is_overflow = true

let node_number = COWBtree::to_compact_nodeid(node_id);
// node_number = 66
```

## Validation

### Node Number Range Check
```rust
fn generate_node_id(&self, is_overflow: bool) -> BNodeId {
    let node_number = self.next_node_id.fetch_add(1, Ordering::AcqRel) as u64;
    
    assert!(node_number <= BTREE_NODE_NUMBER_MASK, 
            "Node number {} exceeds 47-bit limit", node_number);
    
    // ... generate ID
}
```

This ensures we never overflow the 47-bit node number field.

## Persistence

Node IDs are 64-bit values that can be directly serialized:
- No special encoding needed
- Can be stored as-is in node ID → block ID map
- Full 64-bit value preserved in metadata (root_node_id)

## Thread Safety

Node ID generation is thread-safe:
- `next_node_id` is `AtomicU32` with `fetch_add`
- Prefixes are precomputed and immutable
- OR operation is atomic and side-effect free
- Each thread gets unique node number

## Future Extensions

The format leaves room for future extensions:

1. **Additional flags**: Could use some of the 47 node number bits
2. **Version bits**: Could steal a few bits for node format versioning
3. **Type encoding**: Could use 2-3 bits to encode node type directly

However, the current format is sufficient for all foreseeable needs.
