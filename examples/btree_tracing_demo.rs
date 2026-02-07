/***************************************************************************
 * Btree Tracing Demo - Shows actual tracing output format
 *
 * Run with different log levels:
 * RUST_LOG=info cargo run --example btree_tracing_demo
 * RUST_LOG=debug cargo run --example btree_tracing_demo  
 * RUST_LOG=trace cargo run --example btree_tracing_demo
 ***************************************************************************/

use std::sync::atomic::{AtomicU64, Ordering};

// Global operation counter (simulating the one in btree.rs)
static GLOBAL_OP_COUNTER: AtomicU64 = AtomicU64::new(0);

fn main() {
    // Initialize tracing
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive("btree_tracing_demo=trace".parse().unwrap())
        )
        .with_target(false)
        .init();

    println!("\n=== Btree Tracing Demo ===");
    println!("Simulating Homestore btree operations with tracing\n");

    // Simulate operations on different btrees
    simulate_put_one("users_index", 42);
    simulate_put_one("users_index", 100);
    simulate_get("users_index", 42);
    simulate_put_range("metadata_index", 10, 20);
    simulate_remove_one("users_index", 42);
    simulate_get_any("metadata_index", 5, 15);
    simulate_query("users_index", 0, 100);
    
    println!("\n=== Demo Complete ===");
    println!("\nKey observations:");
    println!("✓ Sequential op_id across ALL btrees (global counter)");
    println!("✓ Btree name in every span");
    println!("✓ Keys shown in concise format");
    println!("✓ Automatic timestamps");
    println!("✓ Hierarchical context (span{...}: message)");
    println!("\nTry different RUST_LOG levels:");
    println!("  RUST_LOG=info  - See only INFO logs");
    println!("  RUST_LOG=debug - See DEBUG + INFO");
    println!("  RUST_LOG=trace - See everything");
}

#[tracing::instrument(
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = btree_name,
        key = key
    )
)]
fn simulate_put_one(btree_name: &str, key: u64) {
    tracing::debug!("Starting put operation");
    // Simulate work
    std::thread::sleep(std::time::Duration::from_millis(10));
    tracing::info!("Put completed");
}

#[tracing::instrument(
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = btree_name,
        key = key
    )
)]
fn simulate_get(btree_name: &str, key: u64) {
    tracing::debug!("Starting get operation");
    std::thread::sleep(std::time::Duration::from_millis(5));
    tracing::debug!("Key found");
}

#[tracing::instrument(
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = btree_name,
        start = start,
        end = end
    )
)]
fn simulate_put_range(btree_name: &str, start: u64, end: u64) {
    tracing::debug!("Starting range put");
    std::thread::sleep(std::time::Duration::from_millis(15));
    tracing::info!("Range put completed");
}

#[tracing::instrument(
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = btree_name,
        key = key
    )
)]
fn simulate_remove_one(btree_name: &str, key: u64) {
    tracing::debug!("Starting remove operation");
    std::thread::sleep(std::time::Duration::from_millis(8));
    tracing::info!("Key removed successfully");
}

#[tracing::instrument(
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = btree_name,
        start = start,
        end = end
    )
)]
fn simulate_get_any(btree_name: &str, start: u64, end: u64) {
    tracing::debug!("Starting get_any");
    std::thread::sleep(std::time::Duration::from_millis(6));
    tracing::debug!(found_key = 12, "Found key in range");
}

#[tracing::instrument(
    fields(
        op_id = GLOBAL_OP_COUNTER.fetch_add(1, Ordering::Relaxed),
        btree = btree_name,
        start = start,
        end = end
    )
)]
fn simulate_query(btree_name: &str, start: u64, end: u64) {
    tracing::debug!("Starting query");
    std::thread::sleep(std::time::Duration::from_millis(20));
    tracing::debug!(result_count = 25, has_more = false, "Query completed");
}
