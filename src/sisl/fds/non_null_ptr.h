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

#include <memory>

namespace sisl {

template < typename T >
class NonNullUniquePtr : public std::unique_ptr< T > {
public:
    NonNullUniquePtr() noexcept : std::unique_ptr< T >{new T} {}
    NonNullUniquePtr(T* ptr) : std::unique_ptr< T >{ptr} {
        if (!*this) { std::unique_ptr< T >::reset(new T); }
    }
    NonNullUniquePtr(const NonNullUniquePtr&) = delete;
    NonNullUniquePtr& operator=(const NonNullUniquePtr< T >&) = delete;

    ~NonNullUniquePtr() { std::unique_ptr< T >::reset(); }

    NonNullUniquePtr(NonNullUniquePtr&& other) : std::unique_ptr< T >{std::move(other)} { assert(*this); }

    NonNullUniquePtr& operator=(NonNullUniquePtr&& other) {
        assert(other);
        std::unique_ptr< T >::operator=(std::move(other));
        return *this;
    }

    NonNullUniquePtr& operator=(std::unique_ptr< T >&& other) {
        assert(other);
        std::unique_ptr< T >::operator=(std::move(other));
        return *this;
    }

    T& operator*() const {
        assert(*this);
        return std::unique_ptr< T >::operator*();
    }

    T* operator->() const noexcept {
        assert(*this);
        return std::unique_ptr< T >::operator->();
    }

    T* release() noexcept {
        assert(*this);
        T* ret = std::unique_ptr< T >::release();
        reset();
        return ret;
    }

    void reset(T* t = nullptr) noexcept {
        if (t == nullptr) t = new T;
        std::unique_ptr< T >::reset(t);
    }

    void swap(NonNullUniquePtr& other) noexcept {
        if (!other) other.reset(new T);
        std::unique_ptr< T >::swap(other);
    }
};

template < typename T >
struct EmbeddedT : public T {
    EmbeddedT(T* t) {
        if (t != nullptr) {
            *static_cast< T* >(this) = std::move(*t);
            delete t;
        }
    }
    EmbeddedT() = default;

    const T* get() const noexcept { return this; }
    T* get() noexcept { return this; }
    const T* operator->() const noexcept { return this; }
    T* operator->() noexcept { return this; }

    const T& operator*() const noexcept { return *this; }
    T& operator*() noexcept { return *this; }

    explicit operator bool() const noexcept { return true; }

    T* release() noexcept {
        EmbeddedT* ret = new EmbeddedT();
        *ret = *this;
        return static_cast< T* >(ret);
    }

    void reset() noexcept { *this = EmbeddedT{nullptr}; }
};

template < class T >
constexpr bool operator==(const EmbeddedT< T >&, std::nullptr_t) noexcept { return false; }

template < class T >
constexpr bool operator!=(const EmbeddedT< T >&, std::nullptr_t) noexcept { return true; }

// Legacy alias used by flatbuffers-generated code
template < class T >
using embedded_t = EmbeddedT< T >;

} // namespace sisl
