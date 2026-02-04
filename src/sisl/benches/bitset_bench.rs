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

use std::time::Instant;

use rand::Rng;
use sisl::bitset::Bitset;

fn main() {
    // Parse command-line arguments
    let args: Vec<String> = std::env::args().collect();

    let total_bits = if args.len() > 1 { args[1].parse::<u64>().unwrap_or(2_000_000_000) } else { 2_000_000_000 };

    let fill_percentage = if args.len() > 2 { args[2].parse::<f64>().unwrap_or(0.25) } else { 0.25 };

    let num_queries = if args.len() > 3 { args[3].parse::<usize>().unwrap_or(1_000_000) } else { 1_000_000 };

    let min_contiguous_size = if args.len() > 4 { args[4].parse::<u32>().unwrap_or(1) } else { 1 };

    let max_contiguous_size = if args.len() > 5 { args[5].parse::<u32>().unwrap_or(64) } else { 64 };

    println!("=== Bitset Benchmark ===");
    println!("Total bits: {}", total_bits);
    println!("Fill percentage: {}%", fill_percentage * 100.0);
    println!("Number of queries: {}", num_queries);
    println!("Contiguous size range: {}-{}", min_contiguous_size, max_contiguous_size);
    println!();
    println!(
        "Usage: {} <total_bits> <fill_percentage> <num_queries> <min_contiguous_size> <max_contiguous_size>",
        args[0]
    );
    println!("  Defaults: 2000000000 0.25 1000000 1 64");
    println!();

    // Step 1: Create bitset
    println!("Step 1: Creating bitset with {} million bits...", total_bits / 1_000_000);
    let start = Instant::now();
    let mut bitset = Bitset::new(total_bits, 1);
    let creation_time = start.elapsed();
    println!("  Created in {:.2?}", creation_time);
    println!("  Memory size: {:.2} MB", bitset.serialized_size() as f64 / (1024.0 * 1024.0));
    println!();

    // Step 2: Randomly fill bits
    println!("Step 2: Randomly filling ~{}% of bits...", fill_percentage * 100.0);
    let start = Instant::now();
    let mut rng = rand::thread_rng();

    let mut set_count = 0u64;

    // Use a sampling approach to avoid too much overhead
    // We'll iterate through bits and randomly set them with the target probability
    for bit in 0..total_bits {
        if rng.gen::<f64>() < fill_percentage {
            bitset.set_bit(bit);
            set_count += 1;
        }
    }

    let fill_time = start.elapsed();
    println!("  Filled {} bits in {:.2?}", set_count, fill_time);
    println!("  Actual fill rate: {:.2}%", (set_count as f64 / total_bits as f64) * 100.0);
    println!();

    // Step 3: Benchmark get_next_contiguous_n_reset_bits_range with random sizes
    println!(
        "Step 3: Running {} queries for contiguous reset bits (size range {}-{})...",
        num_queries, min_contiguous_size, max_contiguous_size
    );

    let mut query_min_sizes = Vec::with_capacity(num_queries);
    let mut query_max_sizes = Vec::with_capacity(num_queries);
    for _ in 0..num_queries {
        // Generate random min and max within the specified range
        let min = rng.gen_range(min_contiguous_size..=max_contiguous_size);
        let max = rng.gen_range(min..=max_contiguous_size);
        query_min_sizes.push(min);
        query_max_sizes.push(max);
    }

    println!("  Query distribution (min-max ranges):");
    for size in min_contiguous_size..=std::cmp::min(min_contiguous_size + 9, max_contiguous_size) {
        let count = query_min_sizes.iter().filter(|&&s| s == size).count();
        println!("    Min size {}: {} queries", size, count);
    }
    if max_contiguous_size > min_contiguous_size + 9 {
        println!(
            "    Min size {}-{}: {} queries",
            min_contiguous_size + 10,
            max_contiguous_size,
            query_min_sizes.iter().filter(|&&s| s > min_contiguous_size + 9).count()
        );
    }
    println!();

    // Run the benchmark
    let mut found_count = 0;
    let mut total_bits_found = 0u64;
    let mut start_positions = Vec::with_capacity(num_queries);

    // Generate random starting positions
    for _ in 0..num_queries {
        start_positions.push(rng.gen_range(0..total_bits));
    }

    let start = Instant::now();
    for idx in 0..num_queries {
        let start_bit = start_positions[idx];
        let min_needed = query_min_sizes[idx];
        let max_needed = query_max_sizes[idx];
        let result = bitset.get_next_contiguous_n_reset_bits_range(start_bit, None, min_needed, max_needed);

        if result.start_bit != sisl::bitset::NPOS {
            found_count += 1;
            total_bits_found += result.nbits as u64;
        }
    }

    let query_time = start.elapsed();

    println!();
    println!("=== Benchmark Results ===");
    println!("Total query time: {:.2?}", query_time);
    println!("Average time per query: {:.2?}", query_time / num_queries as u32);
    println!("Queries per second: {:.2}", num_queries as f64 / query_time.as_secs_f64());
    println!("Successful queries: {} ({:.2}%)", found_count, (found_count as f64 / num_queries as f64) * 100.0);
    println!(
        "Average bits found per successful query: {:.2}",
        if found_count > 0 { total_bits_found as f64 / found_count as f64 } else { 0.0 }
    );

    // Additional statistics
    println!();
    println!("=== Additional Statistics ===");

    // Sample a few queries to show results
    println!("Sample query results (first 10 successful):");
    let mut sample_count = 0;
    for idx in 0..std::cmp::min(100, num_queries) {
        let start_bit = start_positions[idx];
        let min_needed = query_min_sizes[idx];
        let max_needed = query_max_sizes[idx];
        let result = bitset.get_next_contiguous_n_reset_bits_range(start_bit, None, min_needed, max_needed);

        if result.start_bit != sisl::bitset::NPOS {
            println!(
                "  Query {}: min={}, max={}, start={}, found_at={}, nbits={}",
                idx, min_needed, max_needed, start_bit, result.start_bit, result.nbits
            );
            sample_count += 1;
            if sample_count >= 10 {
                break;
            }
        }
    }

    println!();
    println!("Benchmark complete!");
}
