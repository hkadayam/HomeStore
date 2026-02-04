# COW B-tree Architecture with IndexManager

## Overview

The COW B-tree implementation now uses an IndexManager and 3 separate VDevs for different purposes, providing better separation of concerns and optimized I/O patterns.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      IndexManager                            │
│  - Registers as MetaClient                                   │
│  - Manages COWBtree lifecycle (create/load)                  │
│  - Persists VDev IDs in MetaBlk                             │
└────────────┬────────────────────────────────────────────────┘
             │
             │ creates/loads
             ▼
┌─────────────────────────────────────────────────────────────┐
│                       COWBtree                               │
│                                                              │
│  ┌──────────────────┐  ┌──────────────────┐                │
│  │  Node Cache      │  │  Node ID Map     │                │
│  │  (DashMap)       │  │  (RwLock<Map>)   │                │
│  └──────────────────┘  └──────────────────┘                │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐  │
│  │            3 VDevs (segregated by purpose)           │  │
│  │                                                       │  │
│  │  1. full_map_vdev (SimpleLogStreamVdev)             │  │
│  │     - Stores complete node_id -> blkid mapping       │  │
│  │     - Append-only log structure                      │  │
│  │     - Used during recovery                           │  │
│  │                                                       │  │
│  │  2. incr_map_vdev (SimpleLogStreamVdev)             │  │
│  │     - Stores incremental map updates (journal)       │  │
│  │     - Applied during recovery after full map         │  │
│  │     - Smaller, faster writes                         │  │
│  │                                                       │  │
│  │  3. node_vdev (FixedBlkStreamVdev)                  │  │
│  │     - Stores actual B-tree node data                 │  │
│  │     - Random read/write access                       │  │
│  │     - Block invalidation support                     │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Components

### 1. IndexManager

**File**: `index_manager.rs`

**Purpose**: Manages the lifecycle of COW B-trees and acts as a MetaClient.

**Responsibilities**:
- Register with MetaBlkManager as a client
- Create new COW B-trees via `create_cow_btree()`
- Load existing COW B-trees via `load_cow_btree()`
- Persist B-tree metadata (VDev IDs) in MetaBlk

**Key Methods**:
```rust
pub async fn create() -> io::Result<Self>
pub async fn create_cow_btree(
    name: String,
    ordinal: u32,
    node_size: u32,
    num_sessions: usize,
) -> io::Result<Arc<COWBtree>>

pub async fn load_cow_btree(
    metablk: MetaBlkWrapper,
) -> io::Result<Arc<COWBtree>>
```

### 2. COWBtreeMetaBlk

**File**: `index_manager.rs`

**Purpose**: Metadata structure persisted to MetaBlk.

**Fields**:
```rust
pub struct COWBtreeMetaBlk {
    magic: u64,                 // "COWBTRE" validation
    version: u32,               // Version number
    ordinal: u32,               // B-tree ordinal
    node_size: u32,             // Node size in bytes
    num_sessions: u32,          // Checkpoint sessions
    full_map_vdev_id: u32,      // VDev ID for full map
    incr_map_vdev_id: u32,      // VDev ID for incremental map
    node_vdev_id: u32,          // VDev ID for nodes
    root_node_id: u64,          // Root node ID
    name: [u8; 64],             // B-tree name
}
```

### 3. COWBtree

**File**: `cow_btree.rs`

**Purpose**: Main COW B-tree implementation with 3 VDevs.

**New Fields**:
```rust
full_map_vdev: Arc<SimpleLogStreamVdev>,
incr_map_vdev: Arc<SimpleLogStreamVdev>,
node_vdev: Arc<FixedBlkStreamVdev>,
meta_blk: MetaBlkWrapper,
name: String,
```

**Key Methods**:

#### Creation
```rust
pub async fn create(
    name: String,
    ordinal: u32,
    node_size: u32,
    num_sessions: usize,
    meta_client: Arc<MetaClient>,
) -> io::Result<Arc<Self>>
```

**Process**:
1. Create 3 VDevs with appropriate parameters:
   - `full_map_vdev`: SimpleLogStreamVdev with AppendBlkAllocator
   - `incr_map_vdev`: SimpleLogStreamVdev with AppendBlkAllocator
   - `node_vdev`: FixedBlkStreamVdev with VarsizeBlkAllocator
2. Create COWBtreeMetaBlk with VDev IDs
3. Allocate MetaBlk and persist metadata
4. Return COWBtree instance

#### Loading
```rust
pub async fn load(
    meta_blk: MetaBlkWrapper,
    meta_client: Arc<MetaClient>,
) -> io::Result<Arc<Self>>
```

**Process**:
1. Read metadata from MetaBlk
2. Validate metadata (magic, version)
3. Get VDevs by ID from DeviceManager
4. Reconstruct SimpleLogStreamVdev and FixedBlkStreamVdev wrappers
5. Load node ID map from full_map_vdev and incr_map_vdev
6. Return COWBtree instance

## VDev Usage

### 1. full_map_vdev (SimpleLogStreamVdev)

**Purpose**: Store complete node ID → block ID mapping.

**Characteristics**:
- **Type**: SimpleLogStreamVdev (append-only log)
- **Allocator**: AppendBlkAllocator
- **Block size**: 4KB
- **Chunk size**: 16MB (incremental)
- **Access pattern**: Write during full map flush, read during recovery

**Operations**:
- `append()`: Write full map snapshot
- `recovery_read_at()`: Read during recovery
- `truncate()`: Reset after recovery/upgrade

**When used**:
- Periodically flush complete node ID map (every N checkpoints)
- Recovery: Read to reconstruct initial map state
- Upgrade/migration: Read old map, write new format

### 2. incr_map_vdev (SimpleLogStreamVdev)

**Purpose**: Store incremental node ID map updates (journal).

**Characteristics**:
- **Type**: SimpleLogStreamVdev (append-only log)
- **Allocator**: AppendBlkAllocator
- **Block size**: 4KB
- **Chunk size**: 8MB (incremental)
- **Access pattern**: Write per checkpoint, read during recovery

**Operations**:
- `append()`: Write journal entries (node additions/deletions)
- `recovery_read_at()`: Read during recovery
- `truncate()`: Reset after full map flush

**When used**:
- Every checkpoint: Write incremental updates
- Recovery: Apply journal entries after loading full map
- Truncate after full map flush (journal no longer needed)

### 3. node_vdev (FixedBlkStreamVdev)

**Purpose**: Store actual B-tree node data.

**Characteristics**:
- **Type**: FixedBlkStreamVdev (random access)
- **Allocator**: VarsizeBlkAllocator
- **Block size**: `node_size` (typically 4KB-64KB)
- **Chunk size**: 64MB (incremental)
- **Access pattern**: Random read/write during normal operation

**Operations**:
- `append()`: Write new/modified nodes
- `read()`: Read nodes by BlkId
- `free_blk()`: Invalidate deleted/replaced nodes
- `cp_flush()`: Flush all pending writes for checkpoint

**When used**:
- Normal operation: Read nodes from cache or disk
- Checkpoint: Write all dirty nodes
- Node deletion: Free blocks

## Checkpoint Flow

### Write Path (Checkpoint N)

```
1. Application modifies nodes
   └─> COWBtree::refresh_node() → COW if needed
       └─> Add to CPSession::modified_nodes

2. Checkpoint begins
   └─> COWBtree::flush_nodes(cp_ctx)
       ├─> For each dirty node:
       │   ├─> node_vdev.append(segment_id, buf, session_id)
       │   └─> Update bnodeid_map in memory
       └─> Record updates in memory

3. (Optional) Full map flush (every K checkpoints)
   └─> full_map_vdev.append(serialized_map, session_id)
       └─> truncate incr_map_vdev (no longer needed)

4. Incremental map flush (every checkpoint)
   └─> incr_map_vdev.append(journal_entries, session_id)

5. Checkpoint completes
   └─> node_vdev.cp_flush(session_id)
   ├─> full_map_vdev.flush(session_id)
   └─> incr_map_vdev.flush(session_id)
```

### Recovery Path

```
1. IndexManager::load_cow_btree(metablk)
   └─> Read COWBtreeMetaBlk
       └─> Get 3 VDev IDs

2. COWBtree::load()
   ├─> Reconstruct VDevs from IDs
   └─> Load node ID map:
       ├─> Read full map from full_map_vdev
       │   └─> Build initial bnodeid_map
       └─> Read incremental map from incr_map_vdev
           └─> Apply journal entries to bnodeid_map

3. B-tree ready for use
   └─> Nodes read on-demand from node_vdev via bnodeid_map
```

## Benefits of 3-VDev Design

### 1. Separation of Concerns
- **Map data** (metadata) separate from **node data** (actual content)
- Different access patterns → different VDev types
- Easier to optimize each VDev independently

### 2. Optimized I/O Patterns
- **Full map**: Bulk sequential write, sequential read during recovery
- **Incremental map**: Small sequential appends, sequential read during recovery
- **Nodes**: Random read/write during normal operation

### 3. Faster Recovery
- Load full map (large, infrequent writes)
- Apply incremental map (small, per-checkpoint writes)
- Avoid rebuilding entire map from node traversal

### 4. Flexible Checkpointing
- Choose when to flush full map (trade-off: recovery time vs checkpoint overhead)
- Incremental map keeps journal size bounded
- Can tune full map frequency based on workload

### 5. Better Resource Management
- Each VDev can have different:
  - Chunk sizes (optimized for access pattern)
  - Block allocators (append vs varsize)
  - Chunk pooling strategies
  - Expansion policies

## Configuration

### VDev Parameters

| VDev | Type | Allocator | BlkSize | ChunkSize | Purpose |
|------|------|-----------|---------|-----------|---------|
| full_map | SimpleLogStream | Append | 4KB | 16MB | Full map snapshots |
| incr_map | SimpleLogStream | Append | 4KB | 8MB | Incremental journals |
| nodes | FixedBlkStream | Varsize | node_size | 64MB | B-tree nodes |

### Tuning Parameters

- **Full map flush frequency**: How often to write complete map
  - More frequent → Faster recovery, slower checkpoints
  - Less frequent → Slower recovery, faster checkpoints
  - Recommended: Every 10-100 checkpoints

- **Incremental map truncation**: When to reset journal
  - After full map flush (journal no longer needed)
  - Prevents journal from growing unbounded

- **Node VDev chunk size**: Trade-off between allocation overhead and fragmentation
  - Larger chunks → Less allocation overhead
  - Smaller chunks → Better space efficiency

## Future Enhancements

### 1. Compression
- Compress full map before writing (node IDs are often sequential)
- Compress incremental map entries

### 2. Parallel Recovery
- Read full map and incremental map in parallel
- Apply incremental map while loading nodes

### 3. Incremental Recovery
- Checkpoint the recovery process
- Resume from last checkpoint if recovery is interrupted

### 4. Map Persistence Format
- Optimize serialization format for compactness
- Use delta encoding for sequential node IDs
- RLE compression for contiguous ranges

### 5. Smart Full Map Flushing
- Trigger based on journal size threshold
- Trigger based on recovery time estimate
- Adaptive based on workload characteristics

## Implementation Status

✅ **Completed**:
- IndexManager creation and registration
- COWBtreeMetaBlk structure
- COWBtree::create() with 3 VDevs
- COWBtree::load() skeleton
- VDev ID persistence in MetaBlk

⏳ **TODO**:
- Implement map serialization/deserialization
- Full map flush to full_map_vdev
- Incremental map journal to incr_map_vdev
- Recovery: load map from VDevs
- SimpleLogStreamVdev::load() method
- FixedBlkStreamVdev::load() method
- Integration tests with actual recovery

## Usage Example

```rust
use homestore::index::{IndexManager, COWBtree};

// Create IndexManager
let index_mgr = IndexManager::create().await?;

// Create a new COW B-tree
let btree = index_mgr.create_cow_btree(
    "my_index".to_string(),
    0, // ordinal
    4096, // node_size
    2, // num_sessions
).await?;

// Use the B-tree...
let cp_ctx = CPContext::new(1);
let node = btree.create_node(true, &cp_ctx);
btree.flush_nodes(&cp_ctx).await?;

// Later, during recovery...
let index_mgr = IndexManager::create().await?;
let metablk = /* load from MetaBlkManager */;
let btree = index_mgr.load_cow_btree(metablk).await?;
```
