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
    // Set some bits
    bitset.set_bit(10);
    bitset.set_bit(100);
    bitset.set_bit(200);
    bitset.set_bit(255);

    println!("2. Set bits at positions: 10, 100, 200, 255");

    // Verify bits are set
    println!("3. Checking bits:");
    println!("   Bit 10: {}", bitset.get_bitval(10));
    println!("   Bit 100: {}", bitset.get_bitval(100));
    println!("   Bit 200: {}", bitset.get_bitval(200));
    println!("   Bit 255: {}", bitset.get_bitval(255));
    println!("   Bit 50 (should be false): {}", bitset.get_bitval(50));

    // Write to file - this IS the serialization (zero-copy!)
    let file_path = "/tmp/zero_copy_demo.bitset";
    bitset.write_to_file(file_path)?;
    println!("4. Wrote bitset to file: {}", file_path);

    // Read from file - zero-copy deserialization
    let loaded_bitset = Bitset::read_from_file(file_path)?;
    println!("5. Loaded bitset from file");

    // Verify all data is preserved
    println!("6. Verifying loaded bitset:");
    println!("   ID: {} (original: {})", loaded_bitset.get_id(), bitset.get_id());
    println!("   Size: {} (original: {})", loaded_bitset.size(), bitset.size());
    println!("   Bit 10: {}", loaded_bitset.get_bitval(10));
    println!("   Bit 100: {}", loaded_bitset.get_bitval(100));
    println!("   Bit 200: {}", loaded_bitset.get_bitval(200));
    println!("   Bit 255: {}", loaded_bitset.get_bitval(255));
    println!("   Bit 50 (should be false): {}", loaded_bitset.get_bitval(50));

    // Demonstrate buffer access - this IS the serialized data
    let buffer = bitset.buffer();
    println!("7. Buffer info:");
    println!("   Size: {} bytes", buffer.len());
    println!("   Alignment: {} bytes", buffer.alignment());

    // Show that we can create another bitset from the same data
    let copied_bitset = {
        // Get the data and create new IOBuffer from it
        use iomgr::IOBuffer;
        let data_slice = buffer.to_vec();
        let mut new_buf = IOBuffer::new(data_slice.len());
        new_buf.as_mut_slice().copy_from_slice(&data_slice);
        Bitset::load(new_buf)?.0
    };
    println!("8. Created bitset from buffer copy - all data matches:");
    println!("   ID: {}", copied_bitset.get_id());
    println!("   Bit 100: {}", copied_bitset.get_bitval(100));

    // Clean up
    std::fs::remove_file(file_path)?;

    println!("\n=== Key Benefits ===");
    println!("• Zero-copy serialization: buffer IS the serialized data");
    println!("• Direct I/O: no intermediate copying for file operations");
    println!("• Memory efficient: single aligned buffer contains everything");
    println!("• Compatible with C++: uses same binary format");
    println!("• Direct memory mapping support for large datasets");

    Ok(())
}
