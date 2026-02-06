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

//! Test Key/Value Types and Generators for Btree Tests
//!
//! This module provides reusable test types and generators for btree testing:
//! - FixedSizeTestKey/Value - Fixed-size types (8 bytes)
//! - TestVarLenKey/Value - Variable-size types (Strings)
//! - Generator trait and implementations for sequential/random generation
//!
//! Can be used across test_btree.rs, test_btree_node.rs, and future COW tests.

use rand::{Rng, SeedableRng, rngs::StdRng};
use crate::index::btree::btree_kvs::{BtreeKey, BtreeValue};
use std::io::{Error, ErrorKind, Result};

//================================================================================
// FixedSizeTestKey - Fixed-size key type (8 bytes, wraps u64)
//================================================================================

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct FixedSizeTestKey {
    pub value: u64,
    pub id: u64,  // Cached ID for test tracking (not serialized)
}

impl std::fmt::Debug for FixedSizeTestKey {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        if f.alternate() {
            // Verbose: {:#?}
            write!(f, "FixedSizeTestKey {{ value: {}, id: {} }}", self.value, self.id)
        } else {
            // Concise: {:?}
            write!(f, "K({})", self.value)
        }
    }
}

impl FixedSizeTestKey {
    /// Create key with value and id
    pub fn new(value: u64, id: u64) -> Self {
        FixedSizeTestKey { value, id }
    }
    
    /// Create key from u64 value (useful for lookups, id == value)
    pub fn from_u64(val: u64) -> Self {
        FixedSizeTestKey { value: val, id: val }
    }
}

impl BtreeKey for FixedSizeTestKey {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);
    
    fn serialized_size(&self) -> u32 {
        8
    }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> Result<u32> {
        if buf.len() < 8 {
            return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for FixedSizeTestKey"));
        }
        buf[0..8].copy_from_slice(&self.value.to_le_bytes());
        Ok(8)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> Result<Self> {
        if buf.len() < 8 {
            return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for FixedSizeTestKey"));
        }
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&buf[0..8]);
        let value = u64::from_le_bytes(bytes);
        Ok(FixedSizeTestKey { value, id: value })  // Deserialized keys use value as id
    }
    
    fn get_max_size() -> u32 {
        8
    }
}

//================================================================================
// FixedSizeTestValue - Fixed-size value type (8 bytes, wraps u64)
//================================================================================

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct FixedSizeTestValue {
    pub value: u64,
    pub id: u64,  // Cached ID for test tracking (not serialized)
}

impl std::fmt::Debug for FixedSizeTestValue {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        if f.alternate() {
            // Verbose: {:#?}
            write!(f, "FixedSizeTestValue {{ value: {}, id: {} }}", self.value, self.id)
        } else {
            // Concise: {:?}
            write!(f, "V({})", self.value)
        }
    }
}

impl FixedSizeTestValue {
    /// Create value with value and id
    pub fn new(value: u64, id: u64) -> Self {
        FixedSizeTestValue { value, id }
    }
    
    /// Create value from u64 (useful for validation, id == value)
    pub fn from_u64(val: u64) -> Self {
        FixedSizeTestValue { value: val, id: val }
    }
}

impl BtreeValue for FixedSizeTestValue {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);
    
    fn serialized_size(&self) -> u32 {
        8
    }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> Result<u32> {
        if buf.len() < 8 {
            return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for FixedSizeTestValue"));
        }
        buf[0..8].copy_from_slice(&self.value.to_le_bytes());
        Ok(8)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> Result<Self> {
        if buf.len() < 8 {
            return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for FixedSizeTestValue"));
        }
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&buf[0..8]);
        let value = u64::from_le_bytes(bytes);
        Ok(FixedSizeTestValue { value, id: value })  // Deserialized values use value as id
    }
}

//================================================================================
// TestVarLenKey - Variable-size key type (stores String)
//================================================================================

#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct TestVarLenKey {
    pub value: String,
    pub id: u64,  // Cached ID for test tracking (not serialized)
}

impl std::fmt::Debug for TestVarLenKey {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        if f.alternate() {
            // Verbose: {:#?}
            write!(f, "TestVarLenKey {{ value: \"{}\", id: {}, len: {} }}", 
                   self.value, self.id, self.value.len())
        } else {
            // Concise: {:?}
            write!(f, "K(\"{}\")", self.value)
        }
    }
}

impl TestVarLenKey {
    /// Create key with string and id
    pub fn new(value: String, id: u64) -> Self {
        TestVarLenKey { value, id }
    }
    
    /// Create key from u64 id (useful for lookups)
    pub fn from_u64(id: u64) -> Self {
        TestVarLenKey { value: format!("key_{:010}", id), id }
    }
}

impl BtreeKey for TestVarLenKey {
    const FIXED_SERIALIZED_SIZE: Option<u32> = None;  // Variable!
    
    fn serialized_size(&self) -> u32 {
        4 + self.value.len() as u32  // 4 bytes for length prefix
    }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> Result<u32> {
        let len = self.value.len() as u32;
        if buf.len() < (4 + len as usize) { return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for TestVarLenKey")); }
        buf[0..4].copy_from_slice(&len.to_le_bytes());
        buf[4..4 + len as usize].copy_from_slice(self.value.as_bytes());
        Ok(4 + len)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> Result<Self> {
        if buf.len() < 4 { return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for TestVarLenKey length")); }
        let mut len_bytes = [0u8; 4];
        len_bytes.copy_from_slice(&buf[0..4]);
        let len = u32::from_le_bytes(len_bytes) as usize;
        
        if buf.len() < 4 + len { return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for TestVarLenKey data")); }
        
        let s = String::from_utf8(buf[4..4 + len].to_vec()).map_err(|e| Error::new(ErrorKind::InvalidData, e))?;
        // Extract id from string "key_0000000042" -> 42, or default to 0
        let id = s.strip_prefix("key_").and_then(|n| n.parse().ok()).unwrap_or(0);
        Ok(TestVarLenKey { value: s, id })
    }
    
    fn get_max_size() -> u32 {
        1024  // Max 1KB keys
    }
}

//================================================================================
// TestVarLenValue - Variable-size value type (stores String)
//================================================================================

#[derive(Clone, PartialEq, Eq)]
pub struct TestVarLenValue {
    pub value: String,
    pub id: u64,  // Cached ID for test tracking (not serialized)
}

impl std::fmt::Debug for TestVarLenValue {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        if f.alternate() {
            // Verbose: {:#?}
            write!(f, "TestVarLenValue {{ value: \"{}\", id: {}, len: {} }}", 
                   self.value, self.id, self.value.len())
        } else {
            // Concise: {:?}
            write!(f, "V(\"{}\")", self.value)
        }
    }
}

impl TestVarLenValue {
    /// Create value with string and id
    pub fn new(value: String, id: u64) -> Self {
        TestVarLenValue { value, id }
    }
    
    /// Create value from u64 id (useful for validation)
    pub fn from_u64(id: u64) -> Self {
        TestVarLenValue { value: format!("value_{:010}", id), id }
    }
}

impl BtreeValue for TestVarLenValue {
    const FIXED_SERIALIZED_SIZE: Option<u32> = None;  // Variable!
    
    fn serialized_size(&self) -> u32 {
        4 + self.value.len() as u32  // 4 bytes for length prefix
    }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> Result<u32> {
        let len = self.value.len() as u32;
        if buf.len() < (4 + len as usize) { return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for TestVarLenValue")); }
        buf[0..4].copy_from_slice(&len.to_le_bytes());
        buf[4..4 + len as usize].copy_from_slice(self.value.as_bytes());
        Ok(4 + len)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> Result<Self> {
        if buf.len() < 4 { return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for TestVarLenValue length")); }
        let mut len_bytes = [0u8; 4];
        len_bytes.copy_from_slice(&buf[0..4]);
        let len = u32::from_le_bytes(len_bytes) as usize;
        
        if buf.len() < 4 + len { return Err(Error::new(ErrorKind::InvalidInput, "Buffer too small for TestVarLenValue data")); }
        
        let s = String::from_utf8(buf[4..4 + len].to_vec()).map_err(|e| Error::new(ErrorKind::InvalidData, e))?;
        // Extract id from string "value_0000000042" -> 42, or default to 0
        let id = s.strip_prefix("value_").and_then(|n| n.parse().ok()).unwrap_or(0);
        Ok(TestVarLenValue { value: s, id })
    }
}

//================================================================================
// Generators section - FixedSizeTestKey, FixedSizeTestValue, TestVarLenKey, TestVarLenValue
//================================================================================
#[derive(Clone, Copy, Debug)]
pub enum GenMode {
    Sequential,
    Random,
}

/// Generator trait - each type implements its own generator
/// The ID determines the key/value content and ordering
pub trait Generator: Send {
    type Output;
    
    /// Generate key/value with optional specific ID
    /// - If id is Some(n): use n as the ID to generate from
    /// - If id is None: pick next ID based on mode (sequential/random), then generate from it
    /// 
    /// The generated key/value content is derived from the ID:
    /// - FixedSize: value = id
    /// - VarLen: value = formatted string with id embedded (maintains ordering)
    /// 
    /// Returns (generated_value, id_used)
    fn generate(&mut self, id: Option<u64>) -> (Self::Output, u64);
}

//================================================================================
// Generators for FixedSizeTestKey
//================================================================================
pub struct FixedSizeKeyGenerator {
    counter: u64,
    mode: GenMode,
    rng: StdRng,
    max_entries: u64,
}

impl FixedSizeKeyGenerator {
    pub fn new(mode: GenMode, seed: u64, max_entries: u64) -> Self {
        Self {
            counter: 0,
            mode,
            rng: StdRng::seed_from_u64(seed),
            max_entries,
        }   
    }
    
    /// Switch generation mode
    pub fn set_mode(&mut self, mode: GenMode) {
        self.mode = mode;
    }
    
    /// Reset counter to 0
    pub fn reset(&mut self) {
        self.counter = 0;
    }
}

impl Generator for FixedSizeKeyGenerator {
    type Output = FixedSizeTestKey;
    
    fn generate(&mut self, id: Option<u64>) -> (FixedSizeTestKey, u64) {
        let key_id = match id {
            Some(id) => id,
            None => match self.mode {
                GenMode::Sequential => {
                    let id = self.counter;
                    self.counter += 1;
                    id
                }
                GenMode::Random => self.rng.gen_range(0..self.max_entries),
            }
        };
        (FixedSizeTestKey::new(key_id, key_id), key_id)
    }
}

//================================================================================
// Generators for FixedSizeTestValue
//================================================================================
pub struct FixedSizeValueGenerator {
    counter: u64,
    mode: GenMode,
    rng: StdRng,
}

impl FixedSizeValueGenerator {
    pub fn new(mode: GenMode, seed: u64) -> Self {
        Self {
            counter: 0,
            mode,
            rng: StdRng::seed_from_u64(seed),
        }
    }
    
    pub fn set_mode(&mut self, mode: GenMode) {
        self.mode = mode;
    }
    
    pub fn reset(&mut self) {
        self.counter = 0;
    }
}

impl Generator for FixedSizeValueGenerator {
    type Output = FixedSizeTestValue;
    
    fn generate(&mut self, id: Option<u64>) -> (FixedSizeTestValue, u64) {
        let value_id = match id {
            Some(id) => id,
            None => match self.mode {
                GenMode::Sequential => {
                    let id = self.counter;
                    self.counter += 1;
                    id
                }
                GenMode::Random => self.rng.gen_range(0..u64::MAX),
            }
        };
        
        // Value content is ALWAYS derived from id (mode only affects id selection)
        (FixedSizeTestValue::new(value_id, value_id), value_id)
    }
}

//================================================================================
// Generators for TestVarLenKey
//================================================================================

pub struct VarLenKeyGenerator {
    counter: u64,
    mode: GenMode,
    rng: StdRng,
    max_entries: u64,
}

impl VarLenKeyGenerator {
    pub fn new(mode: GenMode, seed: u64, max_entries: u64) -> Self {
        Self {
            counter: 0,
            mode,
            rng: StdRng::seed_from_u64(seed),
            max_entries,
        }
    }
    
    pub fn set_mode(&mut self, mode: GenMode) {
        self.mode = mode;
    }
    
    pub fn reset(&mut self) {
        self.counter = 0;
    }
}

impl Generator for VarLenKeyGenerator {
    type Output = TestVarLenKey;
    
    fn generate(&mut self, id: Option<u64>) -> (TestVarLenKey, u64) {
        let key_id = match id {
            Some(id) => id,
            None => match self.mode {
                GenMode::Sequential => {
                    let id = self.counter;
                    self.counter += 1;
                    id
                }
                GenMode::Random => self.rng.gen_range(0..self.max_entries),
            }
        };
        (TestVarLenKey::new(format!("key_{:010}", key_id), key_id), key_id)
    }
}

//================================================================================
// Generators for TestVarLenValue
//================================================================================

pub struct VarLenValueGenerator {
    counter: u64,
    mode: GenMode,
    rng: StdRng,
}

impl VarLenValueGenerator {
    pub fn new(mode: GenMode, seed: u64) -> Self {
        Self {
            counter: 0,
            mode,
            rng: StdRng::seed_from_u64(seed),
        }
    }
    
    pub fn set_mode(&mut self, mode: GenMode) {
        self.mode = mode;
    }
    
    pub fn reset(&mut self) {
        self.counter = 0;
    }
}

impl Generator for VarLenValueGenerator {
    type Output = TestVarLenValue;
    
    fn generate(&mut self, id: Option<u64>) -> (TestVarLenValue, u64) {
        let value_id = match id {
            Some(id) => id,
            None => match self.mode {
                GenMode::Sequential => {
                    let id = self.counter;
                    self.counter += 1;
                    id
                }
                GenMode::Random => self.rng.gen_range(0..u64::MAX),
            }
        };
        
        // Value content is ALWAYS derived from id (mode only affects id selection)
        (TestVarLenValue::new(format!("value_{:010}", value_id), value_id), value_id)
    }
}