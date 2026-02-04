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
/// that can be reused without re-creating objects.
pub struct ChunkPool {
    /// Available chunks by size (chunk_size -> Vec<Arc<Chunk>>)
    pools: Mutex<HashMap<u64, Vec<Arc<super::chunk::Chunk>>>>,
    /// Maximum number of chunks to keep in pool per size
    pool_limit: usize,
}

impl ChunkPool {
    pub fn new(pool_limit: usize) -> Self {
        Self {
            pools: Mutex::new(HashMap::new()),
            pool_limit,
        }
    }
    
    /// Check if there's room for more chunks of this size
    pub fn has_room(&self, chunk_size: u64) -> bool {
        self.pools.lock()
            .get(&chunk_size)
            .map(|v| v.len() < self.pool_limit)
            .unwrap_or(true)  // If no pool exists yet, we have room
    }
    
    /// Return a chunk to the pool
    /// 
    /// Caller is responsible for:
    /// 1. Checking has_room() before deactivating the chunk
    /// 2. Deactivating the chunk before returning it
    /// 
    /// This method always accepts the chunk - the limit check should be done
    /// via has_room() before deactivation to decide whether to pool or remove.
    pub fn return_chunk(&self, chunk: Arc<super::chunk::Chunk>) {
        let chunk_size = chunk.info().chunk_size;
        let mut pools = self.pools.lock();
        let pool = pools.entry(chunk_size).or_insert_with(Vec::new);
        pool.push(chunk);
    }
    
    /// Try to get a chunk from pool
    pub fn try_get_chunk(&self, chunk_size: u64) -> Option<Arc<super::chunk::Chunk>> {
        self.pools.lock().get_mut(&chunk_size)?.pop()
    }
    
    /// Get count of available chunks for a size
    pub fn available_count(&self, chunk_size: u64) -> usize {
        self.pools.lock()
            .get(&chunk_size)
            .map(|v| v.len())
            .unwrap_or(0)
    }
}
