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
impl BenchResult {
    fn throughput_mops(&self) -> f64 { (self.total_ops as f64 / self.duration.as_secs_f64()) / 1_000_000.0 }
}

fn bench_uncontended_mutex<M: 'static + Send>(make: impl Fn() -> M, lock_fn: impl Fn(&M)) -> BenchResult {
    let m = make();
    let iterations: u64 = 5_000_000; // Keep it moderate for CI/runtime
    let start = Instant::now();
    for _ in 0..iterations {
        lock_fn(&m);
    }
    BenchResult { name: "uncontended", total_ops: iterations, duration: start.elapsed() }
}

fn bench_contended_mutex<M, F>(make: impl Fn() -> M, lock_fn: F, threads: usize) -> BenchResult
where
    M: 'static + Send + Sync,
    F: Copy + Send + Sync + 'static + Fn(&M),
{
    let m = std::sync::Arc::new(make());
    let iterations_per_thread: u64 = 1_000_000; // total ops = threads * iterations_per_thread
    let start = Instant::now();
    let mut handles = Vec::with_capacity(threads);
    for _ in 0..threads {
        let m_ref = m.clone();
        handles.push(thread::spawn(move || {
            for _ in 0..iterations_per_thread {
                lock_fn(&m_ref);
            }
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    let total_ops = iterations_per_thread * threads as u64;
    BenchResult { name: "contended", total_ops, duration: start.elapsed() }
}

fn bench_rwlock_read<RW, F>(make: impl Fn() -> RW, read_fn: F, threads: usize) -> BenchResult
where
    RW: 'static + Send + Sync,
    F: Copy + Send + Sync + 'static + Fn(&RW),
{
    let rw = std::sync::Arc::new(make());
    let iterations_per_thread: u64 = 1_000_000;
    let start = Instant::now();
    let mut handles = Vec::with_capacity(threads);
    for _ in 0..threads {
        let rw_ref = rw.clone();
        handles.push(thread::spawn(move || {
            for _ in 0..iterations_per_thread {
                read_fn(&rw_ref);
            }
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    let total_ops = iterations_per_thread * threads as u64;
    BenchResult { name: "rw_read", total_ops, duration: start.elapsed() }
}

fn bench_rwlock_write<RW, F>(make: impl Fn() -> RW, write_fn: F, threads: usize) -> BenchResult
where
    RW: 'static + Send + Sync,
    F: Copy + Send + Sync + 'static + Fn(&RW),
{
    let rw = std::sync::Arc::new(make());
    let iterations_per_thread: u64 = 200_000; // fewer writes, heavier contention
    let start = Instant::now();
    let mut handles = Vec::with_capacity(threads);
    for _ in 0..threads {
        let rw_ref = rw.clone();
        handles.push(thread::spawn(move || {
            for _ in 0..iterations_per_thread {
                write_fn(&rw_ref);
            }
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    let total_ops = iterations_per_thread * threads as u64;
    BenchResult { name: "rw_write", total_ops, duration: start.elapsed() }
}

fn print_result(header: &str, r: &BenchResult) {
    println!(
        "{:20} {:12} ops={} time={:.3?} thrpt={:.2} Mops/s",
        header,
        r.name,
        r.total_ops,
        r.duration,
        r.throughput_mops()
    );
}

fn main() {
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(8).min(16); // cap for stability
    println!("Lock benchmark (threads = {})", threads);
    println!("NOTE: These are microbenchmarks. Real-world performance depends on critical section size, contention pattern, CPU architecture.");

    // std::Mutex
    let std_uncontended = bench_uncontended_mutex(
        || StdMutex::new(()),
        |m| {
            let _g = m.lock().unwrap();
        },
    );
    let std_contended = bench_contended_mutex(
        || StdMutex::new(()),
        |m| {
            let _g = m.lock().unwrap();
        },
        threads,
    );

    // parking_lot::Mutex
    let pl_uncontended = bench_uncontended_mutex(
        || PlMutex::new(()),
        |m| {
            let _g = m.lock();
        },
    );
    let pl_contended = bench_contended_mutex(
        || PlMutex::new(()),
        |m| {
            let _g = m.lock();
        },
        threads,
    );

    // std::RwLock read vs write
    let std_rw_read = bench_rwlock_read(
        || StdRwLock::new(0u64),
        |rw| {
            let _g = rw.read().unwrap();
        },
        threads,
    );
    let std_rw_write = bench_rwlock_write(
        || StdRwLock::new(0u64),
        |rw| {
            let mut g = rw.write().unwrap();
            *g += 1;
        },
        threads,
    );

    // parking_lot::RwLock read vs write
    let pl_rw_read = bench_rwlock_read(
        || PlRwLock::new(0u64),
        |rw| {
            let _g = rw.read();
        },
        threads,
    );
    let pl_rw_write = bench_rwlock_write(
        || PlRwLock::new(0u64),
        |rw| {
            let mut g = rw.write();
            *g += 1;
        },
        threads,
    );

    println!("\nResults (throughput higher is better):");
    print_result("std::Mutex", &std_uncontended);
    print_result("std::Mutex", &std_contended);
    print_result("pl::Mutex", &pl_uncontended);
    print_result("pl::Mutex", &pl_contended);
    print_result("std::RwLock", &std_rw_read);
    print_result("std::RwLock", &std_rw_write);
    print_result("pl::RwLock", &pl_rw_read);
    print_result("pl::RwLock", &pl_rw_write);

    // Simple comparative summary
    println!("\nSummary:");
    println!(
        " - parking_lot::Mutex vs std::Mutex uncontended speedup: {:.2}x",
        pl_uncontended.throughput_mops() / std_uncontended.throughput_mops()
    );
    println!(
        " - parking_lot::Mutex vs std::Mutex contended speedup: {:.2}x",
        pl_contended.throughput_mops() / std_contended.throughput_mops()
    );
    println!(
        " - parking_lot::RwLock read speedup: {:.2}x",
        pl_rw_read.throughput_mops() / std_rw_read.throughput_mops()
    );
    println!(
        " - parking_lot::RwLock write speedup: {:.2}x",
        pl_rw_write.throughput_mops() / std_rw_write.throughput_mops()
    );
}
