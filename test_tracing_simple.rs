// Simple test to demonstrate tracing
#[cfg(test)]
mod tests {
    use homestore::index::btree::{Btree, BtreeConfig};
    use homestore::index::btree::underlying::mem::MemBtree;
    use homestore::index::btree::tests::btree_test_kvs::{FixedSizeTestKey, FixedSizeTestValue};

    #[tokio::test]
    async fn test_btree_with_tracing() {
        // Initialize tracing
        let _ = tracing_subscriber::fmt()
            .with_env_filter(
                tracing_subscriber::EnvFilter::from_default_env()
                    .add_directive("homestore::index::btree=debug".parse().unwrap())
            )
            .with_target(true)
            .try_init();

        println!("\n=== Starting Btree Tracing Test ===\n");

        // Create btree
        let config = BtreeConfig::new(4096, "test_btree".to_string());
        let storage = Box::new(MemBtree::new(4096));
        let btree: Btree<FixedSizeTestKey, FixedSizeTestValue> = 
            Btree::new(config, storage, None).await.unwrap();

        // Insert some keys - watch for op_id=1, 2, 3
        println!("Inserting keys...");
        btree.put_one(&FixedSizeTestKey::from_u64(10), &FixedSizeTestValue::from_u64(100), None).await.unwrap();
        btree.put_one(&FixedSizeTestKey::from_u64(20), &FixedSizeTestValue::from_u64(200), None).await.unwrap();
        btree.put_one(&FixedSizeTestKey::from_u64(30), &FixedSizeTestValue::from_u64(300), None).await.unwrap();

        // Get keys - watch for op_id=4, 5
        println!("\nGetting keys...");
        let val = btree.get(&FixedSizeTestKey::from_u64(20)).await.unwrap();
        assert_eq!(val, Some(FixedSizeTestValue::from_u64(200)));
        
        let val = btree.get(&FixedSizeTestKey::from_u64(99)).await.unwrap();
        assert_eq!(val, None);

        // Remove key - watch for op_id=6
        println!("\nRemoving key...");
        let val = btree.remove_one(&FixedSizeTestKey::from_u64(20)).await.unwrap();
        assert_eq!(val, Some(FixedSizeTestValue::from_u64(200)));

        // Verify removal - watch for op_id=7
        println!("\nVerifying removal...");
        let val = btree.get(&FixedSizeTestKey::from_u64(20)).await.unwrap();
        assert_eq!(val, None);

        println!("\n=== Test Complete ===\n");
    }
}
