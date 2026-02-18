# MemDB - High-Performance In-Memory Key-Value Store

A high-performance in-memory database built on Homestore's Btree implementation with support for multiple tables and multiple indices per table. Runs in either **async** (reactor-based) or **sync** (thread-pool) threading models.

## Threading Models

MemDB supports two distinct threading models:

### Async Mode (Reactor-Based)
- **Execution**: All operations run within IOManager reactor threads
- **Initialization**: `init_mem_homedb()` calls `init_iomgr()` to start reactor threads
- **Concurrency**: Operations dispatched to reactors, managed by IOManager
- **Use Case**: High-throughput workloads with async/await patterns

### Sync Mode (Thread-Pool)
- **Execution**: Operations run on caller's thread
- **Initialization**: No reactor setup - user manages their own thread pool
- **Concurrency**: Direct concurrent access via thread-safe APIs
- **Use Case**: Simpler threading model, integration with existing thread pools

HomeDB is built on top of Homestore pluggable btree which supports in-memory or memory-mapped or persistent version.

Pluggable, Multi-Variant Btree
══════════════════════════════════════════════════════════════════════════
```

### The Homestore Btree:

#### 1. **Node Variant System** (Runtime Polymorphism)
   - **3+ node layouts** optimized for different schemas.
   - **Extenable** clear interface for different use case to implement different format without worrying about btree structure.
   - **Zero overhead** for fixed-size keys/values (SimpleNode)
   - **Flexible layouts** for variable data (VarlenNode family)
   - **Prefix compression** for sorted workloads (PrefixCompressNode)
   - **Runtime dispatch** via trait objects (no monomorphization explosion)
   - **Automatic selection** based on KeySpec + ValueSpec

#### 2. **Storage Abstraction** (Write Once, Run Anywhere)
   - **Same btree code** works for in-memory OR persistent storage
   - **Clean separation**: Btree logic vs storage implementation
   - **4 async methods**: read/write/alloc/delete node
   - **Pluggable**: Implement `UnderlyingBtree` for custom backends
   - **Example**: Same btree runs on HashMap, disk files, block devices, or network storage

#### 3. **Configurable Policies**
   - **Node size**: Typically 4KB (default), configurable per btree
   - **Fill percentages**: ideal_fill (90%), suggested_min (30%)
   - **Split policy**: Default 50/50 split, configurable
   - **Inline threshold**: `inline_value_size` (clamped: 8B ≤ x ≤ node_size/32)
   - **Overflow handling**: Large values automatically overflow to separate storage

#### 4. **Concurrency Model**
   - **Fine-grained locking**: Per-node RwLocks (read/write)
   - **Generation counters**: Detect concurrent modifications
   - **Lock-free reads**: Multiple readers can access nodes concurrently
   - **Safe writes**: Exclusive write locks prevent corruption

### MemDB: Database Layer on Top of Btree

MemDB provides **database semantics** on this foundation:
- **Tables**: Logical grouping with schemas (not just raw btrees)
- **Multiple indices per table**: Primary + secondary index support
- **Schema enforcement**: KeySpec + ValueSpec validation
- **Handle-based API**: `Arc<Table>` / `Arc<TableIndex>` for zero-lookup access
- **Dual threading**: Sync (thread-pool) or Async (reactor-based)

**Separation of Concerns**:
```
MemoryDB:     Database abstractions (tables, schemas, handles)
                        ↓
Btree Core:   B+tree algorithms (split, merge, search, iterate)
                        ↓
Node Variants: Layout optimization (fixed vs variable vs compressed)
                        ↓
Storage:      Medium abstraction (memory, disk, custom)
```

One can chose different combinations of node variants, storage, key and value extendability.

## Features

- **Dual Threading Models**: Async (reactor-based) or Sync (thread-pool)
- **Multiple Tables**: Each table is a logical entity with its own schema
- **Multiple Indices per Table**: Primary and secondary indices
- **Handle-Based API**: Zero string lookups in critical paths
- **Flexible Schemas**: Fixed/variable size keys and values, prefix compression
- **Iterator-Based Queries**: Efficient range scans forward and reverse
- **Thread-Safe**: Concurrent-safe APIs and management
- **Zero-Copy**: Minimal allocations where possible

## Build Instructions

### Sync Mode (No async runtime)

```bash
# Build library
cargo build --package mem_db --features sync_mode

# Run tests
cargo test --package mem_db --lib --features sync_mode

# Run benchmark
cargo test --release --package mem_db --test memdb_bench --features sync_mode -- \
  --workers 8 --ops 2000000 --preload 100000
```

### Async Mode (Reactor-based)

```bash
# Build library
cargo build --package mem_db --features async_mode

# Run tests
cargo test --package mem_db --lib --features async_mode

# Run benchmark
cargo test --release --package mem_db --test memdb_bench --features async_mode -- \
  --workers 8 --ops 2000000 --preload 100000
```

**Note**: Never enable both `sync_mode` and `async_mode` simultaneously.

## Quick Start

### Sync Mode Example

```rust
use mem_db::{MemoryDB, TableSpec};
use std::sync::Arc;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Create MemoryDB instance (no singleton, you own it!)
    let db = MemoryDB::new()?;
    
    // Create a table with fixed-size keys and values
    let spec = TableSpec::fixed_kv(8, 16);
    let users = db.create_table("users", spec)?;
    
    // Direct table operations (no string lookup)
    let key = vec![1, 2, 3, 4, 5, 6, 7, 8];
    let value = vec![0u8; 16];
    users.put(key.clone(), value.clone())?;
    
    // Retrieve value
    if let Some(result) = users.get(key)? {
        println!("Value: {:?}", result);
    }
    
    Ok(())
}
```

### Async Mode Example

```rust
use mem_db::{init_mem_homedb, MemoryDB, TableSpec};
use std::sync::Arc;

async fn run() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize IOManager with 4 reactor threads
    init_mem_homedb(4)?;
    
    // Create MemoryDB instance
    let db = MemoryDB::new()?;
    
    // Create a table
    let spec = TableSpec::fixed_kv(8, 16);
    let users = db.create_table("users", spec).await?;
    
    // Async operations
    let key = vec![1, 2, 3, 4, 5, 6, 7, 8];
    let value = vec![0u8; 16];
    users.put(key.clone(), value.clone()).await?;
    
    // Retrieve value
    if let Some(result) = users.get(key).await? {
        println!("Value: {:?}", result);
    }
    
    Ok(())
}

fn main() {
    // Run with tokio or your async runtime
    tokio::runtime::Runtime::new()
        .unwrap()
        .block_on(run())
        .unwrap();
}
```

## API Design: Handle-Based vs String-Based

### 1. Handle-Based (Recommended - Zero Lookups)

```rust
// Get handle once
let users = db.get_table("users")?;

// Use handle repeatedly (fast! No DashMap lookups)
for i in 0..1_000_000 {
    users.put(keys[i].clone(), values[i].clone()).await?;
}
```

### 2. String-Based (Convenience - Has Lookup Overhead)

```rust
// Convenience methods do DashMap lookup every time
for i in 0..1_000_000 {
    db.put_one("users", keys[i].clone(), values[i].clone()).await?; // Slower!
}
```

**Performance tip**: Use handle-based API for hot paths!

## Basic Operations

### Sync Mode Operations

```rust
use mem_db::{MemoryDB, TableSpec};

let db = MemoryDB::new()?;

// Create table
let spec = TableSpec::fixed_kv(8, 128);
let table = db.create_table("users", spec)?;

// Put
table.put(key, value)?;

// Get
if let Some(value) = table.get(key)? {
    println!("Found: {:?}", value);
}

// Remove
table.remove(key)?;

// Range scan
let iter = table.range(start_key..end_key)?;
for result in iter {
    let (k, v) = result?;
    println!("Key: {:?}, Value: {:?}", k, v);
}

// Reverse scan
let iter = table.range_reverse(end_key..start_key)?;
```

### Async Mode Operations

```rust
use mem_db::{init_mem_homedb, MemoryDB, TableSpec};

init_mem_homedb(4)?;
let db = MemoryDB::new()?;

// Create table
let spec = TableSpec::fixed_kv(8, 128);
let table = db.create_table("users", spec).await?;

// Put
table.put(key, value).await?;

// Get
if let Some(value) = table.get(key).await? {
    println!("Found: {:?}", value);
}

// Remove
table.remove(key).await?;

// Range scan (async iterator)
let iter = table.range(start_key..end_key).await?;
while let Some(result) = iter.next().await {
    let (k, v) = result?;
    println!("Key: {:?}, Value: {:?}", k, v);
}

// Reverse scan
let iter = table.range_reverse(end_key..start_key).await?;
```

## Working with Tables and Indices

### Creating Tables

```rust
use mem_db::{MemoryDB, TableSpec, KeySpec, ValueSpec};

let db = MemoryDB::new()?;

// Fixed-size keys and values (fastest)
let spec1 = TableSpec::fixed_kv(8, 128);
let table1 = db.create_table("fixed", spec1).await?;

// Variable-size keys, fixed values
let key_spec = KeySpec::variable(256);  // max 256 bytes
let val_spec = ValueSpec::Fixed(128);
let spec2 = TableSpec::new(key_spec, val_spec);
let table2 = db.create_table("varkey", spec2).await?;

// Both variable
let key_spec = KeySpec::variable(256);
let val_spec = ValueSpec::Variable(1024);  // max 1KB
let spec3 = TableSpec::new(key_spec, val_spec);
let table3 = db.create_table("varvar", spec3).await?;
```

### Secondary Indices

```rust
// Get table handle
let table = db.get_table("users")?;

// Create secondary index on email
let email_spec = TableSpec::variable_kv(128, 8);
table.create_secondary_index("email_idx", email_spec).await?;

// Get index handle
let email_idx = table.get_index("email_idx")?;

// Use the index directly
email_idx.put(b"user@example.com".to_vec(), user_id).await?;
```

### Schema Flexibility

```rust
use mem_db::{TableSpec, KeySpec, PrefixType};

// Prefix compression for sorted keys
let key_spec = KeySpec {
    key_type: KeyType::Variable(128),
    prefix_type: PrefixType::Prefixable(Some(8)),  // 8-byte prefix
};
let spec = TableSpec::new(key_spec, ValueSpec::Fixed(64));
let table = db.create_table("prefixed", spec).await?;
```

## Iterator-Based Queries

### Forward Range Scan

```rust
// Sync mode
let iter = table.range(start_key..end_key)?;
for result in iter {
    let (key, value) = result?;
    // Process...
}

// Async mode
let mut iter = table.range(start_key..end_key).await?;
while let Some(result) = iter.next().await {
    let (key, value) = result?;
    // Process...
}
```

### Reverse Range Scan

```rust
// Sync mode
let iter = table.range_reverse(end_key..start_key)?;
for result in iter {
    let (key, value) = result?;
    // Process in reverse order
}

// Async mode
let mut iter = table.range_reverse(end_key..start_key).await?;
while let Some(result) = iter.next().await {
    let (key, value) = result?;
    // Process in reverse order
}
```

## Performance

MemDB delivers **high-throughput in-memory operations** with excellent scaling characteristics.

**MacBook Pro M1 (2020)**, Release mode, 32B keys, 128B values:

| Configuration | Workers | Tables | Ops/sec |     Workload     |
|---------------|---------|--------|---------|------------------|
| Single table  |    8    |    1   |  2.05M  | 70% PUT, 30% GET |
| Multi-table   |    8    |    8   |  5.3M   | 30% PUT, 70% GET |
| Multi-table   |    16   |    16  |  5.3M   | 30% PUT, 70% GET |

**Key Observations**:
- **Multi-table workloads** achieve ~3x higher throughput (reduced lock contention per table)
- **Read-heavy** workloads are faster than write-heavy
- **Scales with workers + tables**: More parallelism = higher throughput

**Actual performance varies based on**:
- Key/value sizes (larger = lower throughput due to memory bandwidth)
- Operation mix (GET-heavy is faster than PUT-heavy)
- Number of workers/reactors
- Table count (more tables = better parallelism)
- CPU architecture (M1/M2/x86, core count, cache sizes)
- Build mode (**always use release** - debug is 10x+ slower)

**To benchmark your workload**:
```bash
# Write-heavy, single table
cargo test --release --package mem_db --test memdb_bench --features sync_mode -- \
  --workers 8 --ops 2000000 --preload 100000 --put-pct 70

# Read-heavy, multiple tables (better scaling)
cargo test --release --package mem_db --test memdb_bench --features sync_mode -- \
  --workers 16 --ops 10000000 --tables 16 --preload 100000 --put-pct 10
```

### Performance Tips

1. **Use handle-based API**: Avoid string lookups in hot paths
2. **Batch operations**: Amortize per-operation overhead
3. **Choose appropriate threading model**: Sync for simple cases, async for high concurrency
4. **Right-size workers/reactors**: Match your CPU core count
5. **Fixed-size keys/values**: Use `TableSpec::fixed_kv()` when possible (fastest)

## Testing

### Sync Mode

```bash
# Run all tests
cargo test --package mem_db --lib --features sync_mode

# Run specific test
cargo test --package mem_db --lib --features sync_mode test_put_get_convenience

# Run with output
cargo test --package mem_db --lib --features sync_mode -- --nocapture
```

### Async Mode

```bash
# Run all tests  
cargo test --package mem_db --lib --features async_mode

# Run iterator tests
cargo test --package mem_db --features async_mode test_iterator_seek
```

### Benchmarks

```bash
# Sync mode benchmark
cargo test --release --package mem_db --test memdb_bench --features sync_mode -- \
  --workers 8 --ops 2000000 --key-range 1000000 --preload 100000

# Async mode benchmark
cargo test --release --package mem_db --test memdb_bench --features async_mode -- \
  --workers 8 --ops 2000000 --key-range 1000000 --preload 100000

# Custom parameters
cargo test --release --package mem_db --test memdb_bench --features sync_mode -- \
  --workers 4 --ops 100000 --key-range 10000 --preload 1000 \
  --put-pct 50 --key-size 64 --value-size 256
```

## API Reference

### MemoryDB

```rust
impl MemoryDB {
    // Create new MemoryDB instance (you own it!)
    pub fn new() -> Result<Self>;
    
    // Create a table
    pub async fn create_table(&self, name: &str, spec: TableSpec) -> Result<Arc<Table>>;
    
    // Drop a table (cleanup automatic via RAII)
    pub fn drop_table(&self, name: &str) -> Result<()>;
    
    // Get table handle (for repeated use)
    pub fn get_table(&self, name: &str) -> Result<Arc<Table>>;
    
    // List all tables
    pub fn list_tables(&self) -> Vec<String>;
    
    // Convenience methods (have DashMap lookup overhead)
    pub async fn put_one(&self, table: &str, key: Vec<u8>, value: Vec<u8>) -> Result<bool>;
    pub async fn get(&self, table: &str, key: Vec<u8>) -> Result<Option<Vec<u8>>>;
    pub async fn remove(&self, table: &str, key: Vec<u8>) -> Result<bool>;
}

// Drop trait handles shutdown automatically
impl Drop for MemoryDB {
    fn drop(&mut self) {
        // Automatic cleanup - no explicit shutdown needed!
    }
}
```

### Table

```rust
impl Table {
    // Get table metadata
    pub fn name(&self) -> &str;
    pub fn spec(&self) -> &TableSpec;
    
    // Primary index operations
    pub async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<bool>;
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>>;
    pub async fn remove(&self, key: Vec<u8>) -> Result<bool>;
    
    // Secondary indices
    pub async fn create_secondary_index(&self, name: &str, spec: TableSpec) -> Result<()>;
    pub fn get_index(&self, name: &str) -> Result<Arc<TableIndex>>;
    pub fn list_indices(&self) -> Vec<String>;
    
    // Range queries
    pub async fn range(&self, range: impl RangeBounds<Vec<u8>>) -> Result<impl Iterator>;
    pub async fn range_reverse(&self, range: impl RangeBounds<Vec<u8>>) -> Result<impl Iterator>;
}
```

### TableSpec

```rust
impl TableSpec {
    // Convenience constructors
    pub fn fixed_kv(key_size: usize, value_size: usize) -> Self;
    pub fn variable_kv(max_key: usize, max_value: usize) -> Self;
    
    // Full constructor
    pub fn new(key_spec: KeySpec, value_spec: ValueSpec) -> Self;
    
    // Validation
    pub fn validate(&self, key: &[u8], value: &[u8]) -> Result<()>;
}
```

## Comparison with Traditional Databases

| Concept              | Traditional DB       | MemDB                     |
|----------------------|----------------------|---------------------------|
| Process Manager      | DB Engine + Threads  | `init_mem_homedb()` (async) or user thread pool (sync) |
| Database Instance    | `Database`           | `MemoryDB` (owned by user) |
| Table                | `Table`              | `Arc<Table>`               |
| Index                | `Index`              | `Arc<TableIndex>`          |
| Create Table         | `CREATE TABLE`       | `db.create_table(spec)`    |
| Get Table Handle     | Query by name        | `db.get_table(name)`       |
| Insert/Update        | `INSERT`/`UPDATE`    | `table.put(k, v)`          |
| Query                | `SELECT`             | `table.get(k)`             |
| Range Query          | `SELECT ... WHERE`   | `table.range(start..end)`  |
| Multiple Tables      | Multiple tables      | Multiple `Table` handles   |

**Key Design Philosophy**: Handle-based APIs eliminate string lookups in critical paths, similar to high-performance C++ databases.

## Architecture Details

### Memory Management

```
MemoryDB (user-owned, no singleton)
  └── DashMap<String, Arc<Table>>
       │
       └── Table (Arc-wrapped for sharing)
            ├── TableSpec (schema definition)
            └── DashMap<String, Arc<TableIndex>>
                 │
                 └── TableIndex (Arc-wrapped for sharing)
                      ├── IndexType::Primary | Secondary
                      ├── max_key_size (computed from btree config)
                      └── Arc<Btree<DbKey, DbValue>>
                           └── Nodes (automatically cleaned up via RAII)
```

**When a table is dropped:**
1. Remove `Arc<Table>` from MemoryDB's DashMap
2. If no other references exist, Table's Drop is called
3. All `Arc<TableIndex>` references drop
4. All `Arc<Btree>` references drop
5. All btree nodes cleaned up by Rust's RAII
6. **Zero memory leaks!**

### Threading Model Details

**Async Mode (Reactor-Based)**:
- `init_mem_homedb(N)` → starts N reactor threads via `init_iomgr()`
- Operations dispatched to reactors automatically
- Each reactor runs an event loop processing tasks
- Benefits: High throughput, efficient context switching

**Sync Mode (Thread-Pool)**:
- User creates their own thread pool
- Operations execute on caller's thread
- Concurrent access protected by internal locks (DashMap, Arc)
- Benefits: Simpler mental model, easier debugging

## Current Status

MemDB provides a solid foundation for in-memory key-value storage:

- ✅ Multiple tables with flexible schemas
- ✅ Multiple indices per table (primary + secondary)
- ✅ Handle-based API for zero lookups
- ✅ CRUD operations (put, get, remove)
- ✅ Range queries with forward/reverse iteration
- ✅ Thread-safe concurrent access
- ✅ Automatic memory management via RAII
- ✅ Defense-in-depth validation
- ✅ Both async and sync threading models

**Areas for Enhancement**:
- Transactions (cross-table atomicity)
- Persistence layer integration
- Advanced query optimization
- Memory usage monitoring/limits
- Compression options

## License

Licensed under the Apache License, Version 2.0. See LICENSE file for details.
