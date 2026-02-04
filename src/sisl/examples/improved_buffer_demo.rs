/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

use std::error::Error;

use aligned_vec::{AVec, RuntimeAlign};
use sisl::BitsetSerialized;
use bytes::Bytes;
use zerocopy::{IntoBytes, Ref};

fn main() -> Result<(), Box<dyn Error>> {
    println!("=== Improved Buffer Implementation Demo ===\n");

    // ===== 1. Aligned Vector Demo =====
    println!("1. Using aligned-vec for safe aligned allocation:");

    // Create a 4KB-aligned vector using aligned-vec
    let mut aligned_data = AVec::<u8, RuntimeAlign>::new(4096);
    aligned_data.resize(8192, 0); // 8KB of data, 4KB aligned

    println!("   ✅ Created 8KB buffer with 4KB alignment");
    println!("   ✅ Pointer alignment: {} bytes", aligned_data.as_ptr() as usize % 4096);
    println!("   ✅ Length: {} bytes", aligned_data.len());

    // Modify the data safely
    aligned_data[0] = 0x42;
    aligned_data[4095] = 0xFF;
    aligned_data[8191] = 0xAA;

    println!("   ✅ Data written safely without raw pointers\n");

    // ===== 2. Zero-Copy Serialization Demo =====
    println!("2. Using zerocopy for safe buffer operations:");

    // Create a BitsetSerialized structure
    let original_header = BitsetSerialized {
        magic: BitsetSerialized::MAGIC,
        version: BitsetSerialized::VERSION,
        id: 12345,
        nbits: 1024,
        skip_bits: 0,
        word_count: 16,
        set_count: 0,
    };

    // Convert to bytes using zerocopy (safe transmutation)
    let header_bytes = original_header.as_bytes();
    println!("   ✅ Serialized BitsetSerialized to {} bytes", header_bytes.len());

    // Create a buffer with the header
    let mut buffer_data = Vec::with_capacity(1024);
    buffer_data.extend_from_slice(header_bytes);
    buffer_data.resize(1024, 0); // Add space for bitset words

    // Zero-copy deserialization using zerocopy::Ref
    let header_ref = Ref::<_, BitsetSerialized>::from_bytes(&buffer_data[..std::mem::size_of::<BitsetSerialized>()])
        .map_err(|e| format!("Failed to create zerocopy ref: {:?}", e))?;

    println!("   ✅ Zero-copy deserialized header:");

    // Copy values from packed struct to avoid alignment issues
    let magic = header_ref.magic;
    let version = header_ref.version;
    let id = header_ref.id;
    let nbits = header_ref.nbits;
    let word_count = header_ref.word_count;

    println!("      Magic: 0x{:08X}", magic);
    println!("      Version: {}", version);
    println!("      ID: {}", id);
    println!("      Bits: {}", nbits);
    println!("      Word count: {}", word_count);

    // Verify magic number safely
    if magic == BitsetSerialized::MAGIC {
        println!("   ✅ Magic number validation passed\n");
    }

    // ===== 3. Comparison with Current Implementation =====
    println!("3. Comparison with aligned-vec:");

    // aligned-vec approach - safe and efficient
    println!("   Improved: aligned-vec with safe Rust Vec-like API");
    println!("   Benefits: ✅ No unsafe code ✅ Drop-in Vec replacement ✅ Serde support\n");

    // ===== 4. Buffer View Demo (using bytes crate) =====
    println!("4. BufferView with bytes crate:");

    let data = b"Hello, World! This is zero-copy buffer view demonstration.";
    let buffer_view = Bytes::from_static(data);

    // Zero-copy slicing
    let hello = buffer_view.slice(0..5);
    let world = buffer_view.slice(7..12);

    println!("   ✅ Original: {}", String::from_utf8_lossy(&buffer_view));
    println!("   ✅ Slice 1: {}", String::from_utf8_lossy(&hello));
    println!("   ✅ Slice 2: {}", String::from_utf8_lossy(&world));
    println!("   ✅ Zero-copy operations with reference counting\n");

    // ===== 5. Performance Benefits Summary =====
    println!("=== Benefits Summary ===");
    println!("aligned-vec:");
    println!("  ✅ Safe aligned allocation without unsafe code");
    println!("  ✅ Drop-in replacement for Vec<T>");
    println!("  ✅ Runtime and compile-time alignment");
    println!("  ✅ Serde serialization support");
    println!("  ✅ No custom allocator maintenance");

    println!("\nzerocopy:");
    println!("  ✅ Compile-time safety for transmutations");
    println!("  ✅ Zero-copy serialization/deserialization");
    println!("  ✅ Safe buffer reinterpretation");
    println!("  ✅ Google-maintained, security-audited");
    println!("  ✅ Industry standard for zero-copy operations");

    println!("\nbytes (current):");
    println!("  ✅ Already optimal for buffer views");
    println!("  ✅ Reference counting for efficient sharing");
    println!("  ✅ Zero-copy slicing operations");
    println!("  ✅ Used by Tokio/Hyper (battle-tested)");

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_aligned_vec_integration() -> Result<(), Box<dyn Error>> {
        let mut data = AVec::<u8, RuntimeAlign>::new(512);
        data.resize(1024, 42);

        assert_eq!(data.len(), 1024);
        assert_eq!(data[0], 42);
        assert_eq!(data[1023], 42);
        assert_eq!(data.as_ptr() as usize % 512, 0); // Check alignment

        Ok(())
    }

    #[test]
    fn test_zerocopy_bitset_serialization() {
        let header = BitsetSerialized {
            magic: BitsetSerialized::MAGIC,
            version: BitsetSerialized::VERSION,
            id: 999,
            nbits: 256,
            skip_bits: 0,
            word_count: 4,
            set_count: 0,
        };

        // Safe serialization
        let bytes = header.as_bytes();
        assert_eq!(bytes.len(), std::mem::size_of::<BitsetSerialized>());

        // Safe deserialization
        if let Ok(deserialized) = Ref::<_, BitsetSerialized>::from_bytes(bytes) {
            // Copy values from packed struct
            let magic = deserialized.magic;
            let id = deserialized.id;
            let nbits = deserialized.nbits;
            let word_count = deserialized.word_count;

            assert_eq!(magic, BitsetSerialized::MAGIC);
            assert_eq!(id, 999);
            assert_eq!(nbits, 256);
            assert_eq!(word_count, 4);
        } else {
            panic!("Failed to deserialize BitsetSerialized");
        }
    }

    #[test]
    fn test_zero_copy_buffer_views() {
        let data = b"The quick brown fox jumps over the lazy dog";
        let view = Bytes::from_static(data);

        // Zero-copy slicing
        let quick = view.slice(4..9);
        let brown = view.slice(10..15);
        let fox = view.slice(16..19);

        assert_eq!(&quick[..], b"quick");
        assert_eq!(&brown[..], b"brown");
        assert_eq!(&fox[..], b"fox");

        // All views should be zero-copy (same underlying data)
        let original_ptr = view.as_ptr() as usize;
        let quick_ptr = quick.as_ptr() as usize;

        // Verify they point to same memory region
        assert_eq!(quick_ptr, original_ptr + 4);
    }
}
