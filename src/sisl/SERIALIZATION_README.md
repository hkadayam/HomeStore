# Bitset Serialization - C++ Equivalent Functionality

This implementation provides the same serialization functionality as the C++ `bitset.hpp` with direct I/O support and zero-copy operations.

## Features

### 1. Aligned Buffer System
- **4KB aligned memory allocation** for direct I/O operations
- **RAII memory management** with automatic cleanup
- **Zero-copy design** for maximum performance

### 2. C-Compatible Serialization Format
- **`#[repr(C, packed)]` struct layout** for direct binary compatibility
- **Magic number validation** (0x42495453 = "BITS")
- **Version control** for format evolution
- **Metadata preservation** (id, nbits, skip_bits, word_count)

### 3. Direct I/O Support
- **File I/O methods** for persistent storage
- **Aligned buffer operations** for kernel bypass I/O
- **Complete data integrity** validation

## API Overview

```rust
// Core serialization methods
pub fn serialize(&self) -> Result<AlignedBuffer, String>
pub fn deserialize(buffer: &AlignedBuffer) -> Result<Self, &'static str>
pub fn serialized_size(&self) -> usize

// Direct I/O convenience methods
pub fn write_to_file<P: AsRef<std::path::Path>>(&self, path: P) -> Result<(), Box<dyn std::error::Error>>
pub fn read_from_file<P: AsRef<std::path::Path>>(path: P) -> Result<Self, Box<dyn std::error::Error>>
```

## Usage Examples

### Basic Serialization
```rust
use sisl::{Bitset, AtomicBitset};

// Create and populate a bitset
let mut bitset = Bitset::new(1024, 42);
bitset.set_bit(100);
bitset.set_bit(500);
bitset.set_bit(1000);

// Serialize to aligned buffer
let buffer = bitset.serialize().expect("Serialization failed");

// Deserialize back
let restored = Bitset::deserialize(&buffer).expect("Deserialization failed");
assert_eq!(restored.get_id(), 42);
assert!(restored.get_bitval(100));
```

### File I/O Operations
```rust
// Write bitset to file with direct I/O
bitset.write_to_file("data.bitset").expect("Write failed");

// Read bitset from file
let loaded = Bitset::read_from_file("data.bitset").expect("Read failed");
assert_eq!(loaded.size(), bitset.size());
```

### Advanced Usage with Skip Bits
```rust
// Create bitset with shifted view
let mut shifted = Bitset::new(256, 100);
shifted.skip_bits = 16;  // Skip first 16 bits
shifted.set_bit(32);

// Serialization preserves skip_bits
let buffer = shifted.serialize().unwrap();
let restored = Bitset::deserialize(&buffer).unwrap();
assert_eq!(restored.skip_bits, 16);
assert_eq!(restored.size(), 240);  // 256 - 16
```

## Memory Layout

The serialized format matches the C++ layout:

```rust
#[repr(C, packed)]
struct BitsetSerialized {
    magic: u32,        // 0x42495453 ("BITS")
    version: u32,      // Format version (currently 1)
    id: u64,          // Bitset identifier
    nbits: u64,       // Total number of bits
    skip_bits: u64,   // Number of bits to skip at start
    word_count: u64,  // Number of 64-bit words following
}
// Followed immediately by word_count * 8 bytes of bit data
```

## Performance Characteristics

- **Zero-copy deserialization** when buffer alignment matches
- **4KB aligned buffers** for optimal I/O performance
- **Packed binary format** minimizes storage overhead
- **Direct memory mapping** support for large datasets

## Error Handling

All serialization operations return `Result` types with descriptive error messages:
- Buffer too small
- Invalid magic number
- Unsupported version
- File I/O errors
- Memory allocation failures

## Thread Safety

- **External synchronization model** - caller manages locking
- **Bitset** type for single-threaded use
- **AtomicBitset** type for atomic bit operations (serialization compatible)

Both types serialize to the same format and can be deserialized interchangeably.