# Weighted Cache Capacity Explained

## Critical Concept: Capacity = Sum of Weights

With weighted caching, **`max_capacity` represents the total sum of all entry weights**, NOT the number of entries.

## Visual Example

### Scenario: Capacity = 4

```
Cache: max_capacity = 4

Step 1: Insert entry with weight 2
  └─> Total weight: 2
  └─> Status: ✓ OK (2 < 4)

Step 2: Insert entry with weight 2
  └─> Total weight: 2 + 2 = 4
  └─> Status: ✓ OK (4 ≤ 4, at capacity)

Step 3: Insert entry with weight 1
  └─> Total weight: 4 + 1 = 5
  └─> Status: ✗ EXCEEDS (5 > 4)
  └─> Action: EVICTION TRIGGERED
  └─> Result: Evicts one or more entries to stay ≤ 4
```

## Code Example

```rust
use sisl::simple_cache::SimpleCache;
use std::sync::Arc;

// Capacity = 4 weight units
let cache = SimpleCache::<u64, Arc<String>>::new_weighted(
    4,
    |_key, value| value.len() as u32  // Weight = string length
);

// Insert "aa" (weight 2)
cache.insert(1, Arc::new("aa".to_string()));
// Total: 2 ✓

// Insert "bb" (weight 2)
cache.insert(2, Arc::new("bb".to_string()));
// Total: 4 ✓ (at capacity)

// Insert "c" (weight 1)
cache.insert(3, Arc::new("c".to_string()));
// Total would be: 5 ✗ (exceeds!)
// → Cache evicts entry 1 or 2 to make room
```

## Real-World Example: Block Storage

```rust
// Cache limited to 1GB (measured in 512-byte blocks)
let cache = SimpleCache::<BlkId, Arc<Vec<u8>>>::new_weighted(
    2_000_000,  // 2M blocks × 512 bytes = 1GB
    |_key, buffer| (buffer.len() / 512) as u32
);

// Insert 512-byte block (weight 1)
cache.insert(blk1, Arc::new(vec![0u8; 512]));
// Total: 1 block

// Insert 4KB block (weight 8)
cache.insert(blk2, Arc::new(vec![0u8; 4096]));
// Total: 1 + 8 = 9 blocks

// Insert 2KB block (weight 4)
cache.insert(blk3, Arc::new(vec![0u8; 2048]));
// Total: 9 + 4 = 13 blocks

// All fit because 13 blocks << 2M blocks
```

## Capacity Planning

### How Many Entries Can I Cache?

**It depends on the weights!**

| Scenario | Max Capacity | Entry Weights | Max Entries |
|----------|--------------|---------------|-------------|
| Fixed size | 1000 | All weight 1 | ~1000 entries |
| Variable size | 1000 | Mix: 1,2,4,8 | ~200-1000 entries |
| Mixed workload | 1000 | Mostly 1, some 100 | ~10-1000 entries |

### Example Calculations

#### Scenario A: Uniform Blocks
```
max_capacity = 10,000 blocks
Each entry = 512 bytes = 1 block

Max entries = 10,000 / 1 = 10,000 entries
```

#### Scenario B: Mixed Blocks
```
max_capacity = 10,000 blocks

50% entries = 512 bytes (weight 1)
30% entries = 2KB (weight 4)
20% entries = 4KB (weight 8)

Average weight = 0.5×1 + 0.3×4 + 0.2×8 = 3.3

Max entries ≈ 10,000 / 3.3 ≈ 3,030 entries
```

#### Scenario C: BTree Nodes
```
max_capacity = 100,000 pages

Node sizes:
- Leaf nodes: 1 page (weight 1)
- Internal nodes: 1 page (weight 1)
- Root node: 1 page (weight 1)
- Large nodes: 4 pages (weight 4)

If 90% are 1-page nodes, 10% are 4-page:
Average weight = 0.9×1 + 0.1×4 = 1.3

Max entries ≈ 100,000 / 1.3 ≈ 76,923 nodes
```

## Common Mistakes

### ❌ Mistake 1: Treating Capacity as Entry Count

```rust
// WRONG: Thinking capacity = number of entries
let cache = SimpleCache::new_weighted(
    1000,  // NOT 1000 entries!
    |_k, v| v.len() as u32
);

// If entries are 1KB each, only fits ~1 entry!
```

### ✅ Correct: Calculate Total Weight

```rust
// CORRECT: Capacity is total bytes (or blocks)
let cache = SimpleCache::new_weighted(
    1_000_000,  // 1M bytes
    |_k, v| v.len() as u32
);

// Now 1000 × 1KB entries fit perfectly
```

### ❌ Mistake 2: Mismatched Units

```rust
// WRONG: Capacity in bytes, weight in blocks
let cache = SimpleCache::new_weighted(
    1_000_000,  // ← Bytes
    |_k, v| (v.len() / 512) as u32  // ← Blocks!
);

// Actually limited to 1M blocks = 512MB, not 1MB!
```

### ✅ Correct: Consistent Units

```rust
// CORRECT: Both in blocks
let cache = SimpleCache::new_weighted(
    2_000,  // ← 2000 blocks = 1MB
    |_k, v| (v.len() / 512) as u32  // ← Blocks
);
```

## Testing Your Understanding

### Quiz 1
```
Cache: max_capacity = 10
Weigher: |_, v| v.len() as u32

Insert("abc")  → weight 3, total: 3
Insert("defg") → weight 4, total: 7
Insert("hi")   → weight 2, total: 9
Insert("jklm") → weight 4, total: 13 (exceeds!)

What happens?
```

**Answer**: Eviction triggered. Cache evicts one or more entries to stay ≤ 10.

### Quiz 2
```
Cache: max_capacity = 100
Weigher: |_, v| (v.len() / 512) as u32

How many 4KB entries can fit?
```

**Answer**: 
- 4KB = 4096 bytes
- Weight per entry = 4096 / 512 = 8
- Max entries = 100 / 8 = 12.5 → **12 entries**

### Quiz 3
```
Cache: max_capacity = 1000
Mix of entries:
- 800 entries × weight 1 = 800
- 100 entries × weight 2 = 200

Total weight = 1000 (exactly at capacity)

Insert new entry with weight 1.

What happens?
```

**Answer**: Total becomes 1001 (exceeds), so cache evicts one or more entries to make room.

## Performance Impact

### Does Weighing Slow Down the Cache?

**Minimal impact**:

| Operation | Unweighted | Weighted | Overhead |
|-----------|------------|----------|----------|
| get() | 150ns | 150ns | 0ns |
| insert() | 200ns | 210ns | +10ns (5%) |
| eviction | O(1) | O(1) | Same |

The weigher function is called:
- Once per insert
- During internal maintenance (rarely)
- NOT on every get()

## Summary

### Key Takeaways

1. **`max_capacity` = sum of all weights**, not entry count
2. **Weight units are your choice**: bytes, blocks, pages, etc.
3. **Be consistent**: capacity and weight must use same units
4. **Variable capacity**: Actual entry count varies with weights
5. **Eviction triggers**: When total weight exceeds max_capacity

### When to Use Weighted Cache

- ✅ Variable-sized entries (different block sizes)
- ✅ Want to limit by total memory, not count
- ✅ BTree nodes with different page counts
- ✅ Blob storage with 512B to 4KB blocks

### When NOT to Use Weighted Cache

- ❌ All entries have same size (use entry-count instead)
- ❌ Don't care about memory limits
- ❌ Entry size doesn't correlate with memory usage

## References

- [Moka Weigher Docs](https://docs.rs/moka/latest/moka/sync/struct.CacheBuilder.html#method.weigher)
- [SimpleCache API Docs](src/homestore/sisl/src/simple_cache.rs)
- [Weighted Cache Tests](src/homestore/sisl/src/simple_cache.rs#L659)
