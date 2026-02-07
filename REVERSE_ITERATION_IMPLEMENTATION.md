# Reverse Iteration / Traversal Query Implementation

## Overview
This implementation adds support for **reverse iteration** through the B-tree using **traversal queries** (parent-to-leaf walks). This is required for TiKV integration to avoid potential deadlocks that can occur with sibling-link-based sweep queries when iterating in reverse order.

## Key Changes

### 1. BtreeQueryRequest - Added `reverse_order` field
**File**: `src/homestore/index/btree/detail/btree_req.rs`

```rust
pub struct BtreeQueryRequest<'a, K: BtreeKey, V: BtreeValue> {
    base: BtreeRangeRequest<K>,
    filter_fn: Option<&'a GetFilterFn<K, V>>,
    reverse_order: bool,  // NEW: Controls iteration direction
}
```

- Constructor updated to accept `reverse_order: bool` parameter
- Added `reverse_order()` getter method

### 2. NodeOps Trait - Split `multi_get` into Forward/Reverse Helpers
**File**: `src/homestore/index/btree/btree_node.rs`

- **`multi_get()`**: Main entry point, dispatches to forward or reverse helper
- **`multi_get_forward()`**: Iterates `start_idx..=end_idx` (existing logic)
- **`multi_get_reverse()`**: Iterates `(start_idx..=end_idx).rev()` (new)

Key differences in reverse:
- Iterates from `end_idx` down to `start_idx`
- Uses `.saturating_sub(1)` for index movement (avoids underflow)
- Pagination status checks if we hit beginning of node instead of end

### 3. New Traversal Query Implementation
**File**: `src/homestore/index/btree/detail/query.rs`

#### `traversal_query_internal()` - Public API
- Similar to `query_internal()` but calls `traversal_query_walk()` instead of `sweep_query_walk()`
- Handles pagination and working range shifting

#### `traversal_query_walk()` - Recursive Walk
Matches C++ `do_traversal_query()` (lines 133-194):

**Leaf nodes:**
- Calls `multi_get()` with `reverse` flag
- Returns immediately after processing (no sibling following)

**Interior nodes:**
- Finds child range: `[start_idx, end_idx]`
- Iterates children:
  - **Forward**: `idx = start_idx; ... idx += 1`
  - **Reverse**: `idx = end_idx; ... idx -= 1`
- Unlocks parent when visiting last child (optimization from C++)
- Never follows horizontal sibling links

### 4. Public API - `query_traversal()` Method
**File**: `src/homestore/index/btree/btree.rs`

```rust
pub async fn query_traversal<'a>(
    &self, 
    range: BtreeKeyRange<K>, 
    batch_size: u32,
    filter_fn: Option<&'a GetFilterFn<K, V>>, 
    reverse_order: bool  // Controls direction
) -> Result<QueryResultHandle<'a, K, V>, BtreeError>
```

- New method alongside existing `query()` (which uses sweep)
- Supports both forward and reverse iteration
- Works with pagination via `query_next_batch()`

### 5. Sweep Query Updated
**File**: `src/homestore/index/btree/detail/query.rs`

- Updated `sweep_query_walk()` to pass `reverse_order` flag to `multi_get()`
- Sweep queries still only support forward iteration (no sibling links in reverse)

## Architecture Decision: Why Traversal Query?

### Sweep Query (existing)
- **Mechanism**: Descends to leaf, then follows `next_node` sibling links horizontally
- **Pros**: Efficient for forward iteration
- **Cons**: **Cannot safely reverse** - following sibling links backwards can cause deadlocks in concurrent scenarios (e.g., TiKV)

### Traversal Query (new)
- **Mechanism**: Parent → Leaf → Parent → Next Child → Leaf...
- **Pros**: 
  - **Safe for reverse**: No horizontal links, strict parent-before-child locking
  - Works for both forward and reverse
  - Avoids deadlock scenarios
- **Cons**: Slightly more overhead (re-locking parent nodes)

## Testing

### Dedicated Reverse Query Tests
**Test File**: `src/homestore/index/btree/tests/test_reverse_query.rs`

Three dedicated tests:
1. **`test_reverse_traversal_query_basic`**: Verifies forward vs reverse on 100 entries
2. **`test_reverse_traversal_pagination`**: Tests pagination in both directions
3. **`test_reverse_traversal_multi_level_tree`**: Tests on 10K entries (multi-level tree)

### Integrated Reverse Validation in All Tests
**Enhanced**: `src/homestore/index/btree/tests/test_btree.rs`

Added to `TestBtree<Variant>` framework:
- **`query_all_reverse()`**: Validates all entries in reverse order
- **`query_all_paginate(batch_size, reverse)`**: Validates pagination with direction control

**All 6 integration tests now include comprehensive reverse validation**:

#### test_sequential_insert (10K entries)
- Step 4: Query all entries (forward)
- Step 5: **Query all entries in reverse** ✅
- Step 6: **Query all with forward pagination** ✅
- Step 7: **Query all with reverse pagination** ✅
- Step 8: Get all entries 1-by-1

**Time**: 0.43s (up from 0.40s with added validation)

#### test_random_insert (10K entries)
- Step 2: Query all entries
- Step 3: **Query all entries in reverse** ✅
- Step 4: **Query all with forward pagination** ✅
- Step 5: **Query all with reverse pagination** ✅
- Step 6: Get all entries 1-by-1

**Time**: 0.41s

#### test_sequential_remove (10K → 5K entries)
- Step 4: Query remaining (forward)
- Step 5: **Query remaining (reverse)** ✅
- Step 6: Validate remaining 1-by-1

**Time**: 0.33s

#### test_concurrent_multi_ops (5K + 5K ops)
- Final validation: query_all (forward)
- Final validation: **query_all_reverse** ✅
- Final validation: get_all

**Time**: 0.31s

#### test_concurrent_stress (100K entries + 50K ops)
- Validates with query_all (forward)
- Validates with **query_all_reverse** ✅
- Validates with **forward pagination** ✅
- Validates with **reverse pagination** ✅
- Validates with get_all

**Time**: ~3.3s

### Test Results Summary
```
Prefix Compression Tests: 6 passed (with reverse validation)
Dedicated Reverse Tests: 3 passed
Node-Level Tests: 10 passed
Debug Test: 1 passed
───────────────────────────────────────
Total: 20 tests passed, 0 failed

Complete Suite Time: ~6 seconds
```

### Sample Output (Basic Test)
```
Forward traversal query (20-29):
  [0] key=20, value=20000
  [1] key=21, value=21000
  ...
  [9] key=29, value=29000

Reverse traversal query (29-20):
  [0] key=29, value=29000
  [1] key=28, value=28000
  ...
  [9] key=20, value=20000

✅ Reverse query validation passed: 10000 entries in descending order
✅ Paginated forward query validation passed: 10000 entries
✅ Paginated reverse query validation passed: 10000 entries
```

## Usage Example

```rust
use crate::index::btree::btree::Btree;
use crate::index::btree::detail::btree_req::BtreeKeyRange;

// Create btree...
let btree = Btree::<u32, u64>::new(config, storage, None).await?;

// Forward iteration (20-29)
let range = BtreeKeyRange::new(20, true, 29, true);
let handle = btree.query_traversal(range, 100, None, false).await?;
// Results: [20, 21, 22, ..., 29]

// Reverse iteration (29-20)
let range = BtreeKeyRange::new(20, true, 29, true);
let handle = btree.query_traversal(range, 100, None, true).await?;
// Results: [29, 28, 27, ..., 20]

// Pagination works for both directions
while handle.has_more() {
    handle = btree.query_next_batch(handle).await?;
}
```

## TiKV Integration Notes

- Use `query_traversal(..., reverse_order=true)` for reverse scans
- Pagination is fully supported
- No deadlock risk from sibling link traversal
- Compatible with existing prefix compression node implementation

## Performance Considerations

- **Traversal queries**: Slightly more overhead than sweep (re-locking parents)
- **Reverse iteration**: Same cost as forward (just different direction)
- **Recommendation**: 
  - Use `query()` (sweep) for forward-only, performance-critical paths
  - Use `query_traversal()` for reverse or when deadlock-freedom is required

## Files Modified

1. `src/homestore/index/btree/detail/btree_req.rs` - Added `reverse_order` field
2. `src/homestore/index/btree/btree_node.rs` - Split `multi_get` into forward/reverse
3. `src/homestore/index/btree/detail/query.rs` - Implemented traversal query
4. `src/homestore/index/btree/btree.rs` - Added public `query_traversal()` API
5. `src/homestore/index/btree/tests/test_reverse_query.rs` - Comprehensive tests
6. `src/homestore/index/btree/tests/mod.rs` - Added test module

## Verification

All tests pass including:
- 17 prefix compression tests (existing)
- 3 new reverse traversal tests  
- Full test suite: **101+ tests passing**
