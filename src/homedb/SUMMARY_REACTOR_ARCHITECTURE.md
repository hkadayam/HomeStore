# Summary: Reactor Architecture and Execution Model

## Your Question Was Perfect!

You asked: **"When caller calls in a thread, how does it make sure put, get APIs run in one of the reactor? Won't it always run in same reactor?"**

This revealed a fundamental architectural misunderstanding in the initial design.

## The Truth: Two Separate Thread Pools

### What `init_iomgr(4)` Actually Does:

```
Creates 4 SEPARATE OS threads:
┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│ Reactor 0   │  │ Reactor 1   │  │ Reactor 2   │  │ Reactor 3   │
│ (Thread)    │  │ (Thread)    │  │ (Thread)    │  │ (Thread)    │
│             │  │             │  │             │  │             │
│ Tokio RT    │  │ Tokio RT    │  │ Tokio RT    │  │ Tokio RT    │
│ (single)    │  │ (single)    │  │ (single)    │  │ (single)    │
│             │  │             │  │             │  │             │
│ [WAITING]   │  │ [WAITING]   │  │ [WAITING]   │  │ [WAITING]   │
│ for tasks   │  │ for tasks   │  │ for tasks   │  │ for tasks   │
│ via channel │  │ via channel │  │ via channel │  │ via channel │
└─────────────┘  └─────────────┘  └─────────────┘  └─────────────┘
```

Each reactor thread:
- Has its own `tokio::runtime::Builder::new_current_thread()` runtime
- Waits for tasks sent via unbounded channel
- Executes tasks sequentially on that single thread

### What `#[tokio::main]` Does:

```
Creates a SEPARATE multi-threaded tokio runtime:
┌──────────────────────────────────────────────────────┐
│  Tokio Main Runtime (Multi-threaded)                 │
│                                                       │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐       │
│  │Worker 1│ │Worker 2│ │Worker 3│ │Worker 4│  ...  │
│  └────────┘ └────────┘ └────────┘ └────────┘       │
│                                                       │
│  This is where db.put_one().await runs!             │
└──────────────────────────────────────────────────────┘
```

**Result:** You have TWO completely separate thread pools that don't communicate!

## The Problem

```rust
#[tokio::main]
async fn main() {
    let db = MemoryDB::new(4)?;  // Creates 4 reactor threads (idle)
    
    // This runs on tokio::main's worker threads
    // Reactor threads are doing NOTHING
    db.put_one("users", &key, &value).await?;  
}
```

Answer to your questions:
1. ❌ Operations do NOT run on reactor threads
2. ❌ They run on `tokio::main` worker threads
3. ✅ Reactor threads sit idle doing nothing

## Work Distribution (When Used Correctly)

When you DO spawn on reactors, distribution is round-robin:

```rust
// IOManager tracks next reactor atomically
spawn_rr: AtomicU64 = 0;

// Each spawn increments and wraps around
fn next_reactor() -> ReactorId {
    (self.spawn_rr.fetch_add(1, Ordering::Relaxed) % 4)
}

// Spawn 1 → Reactor 0
// Spawn 2 → Reactor 1
// Spawn 3 → Reactor 2
// Spawn 4 → Reactor 3
// Spawn 5 → Reactor 0 (wraps)
```

## The Correct Architecture

### ❌ WRONG: Separate tokio::main Runtime

```rust
#[tokio::main]  // ← Creates SEPARATE runtime
async fn main() {
    let db = MemoryDB::new(4)?;
    db.put_one(...).await?;  // Runs on main runtime, NOT reactors
}
```

### ✅ CORRECT: Run App ON a Reactor

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let db = MemoryDB::new(4)?;
    
    // Run entire app on reactor 0
    iomgr::run_on(iomgr::ReactorTarget::Reactor(0), async move {
        // NOW we're on a reactor thread!
        // All .await operations run in reactor context
        
        db.put_one("users", &key, &value).await?;
        
        Ok::<(), Box<dyn std::error::Error>>(())
    })?;
    
    db.shutdown().await;
    Ok(())
}
```

### ✅ BEST: Distribute Work Across All Reactors

```rust
use iomgr::{BackgroundTasks, ReactorTarget};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let db = Arc::new(MemoryDB::new(4)?);
    
    // Start on reactor 0
    iomgr::run_on(ReactorTarget::Reactor(0), async move {
        let bg_tasks = BackgroundTasks::new();
        
        // Distribute 1000 operations across all 4 reactors
        for i in 0..1000 {
            let db = Arc::clone(&db);
            let reactor_id = i % 4;  // Round-robin manually
            
            bg_tasks.spawn(ReactorTarget::Reactor(reactor_id), async move {
                let key = i.to_le_bytes();
                let value = vec![i as u8; 16];
                db.put_one("users", &key, &value).await?;
                Ok::<(), MemDbError>(())
            });
        }
        
        // Wait for all 1000 operations to complete
        bg_tasks.join_all().await;
        
        println!("All operations complete!");
        Ok::<(), Box<dyn std::error::Error>>(())
    })?;
    
    Ok(())
}
```

**This achieves:**
- ✅ All 4 reactors actively processing work
- ✅ True parallel execution
- ✅ Round-robin distribution (250 ops per reactor)
- ✅ Optimal CPU utilization

## Why Not Auto-Spawn Inside MemoryDB?

I tried implementing `put_one` to internally call `spawn_waitable`, but this fails with:

```
panic: spawn_waitable can only be called from a reactor thread
```

**IOManager's design constraint:** You must ALREADY be on a reactor to spawn work to other reactors.

This means:
- Can't hide reactor scheduling inside MemoryDB
- User must structure their app to run ON reactors
- This is intentional - gives users explicit control

## Recommended Application Structure

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. Initialize MemoryDB (creates reactors)
    let db = Arc::new(MemoryDB::new(4)?);
    
    // 2. Run application logic on a reactor
    iomgr::run_on(ReactorTarget::Reactor(0), async move {
        // Your application code here
        application_logic(db).await?;
        Ok::<(), Box<dyn std::error::Error>>(())
    })?;
    
    // 3. Cleanup
    Ok(())
}

async fn application_logic(db: Arc<MemoryDB>) -> Result<(), Box<dyn std::error::Error>> {
    // Option A: Sequential operations (all on current reactor)
    for i in 0..100 {
        db.put_one("users", &i.to_le_bytes(), &vec![i as u8]).await?;
    }
    
    // Option B: Parallel operations (distribute across reactors)
    let bg_tasks = BackgroundTasks::new();
    for i in 0..1000 {
        let db = Arc::clone(&db);
        bg_tasks.spawn(ReactorTarget::Reactor(i % 4), async move {
            db.put_one("users", &i.to_le_bytes(), &vec![i as u8]).await?;
            Ok::<(), MemDbError>(())
        });
    }
    bg_tasks.join_all().await;
    
    Ok(())
}
```

## Performance Characteristics

### Without Reactor Execution (Current Default):
- ❌ Reactor threads idle
- ❌ All work on tokio::main threads
- ❌ No benefit from reactor architecture
- ❌ Wasted resources (4 idle threads)

### With Reactor Execution (Recommended):
- ✅ All 4 reactors actively working
- ✅ True parallel execution
- ✅ Optimal for async I/O workloads
- ✅ Linear scaling with num_reactors

### Overhead:
- Channel-based spawning: ~1-5 μs per spawn
- Negligible compared to B-tree operations (10-1000 μs)
- Parallelism gains >> spawn overhead

## Key Takeaways

1. **`init_iomgr(4)` creates 4 separate reactor threads, each with its own tokio runtime**

2. **`#[tokio::main]` creates a DIFFERENT tokio runtime that doesn't use reactors**

3. **Just calling `.await` does NOT use reactor threads**

4. **To use reactors, you must run your app ON a reactor using `iomgr::run_on()`**

5. **Work distribution is round-robin via atomic counter `spawn_rr`**

6. **`BackgroundTasks` is the tool for parallel execution across reactors**

7. **You can't hide reactor spawning inside MemoryDB due to IOManager constraints**

## Your Original Question: Answered

> "Won't it always run in same reactor?"

**With correct usage (BackgroundTasks):** NO - work is distributed round-robin
**With wrong usage (#[tokio::main]):** YES - but worse, it's not on ANY reactor!

The reactor selection depends on how you spawn:
- `ReactorTarget::Reactor(id)` → Always that specific reactor
- `ReactorTarget::Any` → Round-robin (next_reactor() uses atomic counter)
- `ReactorTarget::Current` → Current reactor (must already be on one)

## Files

See `/Users/hkadayam/src/Homestore/homedb/REACTOR_EXECUTION_EXPLAINED.md` for even more details.
