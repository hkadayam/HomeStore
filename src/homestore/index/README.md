# COW B-tree Implementation

This module contains a Rust port of the C++ COWBtree implementation from HomeStore.

## Overview

The Copy-On-Write (COW) B-tree is a persistent B-tree that integrates with HomeStore's checkpoint system. It provides efficient node management with copy-on-write semantics to support concurrent checkpoints.

## Architecture

### Files

- **`btree_node.rs`** - Node data structure
  - Non-persistent version (persistent header skipped for initial port)
  - Uses `ArcSwap<Vec<u8>>` for lock-free COW buffer management
  - Atomic metadata fields (modified_cp_id, nentries, node_gen, level)

- **`cow_btree.rs`** - Main COW B-tree implementation
  - Node cache using `DashMap` for concurrent access
  - Node ID to Block ID mapping using `RwLock<HashMap>`
  - Checkpoint session tracking (dirty/deleted nodes)
  - Integration with VirtualDev for block I/O

- **`tests/`** - Unit and integration tests

## Key Concepts

### Node ID Format

Node IDs are 64-bit values split into:
- **Upper 32 bits**: B-tree ordinal (for multi-tree support)
- **Lower 32 bits**: Compact node ID (unique within this tree)

This allows multiple B-trees to coexist with non-overlapping node ID spaces.

### Copy-On-Write Semantics

When a node is modified:
1. Check if the node's `modified_cp_id` matches the current checkpoint
2. If yes, reuse the existing buffer (already COW'd for this checkpoint)
3. If no, clone the buffer and update `modified_cp_id` (COW operation)
4. Add to the checkpoint's dirty list

### Checkpoint Integration

Each checkpoint session tracks:
- **Modified nodes**: Nodes that need to be flushed to disk
- **Deleted nodes**: Nodes whose blocks should be freed
- **New root ID**: Updated root node (if structure changed)

During checkpoint flush:
1. Write all modified nodes to newly allocated blocks
2. Update the node ID → block ID mapping
3. Free blocks of deleted nodes
4. Persist the updated mapping (not yet implemented)

## Core Operations

### Create Node

```rust
let cp_ctx = CPContext::new(1);
let node = btree.create_node(true, &cp_ctx); // true = leaf
```

- Generates a unique node ID
- Creates node with zero-initialized buffer
- Adds to cache
- Adds to checkpoint's dirty list

### Read Node

```rust
let node = btree.read_node(node_id).await?;
```

- First checks the in-memory cache
- If not found, looks up block ID in the map
- Reads from disk via VirtualDev
- Adds to cache for future access

### Refresh Node (COW)

```rust
let status = btree.refresh_node(&node, true, &cp_ctx);
```

- Called before modifying a node
- Implements COW logic (see above)
- Returns `CpMismatch` if trying to access older CP

### Remove Node

```rust
btree.remove_node(&node, &cp_ctx);
```

- Adds to checkpoint's deleted list
- Removes from cache immediately
- Block freed during `delete_nodes()` after flush

### Flush Nodes

```rust
btree.flush_nodes(&cp_ctx).await?;
```

- Writes all dirty nodes to disk
- Allocates new blocks for each node
- Updates node ID → block ID mapping
- Frees old blocks (COW - old blocks freed on new write)

### Delete Nodes

```rust
btree.delete_nodes(&cp_ctx);
```

- Processes the deleted nodes list
- Removes from node ID → block ID mapping
- Frees the corresponding blocks

## What's Missing (Not Yet Implemented)

### From C++ Version

1. **Persistent Header**: BtreeNode doesn't have persistent header fields yet
2. **Map Persistence**: Node ID → Block ID map recovery from disk
3. **Journal Support**: Incremental journal for map updates
4. **Full Map Flush**: Periodic full map flush with chaining
5. **Superblock Integration**: COW B-tree superblock persistence
6. **Multi-threaded Flush**: Parallel flushing of nodes and map
7. **Crash Recovery**: Loading existing trees from disk
8. **Node Buffer Allocator**: Custom allocator tokens (using default for now)
9. **Metrics & Stats**: Performance counters and diagnostics

### Upper-Layer B-tree Logic

This port only includes the **lower layer** (node management, persistence, checkpointing).
The upper layer B-tree algorithms are not included:
- Key-value operations (put, get, remove, query)
- Tree traversal and search
- Node splitting and merging
- Range queries and iteration
- Lock management for concurrent operations

## Dependencies

- `arc-swap` - Lock-free Arc swapping for COW buffers
- `dashmap` - Concurrent HashMap for node cache
- `parking_lot` - RwLock for node ID map
- `tokio` - Async runtime for I/O operations

## Design Decisions

### Why ArcSwap for Buffers?

Using `ArcSwap<Vec<u8>>` provides:
- Lock-free reads (just an atomic load)
- Efficient COW (clone data, atomic swap)
- Automatic cleanup when no longer referenced
- Safe sharing across checkpoint sessions

Alternative considered: Manual reference counting + mutex = more overhead.

### Why DashMap for Cache?

- Concurrent reads/writes without global lock
- Sharded internally for scalability
- Convenient API (no need for RwLock)

### Why HashMap + RwLock for Map?

- Map is sorted for efficient persistence (future)
- Reads are far more common than writes
- RwLock allows many concurrent readers
- Could be replaced with more sophisticated concurrent map later

## Usage Example

```rust
use homestore::index::{COWBtree, CPContext};
use homestore::device::VirtualDev;
use std::sync::Arc;

// Setup
let vdev = Arc::new(VirtualDev::new(/* ... */));
let btree = Arc::new(COWBtree::new(vdev, 0, 4096, false));

// Checkpoint 1: Create nodes
let cp1 = CPContext::new(1);
let root = btree.create_node(false, &cp1);
let leaf1 = btree.create_node(true, &cp1);
let leaf2 = btree.create_node(true, &cp1);
btree.set_root_node_id(root.node_id(), Some(&cp1));
btree.flush_nodes(&cp1).await?;
btree.finish_checkpoint(&cp1);

// Checkpoint 2: Modify root
let cp2 = CPContext::new(2);
btree.refresh_node(&root, true, &cp2)?; // COW happens here
// ... modify root buffer ...
btree.flush_nodes(&cp2).await?;
btree.finish_checkpoint(&cp2);

// Checkpoint 3: Delete leaf1
let cp3 = CPContext::new(3);
btree.remove_node(&leaf1, &cp3);
btree.flush_nodes(&cp3).await?;
btree.delete_nodes(&cp3);
btree.finish_checkpoint(&cp3);
```

## Next Steps

To complete the COW B-tree port:

1. Add persistent header to BtreeNode
2. Implement map persistence and recovery
3. Add journal support for incremental updates
4. Implement crash recovery
5. Add comprehensive tests with real VirtualDev
6. Performance optimization and benchmarking
7. Port upper-layer B-tree algorithms

## References

- C++ Implementation: `Homestore/src/lib/index/cow_btree/`
- Original Design: See C++ btree.hpp for interface
- Checkpoint Manager: `src/homestore/checkpoint/`
- Virtual Device: `src/homestore/device/virtual_dev.rs`
