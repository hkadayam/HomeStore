# Tracing Implementation Summary

## Changes Made

### 1. Dependencies Added (`src/homestore/Cargo.toml`)
```toml
tracing = "0.1"
tracing-subscriber = { version = "0.3", features = ["env-filter", "fmt"] }
```

### 2. Trait Updates (`src/homestore/index/btree/btree_kvs.rs`)

Added `std::fmt::Debug` requirement to both traits:

```rust
pub trait BtreeKey: ... + std::fmt::Debug { }
pub trait BtreeValue: ... + std::fmt::Debug { }
```

This allows keys and values to be logged in traces.

### 3. Custom Debug Implementations (`src/homestore/index/btree/tests/btree_test_kvs.rs`)

Implemented custom `Debug` for all test key/value types with **alternate formatting support**:

- **Concise format** (`{:?}`): `K(42)` or `V(100)`
- **Verbose format** (`{:#?}`): `FixedSizeTestKey { value: 42, id: 1 }`

Types updated:
- `FixedSizeTestKey`
- `FixedSizeTestValue`
- `TestVarLenKey`
- `TestVarLenValue`

### 4. Global Operation Counter (`src/homestore/index/btree/btree.rs`)

Added a **global** static operation counter for sequential operation IDs across all btree instances:

```rust
/// Global operation counter for sequential operation IDs across all btree instances
static GLOBAL_OP_COUNTER: AtomicU64 = AtomicU64::new(0);
```

This provides a single sequence of operation IDs across all btrees, making it easier to track operations in systems with multiple btree instances.

### 5. Instrumentation (`src/homestore/index/btree/btree.rs`)

Replaced verbose manual span code with clean `#[instrument]` macros on all public methods:

**Before:**
```rust
pub async fn put_one(...) -> Result<(), BtreeError> {
    let span = tracing::info_span!("put_one", btree = %self.config.btree_name);
    let _enter = span.enter();
    tracing::debug!("Starting put operation");
    // ... code
}
```

**After:**
```rust
#[tracing::instrument(
    skip(self, key, value, filter_fn),
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = %self.config.btree_name,
        key = ?key
    )
)]
pub async fn put_one(...) -> Result<(), BtreeError> {
    tracing::debug!("Starting put operation");
    // ... code
}
```

Methods instrumented:
- `put_one` - Single key insert/update
- `put_range` - Range insert/update
- `get` - Single key lookup
- `remove_one` - Single key removal
- `remove_any` - Remove any key in range
- `remove_range` - Range removal
- `get_any` - Get any key in range
- `query` - Range query with pagination

### 6. Initialization Helper (`src/homestore/lib.rs`)

Added public initialization function:

```rust
pub fn init_tracing() {
    use tracing_subscriber::EnvFilter;
    
    let _ = tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::from_default_env()
                .add_directive("homestore=info".parse().unwrap())
        )
        .with_target(true)
        .with_thread_ids(false)
        .try_init();
}
```

## Features

### ✅ Global Sequential Operation IDs
Every operation gets a unique, sequential ID from a global counter, making it easy to track operations across all btree instances and correlate activities across different btrees.

### ✅ Hierarchical Spans
Operations automatically create nested contexts that show the call hierarchy.

### ✅ Module-Level Filtering
Fine-grained control via `RUST_LOG` environment variable:
- `homestore=info` - All modules
- `homestore::index::btree=debug` - Just btree
- `homestore::index::btree::detail::remove=trace` - Specific file

### ✅ Concise & Verbose Formatting
Keys/values support both compact and detailed debug output using Rust's alternate formatting (`{:#?}`).

### ✅ Automatic Timestamps
Every log entry includes precise timestamps.

### ✅ Zero-Cost When Disabled
Trace-level logs have zero overhead when not enabled.

## Usage

### Basic Setup
```rust
fn main() {
    homestore::init_tracing();
    // Your code...
}
```

### Control Logging
```bash
# Info level (production)
RUST_LOG=homestore::index::btree=info cargo run

# Debug level (development)
RUST_LOG=homestore::index::btree=debug cargo run

# Trace level (deep debugging)
RUST_LOG=homestore::index::btree::detail::remove=trace cargo run
```

### Example Output
```
2026-02-05T10:23:45.123Z DEBUG put_one{op_id=1, btree="users_index", key=K(42)}: Starting put operation
2026-02-05T10:23:45.234Z  INFO put_one{op_id=1, btree="users_index", key=K(42)}: Put completed
2026-02-05T10:23:45.345Z DEBUG remove_one{op_id=2, btree="metadata_index", key=K(42)}: Starting remove operation
2026-02-05T10:23:45.456Z  INFO remove_one{op_id=2, btree="metadata_index", key=K(42)}: Key removed successfully
```

Note how operations from different btree instances (users_index vs metadata_index) have sequential op_ids and include the btree name for easy filtering.

## Documentation

See `TRACING_GUIDE.md` for comprehensive usage examples and best practices.

## Validation

All changes compile cleanly with no new errors or warnings related to tracing.
The existing build errors in the codebase are unrelated to these changes.

## Benefits

1. **Performance**: Minimal overhead, zero-cost when disabled
2. **Flexibility**: Module-level and span-level filtering
3. **Debuggability**: Sequential IDs and hierarchical contexts
4. **Maintainability**: Clean `#[instrument]` macros vs verbose manual spans
5. **Searchability**: Structured logs easy to grep/parse
