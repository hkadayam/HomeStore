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
    atomic_bitset.set_bit(24);
    println!("AtomicBitset: bit 24 = {}", atomic_bitset.get_bitval(24));

    // For thread safety, wrap in Arc<RwLock<>>
    let thread_safe_bitset = Arc::new(RwLock::new(Bitset::new(100, 3)));

    // Write operation
    {
        let mut guard = thread_safe_bitset.write().unwrap();
        guard.set_bit(99);
        println!("ThreadSafe Bitset: Set bit 99");
    }

    // Read operation
    {
        let guard = thread_safe_bitset.read().unwrap();
        println!("ThreadSafe Bitset: bit 99 = {}", guard.get_bitval(99));
    }

    println!("\nKey Benefits:");
    println!("✅ Removed complex const generic thread safety");
    println!("✅ Thread safety is caller's responsibility");
    println!("✅ Clean, simple API with no borrowing conflicts");
    println!("✅ Use Arc<RwLock<Bitset>> for actual thread safety");

    println!("\nAs you requested:");
    println!("- Removed THREAD_SAFE template variable");
    println!("- Removed ThreadSafeResizing complexity");
    println!("- Let caller handle external locking");
}
