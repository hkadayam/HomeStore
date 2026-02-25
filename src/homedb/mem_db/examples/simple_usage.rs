//! Simple usage example for MemDB
//!
//! This example demonstrates:
//! - Creating MemoryDB (IOManager initialized internally in async modes)
//! - Creating tables with different schemas
//! - Basic CRUD operations using table handles
//! - Creating and using secondary indices
//! - Range queries with iterators
//!
//! Run with sync:
//!   cargo run --example simple_usage --no-default-features --features sync_code
//!
//! Run with async:
//!   cargo run --example simple_usage --no-default-features --features async_code

use mem_db::{MemoryDB, TableSpec};

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
async fn run_example() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== MemDB Simple Usage Example ===\n");

    // Create a MemoryDB instance.
    // In async/sync_over_async modes, IOManager is initialized internally with 4 reactors.
    // In sync mode, the argument is ignored.
    println!("1. Creating MemoryDB...");
    let db = MemoryDB::new(4)?;
    println!("   MemoryDB created\n");

    // Create a users table with fixed-size keys and values
    println!("2. Creating 'users' table...");
    let user_spec = TableSpec::fixed_kv(8, 64); // 8-byte ID, 64-byte data
    let users_table = db.create_table("users", user_spec).await?;
    println!("   Table '{}' created\n", users_table.name());

    // Insert some user data using the table handle (no string lookup!)
    println!("3. Inserting user data...");
    for i in 0u64..10 {
        let user_id = i.to_le_bytes();
        let user_data = vec![i as u8; 64];
        users_table.put(user_id.to_vec(), user_data).await?;
    }
    println!("   Inserted 10 users\n");

    // Create a secondary index for email lookups
    println!("4. Creating secondary index for emails...");
    let email_spec = TableSpec::fixed_kv(32, 8); // 32-byte email hash -> 8-byte user ID
    let email_idx = users_table.create_index("email_idx", email_spec).await?;
    println!("   Email index created\n");

    // Add some email->user_id mappings
    println!("5. Adding email mappings to secondary index...");
    for i in 0u64..10 {
        let email_hash = {
            let mut hash = [0u8; 32];
            hash[0..8].copy_from_slice(&i.to_le_bytes());
            hash
        };
        let user_id = i.to_le_bytes();
        email_idx.put(email_hash.to_vec(), user_id.to_vec()).await?;
    }
    println!("   Added 10 email mappings\n");

    // Lookup via secondary index
    println!("6. Looking up user by email (via secondary index)...");
    let search_email_hash = {
        let mut hash = [0u8; 32];
        hash[0..8].copy_from_slice(&5u64.to_le_bytes());
        hash
    };

    if let Some(user_id_bytes) = email_idx.get(search_email_hash.to_vec()).await? {
        let user_id = u64::from_le_bytes(user_id_bytes[..8].try_into().unwrap());
        println!("   Found user ID: {}", user_id);

        // Now get the actual user data from primary index
        let primary = users_table.primary_index();
        if let Some(user_data) = primary.get(user_id_bytes).await? {
            println!("   User data length: {} bytes\n", user_data.len());
        }
    }

    // Read some data using the primary index
    println!("7. Reading user data (via primary index)...");
    let user_id = 5u64.to_le_bytes();
    if let Some(data) = users_table.get(user_id.to_vec()).await? {
        println!("   User 5 data length: {} bytes\n", data.len());
    }

    // Demonstrate convenience methods (with string lookup)
    println!("8. Using convenience methods (with table name lookup)...");
    let key = 7u64.to_le_bytes();
    let value = db.get("users", key.to_vec()).await?;
    println!("   User 7 data: {} bytes\n", value.unwrap().len());

    // Range query
    println!("9. Range query (users 3-7)...");
    let start_key = 3u64.to_le_bytes();
    let end_key = 8u64.to_le_bytes(); // exclusive
    let mut iter = db.get_range("users", start_key.to_vec(), end_key.to_vec(), 10).await?;

    let mut count = 0;
    while let Some((key, value)) = iter.next().await? {
        let id = u64::from_le_bytes(key[..8].try_into().unwrap());
        count += 1;
        println!("   User {}: {} bytes", id, value.len());
    }
    println!("   Total users in range: {}\n", count);

    // List all tables
    println!("10. Listing all tables...");
    let tables = db.list_tables();
    println!("   Tables: {:?}\n", tables);

    // List all indices on users table
    println!("11. Listing all indices on 'users' table...");
    let indices = users_table.list_indices();
    println!("   Indices: {:?}\n", indices);

    // Update a value
    println!("12. Updating user 5...");
    let user_id = 5u64.to_le_bytes();
    let new_data = vec![255u8; 64];
    users_table.put(user_id.to_vec(), new_data).await?;
    println!("   User 5 updated\n");

    // Delete a value
    println!("13. Deleting user 9...");
    let user_id = 9u64.to_le_bytes();
    if let Some(deleted_data) = users_table.remove(user_id.to_vec()).await? {
        println!("   Deleted user 9, data length: {} bytes\n", deleted_data.len());
    }

    // Verify deletion
    if db.get("users", user_id.to_vec()).await?.is_none() {
        println!("   Confirmed: user 9 no longer exists\n");
    }

    // MemoryDB Drop handles shutdown automatically — no explicit shutdown needed.
    println!("=== Example Complete ===");
    Ok(())
}

fn main() {
    cfg_if::cfg_if! {
        if #[cfg(feature = "async_frontend")] {
            tokio::runtime::Runtime::new().unwrap().block_on(run_example()).unwrap();
        } else if #[cfg(feature = "sync_frontend")] {
            run_example().unwrap();
        } else {
            eprintln!("No frontend feature enabled. Run with --features sync_code or --features async_code");
        }
    }
}
