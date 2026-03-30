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
 ***************************************************************************/
#pragma once

#include <gtest/gtest.h>
#include <folly/coro/Task.h>
#include "iomanager/iomanager.h"

// gtest's ASSERT_* macros use `return;` which is a compile error inside coroutines.
// These wrappers provide the same abort-on-failure behavior using `co_return;` instead.
#define CO_ASSERT_TRUE(cond)                                                                                           \
    do {                                                                                                               \
        EXPECT_TRUE(cond);                                                                                             \
        if (!(cond)) co_return;                                                                                        \
    } while (0)
#define CO_ASSERT_FALSE(cond)                                                                                          \
    do {                                                                                                               \
        EXPECT_FALSE(cond);                                                                                            \
        if ((cond)) co_return;                                                                                         \
    } while (0)
#define CO_ASSERT_EQ(a, b)                                                                                             \
    do {                                                                                                               \
        EXPECT_EQ(a, b);                                                                                               \
        if ((a) != (b)) co_return;                                                                                     \
    } while (0)
#define CO_ASSERT_NE(a, b)                                                                                             \
    do {                                                                                                               \
        EXPECT_NE(a, b);                                                                                               \
        if ((a) == (b)) co_return;                                                                                     \
    } while (0)

// Wraps a coroutine body in spawn_and_block so gtest can run it as a regular TEST_F.
#define CORO_TEST_F(fixture, name)                                                                                     \
    folly::coro::Task< void > fixture##_##name##_coro(fixture& self);                                                  \
    TEST_F(fixture, name) {                                                                                            \
        homestore::iomgr().spawn_and_block(                                                                            \
            homestore::ReactorTarget::any(),                                                                           \
            [this]() -> folly::coro::Task< void > { co_await fixture##_##name##_coro(*this); }());                     \
    }                                                                                                                  \
    folly::coro::Task< void > fixture##_##name##_coro([[maybe_unused]] fixture& self)