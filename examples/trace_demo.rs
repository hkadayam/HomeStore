/***************************************************************************
 * Simple demonstration of tracing output
 * 
 * Run with:
 * RUST_LOG=trace cargo run --example trace_demo --features btree-only
 ***************************************************************************/

// This is a minimal example to show tracing output without full compilation
fn main() {
    // Initialize tracing
    use tracing_subscriber::EnvFilter;
    
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::from_default_env()
                .add_directive("homestore=trace".parse().unwrap())
        )
        .with_target(true)
        .init();
    
    println!("\n=== Tracing Demo ===\n");
    println!("Demonstrating tracing output format:");
    println!();
    
    // Simulate what our btree tracing would look like
    println!("Expected output format:");
    println!("  2026-02-05T10:23:45.123Z DEBUG put_one{{op_id=1, btree=\"users_index\", key=K(42)}}: Starting put operation");
    println!("  2026-02-05T10:23:45.234Z  INFO put_one{{op_id=1, btree=\"users_index\", key=K(42)}}: Put completed");
    println!();
    println!("  2026-02-05T10:23:45.345Z DEBUG remove_one{{op_id=2, btree=\"metadata_index\", key=K(42)}}: Starting remove operation");
    println!("  2026-02-05T10:23:45.456Z  INFO remove_one{{op_id=2, btree=\"metadata_index\", key=K(42)}}: Key removed successfully");
    println!();
    
    println!("Key features:");
    println!("  ✓ Global sequential op_id across all btrees (1, 2, 3...)");
    println!("  ✓ Btree name in every span (btree=\"users_index\")");
    println!("  ✓ Keys shown in concise format K(42) or verbose FixedSizeTestKey {{ value: 42, id: 1 }}");
    println!("  ✓ Automatic timestamps");
    println!("  ✓ Hierarchical spans");
    println!();
    
    println!("Filter examples:");
    println!("  RUST_LOG=homestore::index::btree=info        # Info level");
    println!("  RUST_LOG=homestore::index::btree=debug       # Debug level");
    println!("  RUST_LOG=homestore::index::btree=trace       # Trace level");
    println!("  RUST_LOG=homestore::index::btree[put_one]=trace  # Only put_one operations");
    println!();
    
    // Actual tracing example
    demo_tracing();
}

#[tracing::instrument(fields(op_id=1, btree="demo_btree", key="K(42)"))]
fn demo_tracing() {
    tracing::debug!("This is what a debug log looks like");
    tracing::info!("This is what an info log looks like");
    tracing::trace!("This is what a trace log looks like (only with RUST_LOG=trace)");
}
