# Tracing Guide for Homestore

Homestore uses the `tracing` crate for high-performance structured logging with minimal overhead.

## Quick Start

### 1. Initialize Tracing

```rust
use homestore;

fn main() {
    // Initialize tracing with default settings
    homestore::init_tracing();
    
    // Your code here...
}
```

### 2. Control Log Levels via Environment

```bash
# Show info-level logs for all homestore modules
RUST_LOG=homestore=info cargo run

# Show debug logs for btree module
RUST_LOG=homestore::index::btree=debug cargo run

# Show trace logs for remove operations only
RUST_LOG=homestore::index::btree::detail::remove=trace cargo run

# Multiple filters (debug for btree, trace for remove)
RUST_LOG=homestore::index::btree=debug,homestore::index::btree::detail::remove=trace cargo run
```

## Features

### Global Sequential Operation IDs + Btree Names

Every btree operation gets a **global sequential** `op_id` and includes the `btree` name for easy tracking across multiple btree instances:

```
2026-02-05T10:23:45.123Z DEBUG put_one{op_id=1, btree="users_index", key=K(42)}: Starting put operation
2026-02-05T10:23:45.234Z  INFO put_one{op_id=1, btree="users_index", key=K(42)}: Put completed
2026-02-05T10:23:45.345Z DEBUG remove_one{op_id=2, btree="metadata_index", key=K(42)}: Starting remove operation
2026-02-05T10:23:45.456Z  INFO remove_one{op_id=2, btree="metadata_index", key=K(42)}: Key removed successfully
```

The global counter means op_ids are unique across **all** btree instances in your application.

### Concise vs Verbose Key/Value Formatting

Keys and values support two debug formats:

**Concise (default `{:?}`):**
```rust
let key = FixedSizeTestKey::new(42, 1);
println!("{:?}", key);  // Output: K(42)
```

**Verbose (alternate `{:#?}`):**
```rust
let key = FixedSizeTestKey::new(42, 1);
println!("{:#?}", key);  // Output: FixedSizeTestKey { value: 42, id: 1 }
```

In logs, you'll see concise format by default:
```
DEBUG put_one{op_id=1, key=K(42)}: Starting put operation
```

### Hierarchical Spans

Operations automatically create hierarchical contexts:

```
INFO put_one{op_id=1, key=K(42)}: Starting put
DEBUG put_one{op_id=1, key=K(42)}:traverse_tree: Finding leaf node
TRACE put_one{op_id=1, key=K(42)}:traverse_tree:binary_search: Searching...
INFO put_one{op_id=1, key=K(42)}: Put completed
```

### Module-Level Filtering

Filter by specific modules or operations:

```bash
# Only see btree operations (not internal details)
RUST_LOG=homestore::index::btree=info

# See all btree details
RUST_LOG=homestore::index::btree=debug

# See everything in remove.rs
RUST_LOG=homestore::index::btree::detail::remove=trace

# See only specific span names
RUST_LOG=homestore::index::btree[remove_one]=trace
```

## Example Output

### Info Level (Production)
```bash
RUST_LOG=homestore::index::btree=info cargo run
```
```
2026-02-05T10:23:45.123Z  INFO put_one{op_id=1, btree="users_index", key=K(42)}: Put completed
2026-02-05T10:23:45.234Z  INFO remove_one{op_id=2, btree="users_index", key=K(42)}: Key removed successfully
2026-02-05T10:23:45.345Z  INFO query{op_id=3, btree="users_index", batch_size=100}: Query completed result_count=50 has_more=true
```

### Debug Level (Development)
```bash
RUST_LOG=homestore::index::btree=debug cargo run
```
```
2026-02-05T10:23:45.123Z DEBUG put_one{op_id=1, btree="users_index", key=K(42)}: Starting put operation
2026-02-05T10:23:45.234Z  INFO put_one{op_id=1, btree="users_index", key=K(42)}: Put completed
2026-02-05T10:23:45.345Z DEBUG remove_one{op_id=2, btree="users_index", key=K(42)}: Starting remove operation
2026-02-05T10:23:45.456Z DEBUG remove_one{op_id=2, btree="users_index", key=K(42)}: Key found
2026-02-05T10:23:45.567Z  INFO remove_one{op_id=2, btree="users_index", key=K(42)}: Key removed successfully
```

### Trace Level (Deep Debugging)
```bash
RUST_LOG=homestore::index::btree::detail::remove=trace cargo run
```
```
2026-02-05T10:23:45.123Z DEBUG remove_one{op_id=2, btree="users_index", key=K(42)}: Starting remove operation
2026-02-05T10:23:45.234Z TRACE remove_one{op_id=2, btree="users_index", key=K(42)}:find_leaf: Traversing to leaf
2026-02-05T10:23:45.345Z TRACE remove_one{op_id=2, btree="users_index", key=K(42)}:find_leaf: Found leaf node_id=123
2026-02-05T10:23:45.456Z TRACE remove_one{op_id=2, btree="users_index", key=K(42)}:handle_underflow: Checking underflow
2026-02-05T10:23:45.567Z  INFO remove_one{op_id=2, btree="users_index", key=K(42)}: Key removed successfully
```

## Searching Logs

### By Operation ID
```bash
# Find all logs for operation #42
grep "op_id=42" logfile.txt

# Or with ripgrep
rg "op_id=42" logfile.txt
```

### By Time Range
```bash
# Find operations between 10:23:45 and 10:23:50
grep "2026-02-05T10:23:4[5-9]" logfile.txt
```

### By Operation Type
```bash
# Find all remove operations
grep "remove_one{" logfile.txt

# Find all operations on a specific key
grep "key=K(42)" logfile.txt

# Find all operations on a specific btree
grep 'btree="users_index"' logfile.txt

# Find operations on specific btree with specific key
grep 'btree="users_index".*key=K(42)' logfile.txt
```

## Performance

- **Zero cost when disabled**: Trace-level logs have zero overhead if not enabled
- **Minimal overhead when enabled**: ~8-12 million logs/sec throughput
- **Sequential IDs**: Atomic counter, no UUID generation overhead
- **Structured fields**: Easy to parse and filter

## Advanced: JSON Output

For structured log analysis, use JSON format:

```rust
use tracing_subscriber::{fmt, EnvFilter};

tracing_subscriber::fmt()
    .json()
    .with_env_filter(EnvFilter::from_default_env())
    .init();
```

Output:
```json
{
  "timestamp": "2026-02-05T10:23:45.123456Z",
  "level": "INFO",
  "target": "homestore::index::btree::btree",
  "span": {
    "name": "put_one",
    "op_id": 1,
    "btree": "users_index",
    "key": "K(42)"
  },
  "fields": {
    "message": "Put completed"
  }
}
```

Then query with `jq`:
```bash
# Find all operations with op_id=1
jq 'select(.span.op_id == 1)' logfile.json

# Find all operations on users_index btree
jq 'select(.span.btree == "users_index")' logfile.json

# Find all failed operations
jq 'select(.fields.message | contains("failed"))' logfile.json

# Find operations on specific btree with specific key
jq 'select(.span.btree == "users_index" and .span.key == "K(42)")' logfile.json
```

## Tips

1. **Start broad, narrow down**: Begin with `info` level, then increase to `debug` or `trace` for specific modules
2. **Use op_id for tracking**: Follow a single operation through the entire call stack
3. **Module-level filtering**: Target specific files without noise from others
4. **Timestamps are automatic**: Every log entry includes precise timestamps
5. **Concise by default**: Keys show compact format unless you need verbose details
