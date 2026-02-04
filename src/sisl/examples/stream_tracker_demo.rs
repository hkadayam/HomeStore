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
    // Check status
    let status1 = tracker.status(1).await;
    println!("Entry 1 status: active={}, completed={}", status1.is_active, status1.is_completed);

    // Complete some entries
    println!("2. Completing entries 1 and 2...");
    tracker.complete(1, 2).await;

    let status1_after = tracker.status(1).await;
    println!(
        "Entry 1 status after completion: active={}, completed={}",
        status1_after.is_active, status1_after.is_completed
    );

    // Find completed upto
    let completed_upto = tracker.completed_upto(0).await;
    println!("Completed upto: {}", completed_upto);

    // Iterate over completed entries
    println!("3. Iterating over completed entries:");
    tracker
        .foreach_all_completed(1, |idx, data| {
            println!("   Entry {}: {}", idx, data);
            true
        })
        .await;

    // Truncate
    println!("4. Truncating upto index 2...");
    let truncated = tracker.truncate_to(2).await;
    println!("Truncated upto: {}", truncated);

    // Check status after truncation
    let status1_final = tracker.status(1).await;
    println!("Entry 1 status after truncation: out_of_range={}", status1_final.is_out_of_range);

    // Entry 3 should still be accessible
    match tracker.at(3).await {
        Ok(data) => println!("Entry 3 is still accessible: {}", data),
        Err(e) => println!("Error accessing entry 3: {}", e),
    }

    // Demo auto-truncating stream tracker
    println!("\n=== Auto-Truncating Stream Tracker Demo ===");
    let auto_tracker = StreamTrackerAutoTruncate::<i32>::new("auto_tracker", 10);

    // Create and complete entries - should auto-truncate after threshold
    for i in 11..20 {
        auto_tracker.create_and_complete(i, (i * 10) as i32).await;
    }

    println!("Auto-tracker status:");
    let status = auto_tracker.get_status(2).await;
    println!("{}", serde_json::to_string_pretty(&status).unwrap());

    println!("\n=== Stream Tracker Benefits ===");
    println!("• Ordered stream processing with completion tracking");
    println!("• Efficient truncation with lazy compaction");
    println!("• Thread-safe operations with AsyncRwLock protection");
    println!("• Automatic truncation support for high-throughput scenarios");
    println!("• Memory-efficient bit tracking for large streams");
}
