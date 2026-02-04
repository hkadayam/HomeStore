# SISL Rust - Bitset and Bitword Implementation

This is a Rust translation of the SISL (eBay's Systems Infrastructure Software Library) bitset and bitword data structures from the original C++ implementation.

## Overview

This library provides efficient bit manipulation capabilities with both thread-safe and non-thread-safe variants. It includes:

- **Bitword**: Efficient single-word (64-bit) bit manipulation with support for atomic operations
- **Bitset**: Dynamic bitset implementation with advanced features

## Features

### Bitword Features
- Set/reset individual bits or ranges of bits
- Find next set/reset bits
- Count set/reset bits
- Atomic and non-atomic variants
- Bit filtering for complex bit pattern matching
- Efficient bit manipulation using hardware intrinsics where available

### Bitset Features
- Dynamic resizing
- Efficient set/reset operations on individual bits or ranges
- Finding next set/reset bits across multiple words
- Finding contiguous sequences of reset bits with customizable requirements
- Bit shifting operations (shrink from head)
- Thread-safe variants for concurrent access
- Optimized operations for word-aligned access patterns

## Quick Start

Add this to your `Cargo.toml`:

```toml
[dependencies]
sisl = { path = "path/to/sisl" }
```

### Basic Usage

```rust
use sisl::{UnsafeBitword, UnsafeBitset};

// Bitword example
let mut bitword = UnsafeBitword::from_value(0);
bitword.set_reset_bit(5, true);
assert!(bitword.get_bitval(5));
assert_eq!(bitword.get_set_count(), 1);

// Bitset example
let mut bitset = UnsafeBitset::new(100, 0, false);
bitset.set_bit(42);
bitset.set_bits(10, 5); // Set 5 bits starting at position 10
assert!(bitset.get_bitval(42));
assert_eq!(bitset.get_set_count(0, None), 6);

// Find next set bit
let next_set = bitset.get_next_set_bit(0);
assert_eq!(next_set, 10);
```

### Thread-Safe Usage

```rust
use sisl::{SafeBitword, SafeBitset};

// Thread-safe bitword
let mut atomic_bitword = SafeBitword::from_value(0);
atomic_bitword.set_reset_bit(0, true);

// Thread-safe bitset
let mut thread_safe_bitset = SafeBitset::new(1000, 0, true);
thread_safe_bitset.set_bit(500);
```

### Advanced Features

```rust
use sisl::{UnsafeBitset, BitFilter, BitMatchType};

let mut bitset = UnsafeBitset::new(1000, 0, false);
bitset.set_bits(0, 1000); // Set all bits
bitset.reset_bits(100, 50); // Reset 50 bits starting at position 100

// Find contiguous reset bits
let result = bitset.get_next_contiguous_n_reset_bits(0, 50);
assert_eq!(result.start_bit, 100);
assert_eq!(result.nbits, 50);

// Advanced filtering (equivalent to C++ filtered search)
let bitword = bitset.words[0]; // Access internal bitword for advanced operations
let filter = BitFilter::new(10, 20, 5); // lsb_reqd, mid_reqd, msb_reqd
let match_result = bitword.get_next_reset_bits_filtered(0, &filter);
```

## Thread Safety

The library provides two storage backends:

- **`UnsafeBits<T>`**: Non-atomic storage for single-threaded use (fastest)
- **`SafeBits`**: Atomic storage using `AtomicU64` for thread-safe concurrent access

Type aliases are provided for convenience:
- `UnsafeBitword` = `Bitword<UnsafeBits<u64>>`
- `SafeBitword` = `Bitword<SafeBits>`
- `UnsafeBitset` = `BitsetImpl<UnsafeBits<u64>>`
- `SafeBitset` = `BitsetImpl<SafeBits>`

## Performance Considerations

1. **Use `UnsafeBits` for single-threaded code** - it avoids atomic overhead
2. **Use `SafeBits` only when you need thread safety** - atomic operations have overhead
3. **Batch operations when possible** - `set_bits(start, count)` is more efficient than multiple `set_bit()` calls
4. **Consider alignment** - operations aligned to word boundaries (64-bit) are most efficient

## Differences from C++ Implementation

### Simplified Areas
1. **Serialization**: The complex C++ serialization with alignment and byte_array is simplified
2. **Memory Management**: Rust's ownership system replaces manual memory management
3. **Locking**: Uses Rust's `RwLock` instead of folly::SharedMutex
4. **Error Handling**: Uses `Result<T, String>` instead of exceptions

### Maintained Features
1. **Core Algorithms**: All bit manipulation algorithms are faithfully translated
2. **Performance Characteristics**: Word-level optimizations and lazy compaction preserved
3. **API Compatibility**: Method names and behavior closely match the C++ version
4. **Thread Safety Options**: Both atomic and non-atomic variants available

## Testing

Run the tests:

```bash
cd sisl/rust
cargo test
```

The test suite includes translations of the original C++ tests, covering:
- Basic bit operations
- Word-level operations
- Contiguous bit finding
- Thread safety
- Edge cases and error conditions

## API Reference

### Bitword Methods
- `set_reset_bit(bit, value)` - Set or reset a single bit
- `set_reset_bits(start, count, value)` - Set or reset multiple bits
- `get_bitval(bit)` - Get value of a bit
- `get_next_set_bit(start)` - Find next set bit
- `get_next_reset_bit(start)` - Find next reset bit
- `get_set_count()` - Count set bits
- `get_reset_count()` - Count reset bits

### Bitset Methods
- `new(nbits, id, thread_safe)` - Create new bitset
- `set_bit(bit)` / `reset_bit(bit)` - Set/reset single bit
- `set_bits(start, count)` / `reset_bits(start, count)` - Set/reset multiple bits
- `get_bitval(bit)` - Get bit value
- `get_next_set_bit(start)` / `get_next_reset_bit(start)` - Find next set/reset bit
- `get_next_contiguous_n_reset_bits(start, n)` - Find contiguous reset bits
- `shrink_head(nbits)` - Remove bits from the beginning
- `resize(nbits, value)` - Resize bitset
- `size()` - Get current size
- `get_set_count(start, end)` - Count set bits in range

## License

Licensed under the Apache License, Version 2.0. See the original SISL license headers in the source files.