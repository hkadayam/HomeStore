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
    println!("   Capacity: {}, Reserved count: {}", reserver.capacity().await, reserver.reserved_count().await);

    println!("\n2. Reserving specific IDs...");
    reserver.reserve_specific(10).await.unwrap();
    reserver.reserve_specific(15).await.unwrap();

    println!("   Reserved specific IDs: 10, 15");
    println!("   Reserved count: {}", reserver.reserved_count().await);

    println!("\n3. Checking reservation status...");
    for id in [1, 5, 10, 15, 20] {
        println!("   ID {}: {}", id, if reserver.is_reserved(id).await { "Reserved" } else { "Available" });
    }

    println!("\n4. Unreserving some IDs...");
    reserver.unreserve(id2).await.unwrap();
    reserver.unreserve(10).await.unwrap();

    println!("   Unreserved IDs: {}, 10", id2);
    println!("   Reserved count: {}", reserver.reserved_count().await);

    println!("\n5. Reserving again (should reuse unreserved IDs)...");
    let id4 = reserver.reserve().await.unwrap();
    let id5 = reserver.reserve().await.unwrap();

    println!("   New reserved IDs: {}, {}", id4, id5);

    println!("\n6. Iterating over all reserved IDs...");
    let mut reserved_ids = Vec::new();
    let mut cur = reserver.first_reserved_id().await;
    while let Some(id) = cur {
        reserved_ids.push(id);
        cur = reserver.next_reserved_id(id).await;
    }
    println!("   All reserved IDs: {:?}", reserved_ids);

    println!("\n7. Testing expansion by reserving many IDs...");
    let mut expansion_ids = Vec::new();
    for _ in 0..20 {
        if let Ok(id) = reserver.reserve().await {
            expansion_ids.push(id);
        }
    }

    println!("   Reserved {} additional IDs", expansion_ids.len());
    println!("   New capacity: {}, Total reserved: {}", reserver.capacity().await, reserver.reserved_count().await);

    println!("\n8. Testing serialization...");
    let serialized = reserver.serialize().await;
    println!("   Serialized to {} bytes", serialized.len());

    // Create a new reserver from serialized data
    let reserver2 = IdReserver::from_bytes(&serialized).unwrap();
    println!(
        "   Deserialized reserver capacity: {}, reserved count: {}",
        reserver2.capacity().await,
        reserver2.reserved_count().await
    );

    // Verify state is preserved
    let mut original_reserved = Vec::new();
    let mut cur = reserver.first_reserved_id().await;
    while let Some(id) = cur {
        original_reserved.push(id);
        cur = reserver.next_reserved_id(id).await;
    }
    let mut deserialized_reserved = Vec::new();
    let mut cur2 = reserver2.first_reserved_id().await;
    while let Some(id) = cur2 {
        deserialized_reserved.push(id);
        cur2 = reserver2.next_reserved_id(id).await;
    }
    println!("   State preserved: {}", original_reserved == deserialized_reserved);

    println!("\n=== ID Reserver Benefits ===");
    println!("• Thread-safe ID allocation and management");
    println!("• Automatic expansion when capacity is exceeded");
    println!("• Efficient reuse of unreserved IDs");
    println!("• Serialization support for persistence");
    println!("• Simple traversal via first/next methods (iterator removed in async version)");
    println!("• Memory-efficient bitset-based storage");
}
