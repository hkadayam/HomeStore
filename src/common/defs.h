/*********************************************************************************
 *
 * Author/Developer(s): Harihara Kadayam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>

#include <boost/intrusive_ptr.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

// ── Common type aliases ─────────────────────────────────────────────────────

using Uuid = boost::uuids::uuid;

// ── Smart-pointer aliases ────────────────────────────────────────────────────

template < typename T >
using shared = std::shared_ptr< T >;

template < typename T >
using cshared = const std::shared_ptr< T >;

template < typename T >
using unique = std::unique_ptr< T >;

template < typename T >
using intrusive = boost::intrusive_ptr< T >;

template < typename T >
using cintrusive = const boost::intrusive_ptr< T >;

// ── Cast shortcuts ───────────────────────────────────────────────────────────
//
// Generic-type casts (supply the target type explicitly):
//   r_cast<T*>(p)    reinterpret_cast<T*>
//   s_cast<T>(v)     static_cast<T>
//   d_cast<T*>(p)    dynamic_cast<T*>
//   dp_cast<T>(sp)   std::dynamic_pointer_cast<T>
//   sp_cast<T>(sp)   std::static_pointer_cast<T>
//
// Fixed-type scalar casts (supply the value only):
//   to_i64(v)    static_cast< int64_t >
//   to_u64(v)    static_cast< uint64_t >
//   to_u32(v)    static_cast< uint32_t >
//   to_i32(v)    static_cast< int32_t >
//   to_u16(v)    static_cast< uint16_t >
//   to_i16(v)    static_cast< int16_t >
//   to_u8(v)     static_cast< uint8_t >
//   to_int(v)    static_cast< int >
//   to_size(v)   static_cast< size_t >
//
// Fixed-type pointer casts (reinterpret):
//   to_u8ptr(p)   reinterpret_cast< uint8_t* >
//   to_ccptr(p)   reinterpret_cast< const char* >
//   to_cptr(p)    reinterpret_cast< char* >
//   to_vptr(p)    reinterpret_cast< void* >
//   to_cvptr(p)   reinterpret_cast< const void* >

#define r_cast reinterpret_cast
#define s_cast static_cast
#define d_cast dynamic_cast
#define dp_cast std::dynamic_pointer_cast
#define sp_cast std::static_pointer_cast

#define to_int(v) static_cast< int >(v)
#define to_i64(v) static_cast< int64_t >(v)
#define to_u64(v) static_cast< uint64_t >(v)
#define to_u32(v) static_cast< uint32_t >(v)
#define to_i32(v) static_cast< int32_t >(v)
#define to_u16(v) static_cast< uint16_t >(v)
#define to_i16(v) static_cast< int16_t >(v)
#define to_u8(v) static_cast< uint8_t >(v)
#define to_int(v) static_cast< int >(v)
#define to_size(v) static_cast< size_t >(v)
#define to_double(v) static_cast< double >(v)
#define to_float(v) static_cast< float >(v)
#define to_ulong(v) static_cast< ulong >(v)
#define to_bool(v) static_cast< bool >(v)

#define to_u8ptr(p) reinterpret_cast< uint8_t* >(p)
#define to_cu8ptr(p) reinterpret_cast< const uint8_t* >(p)
#define to_cptr(p) reinterpret_cast< char* >(p)
#define to_ccptr(p) reinterpret_cast< const char* >(p)
#define to_vptr(p) reinterpret_cast< void* >(p)
#define to_cvptr(p) reinterpret_cast< const void* >(p)

// Legacy aliases for existing code.
#define uintptr_cast reinterpret_cast< uint8_t* >
#define voidptr_cast reinterpret_cast< void* >
#define c_voidptr_cast reinterpret_cast< const void* >
#define charptr_cast reinterpret_cast< char* >
#define c_charptr_cast reinterpret_cast< const char* >
#define int_cast static_cast< int >
#define uint32_cast static_cast< uint32_t >
#define int64_cast static_cast< int64_t >
#define uint64_cast static_cast< uint64_t >
#define size_cast static_cast< size_t >
