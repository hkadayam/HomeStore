# Understanding Reactor Execution in MemoryDB

## The Problem You Identified

You asked an excellent question: **"When caller calls in a thread, how does it make sure put, get APIs run in one of the reactor? Won't it always run in same reactor?"**

The answer reveals a fundamental architectural issue.

## What Actually Happens

### `init_iomgr(4)` Creates Separate Reactor Threads

```rust
pub fn init_iomgr(4) {
    // Creates 4 SEPARATE OS threads, each with its own single-threaded tokio runtime
    for reactor_id in 0..4 {
        thread::spawn(|| {
            // Each reactor thread has its own tokio runtime
            let rt = tokio::runtime::Builder::new_current_thread()
                .build()
                .unwrap();
            
            // Block this thread, waiting for tasks via channel
            rt.block_on(async {
                loop {
                    // Wait for tasks to be sent via channel
                    let task = task_receiver.recv().await;
                    task.await;  // Execute the task
                }
            });
        });
    }
}
```

**Key points:**
- 4 separate OS threads created
- Each has its own isolated `current_thread` tokio runtime
- Each waits for tasks via an unbounded channel
- Tasks are distributed via `spawn_rr: AtomicU64` (round-robin counter)

### `#[tokio::main]` Creates a SEPARATE Runtime!

```rust
#[tokio::main]  // ← Creates its own multi-threaded tokio runtime
async fn main() {
    let db = MemoryDB::new(4)?;  // Initializes 4 reactor threads
    
    // This runs on tokio::main's runtime, NOT on reactor threads!
    db.put_one("users", &key, &value).await?;
}
```

**Result:** You have **TWO SEPARATE THREAD POOLS**:

1. **4 Reactor Threads** (from `init_iomgr`)
   - Each with single-threaded tokio runtime
   - Sitting idle, waiting for work
   - Can only execute work sent via channels

2. **N Tokio Worker Threads** (from `#[tokio::main]`)
   - Multi-threaded tokio runtime
   - This is where `.await` operations actually run
   - Does NOT communicate with reactor threads

## The Core Issue

### Current Behavior (BROKEN):

```rust
let db = MemoryDB::new(4)?;

// This does NOT run on any reactor thread!
// It runs on tokio::main's worker threads
db.put_one(...).await?;  // ← Reactors are IDLE
db.get(...).await?;      // ← Reactors are IDLE
```

**Problem:** The 4 reactor threads are doing nothing. All work happens on the main tokio runtime.

### Why This Matters:

- IOManager's reactor threads are designed for specific workloads (async I/O, lock-free operations)
- Just calling `.await` doesn't magically use reactors
- You must **explicitly spawn** work on reactors for them to be used

## Work Distribution Across Reactors

When you DO use reactors correctly, work is distributed via **round-robin**:

```rust
// IOManager has an atomic counter
spawn_rr: AtomicU64

// When spawning with ReactorTarget::Any
pub fn next_reactor(&self) -> ReactorId {
    (self.spawn_rr.fetch_add(1, Ordering::Relaxed) as usize) % self.num_reactors
}
```

**Each spawn picks the next reactor:**
- Call 1 → Reactor 0
- Call 2 → Reactor 1
- Call 3 → Reactor 2
- Call 4 → Reactor 3
- Call 5 → Reactor 0 (wraps around)

## Solutions

### Solution 1: Manual Spawning (Current Capability)

```rust
use iomgr::{ReactorTarget, spawn_waitable};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let db = MemoryDB::new(4)?;
    
    // Explicitly spawn each operation on a reactor
    let result = spawn_waitable(ReactorTarget::Any, async move {
        db.put_one("users", &key, &value).await
    }).await?;
    
    Ok(())
}
```

**Pros:**
- Uses reactors correctly
- Round-robin distribution

**Cons:**
- Verbose
- Easy to forget
- Clutters user code

### Solution 2: MemoryDB Spawns Internally (IMPLEMENTED)

```rust
pub async fn put_one(&self, table_name: &str, key: &[u8], value: &[u8]) -> Result<()> {
    let table = self.get_table(table_name)?;
    let key = key.to_vec();
    let value = value.to_vec();
    
    // Automatically spawn on reactor using round-robin
    iomgr::spawn_waitable(iomgr::ReactorTarget::Any, async move {
        table.put_one(&key, &value).await
    }).await
}
```

**Pros:**
- Transparent to user
- All operations automatically use reactors
- Round-robin distribution built-in

**Cons:**
- Small overhead for spawning
- Must clone data to send across threads

### Solution 3: Run Entire App in Reactor Context (Alternative)

Instead of `#[tokio::main]`, run your main function ON a reactor:

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize reactors
    let db = MemoryDB::new(4)?;
    
    // Run main logic on reactor 0
    iomgr::run_on(ReactorTarget::Reactor(0), async move {
        // Now all .await calls naturally run on reactor threads
        db.put_one("users", &key, &value).await?;
        db.get("users", &key).await?;
        Ok(())
    })?;
    
    Ok(())
}
```

**Pros:**
- No separate main runtime
- Natural execution model
- No spawning overhead

**Cons:**
- Less flexible
- All work pinned to one reactor initially (though you can still spawn to others)

## Verification: Are Reactors Being Used?

Add logging to verify:

```rust
#[tokio::main]
async fn main() {
    let db = MemoryDB::new(4)?;
    
    for i in 0..10 {
        db.put_one("users", &i.to_le_bytes(), &vec![i as u8; 16]).await?;
        
        // Check which reactor we're on
        if let Some(reactor) = iomgr::iomgr().current_reactor() {
            println!("Put {} executed on reactor {}", i, reactor.id());
        } else {
            println!("Put {} executed on NON-reactor thread!", i);
        }
    }
}
```

**With Solution 1/2:** You'll see round-robin distribution (0, 1, 2, 3, 0, 1, ...)
**Without spawning:** You'll see "NON-reactor thread" every time!

## The Correct Solution: Run App in Reactor Context

The **IOManager design constraint** is that you must already be on a reactor to spawn work. This means:

❌ **WRONG:** Using `#[tokio::main]` with separate reactor threads
```rust
#[tokio::main]  // ← Creates separate runtime, can't spawn on reactors!
async fn main() {
    let db = MemoryDB::new(4)?;
    db.put_one(...).await?;  // Runs on tokio::main, NOT reactors
}
```

✅ **CORRECT:** Run your app ON a reactor
```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let db = MemoryDB::new(4)?;
    
    // Run your app logic on reactor 0
    iomgr::run_on(iomgr::ReactorTarget::Reactor(0), async move {
        // Now we're ON a reactor, all operations run in reactor context
        db.put_one("users", &key, &value).await?;
        
        // Can spawn to other reactors from here
        iomgr::spawn_detached(iomgr::ReactorTarget::Reactor(1), async {
            // This runs on reactor 1
        });
        
        Ok::<(), Box<dyn std::error::Error>>(())
    })?;
    
    db.shutdown().await;
    Ok(())
}
```

### For Multi-Operation Workloads: Use BackgroundTasks

To distribute work across all reactors:

```rust
use iomgr::{BackgroundTasks, ReactorTarget};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let db = MemoryDB::new(4)?;
    
    iomgr::run_on(ReactorTarget::Reactor(0), async move {
        let bg_tasks = BackgroundTasks::new();
        
        // Distribute 1000 operations across 4 reactors
        for i in 0..1000 {
            let db = db.clone();  // Clone Arc
            let reactor_id = i % 4;
            
            bg_tasks.spawn(ReactorTarget::Reactor(reactor_id), async move {
                db.put_one("users", &i.to_le_bytes(), &vec![i as u8; 16]).await?;
                Ok::<(), MemDbError>(())
            });
        }
        
        // Wait for all operations to complete
        bg_tasks.join_all().await;
        
        Ok::<(), Box<dyn std::error::Error>>(())
    })?;
    
    Ok(())
}
```

This achieves true parallel execution across all reactors!

## Performance Implications

### Without Reactor Spawning:
- Operations run on main tokio runtime
- No work distribution across reactors
- Reactors idle (wasted resources)

### With Reactor Spawning (Round-Robin):
- Operations distributed across all reactors
- Better CPU utilization
- True parallelism for concurrent operations
- Optimal for async I/O workloads

### Overhead:
- Spawning has ~1-5 microsecond overhead
- Negligible compared to actual B-tree operations (microseconds to milliseconds)
- The parallelism gains far outweigh spawn cost

## Summary

Your question was spot-on! The current design has a critical flaw:

❌ **Before:** Operations run on `#[tokio::main]` runtime, reactors are idle
✅ **After:** Operations spawn on reactors via round-robin, full parallelism

The fix is to make MemoryDB internally spawn all operations on reactors using `ReactorTarget::Any`, which provides automatic round-robin distribution.
