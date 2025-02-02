/*
 * Tencent is pleased to support the open source community by making
 * MMKV available.
 *
 * Copyright (C) 2018 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MMKV_SCOPEDLOCK_HPP
#define MMKV_SCOPEDLOCK_HPP
#ifdef __cplusplus

namespace mmkv {

template <typename T>
class ScopedLock {
    T *m_lock;

    void lock() {
        if (m_lock) {
            m_lock->lock();
        }
    }

    void unlock() {
        if (m_lock) {
            m_lock->unlock();
        }
    }

public:
    explicit ScopedLock(T *oLock) : m_lock(oLock) {
        MMKV_ASSERT(m_lock);
        lock();
    }

    ~ScopedLock() {
        unlock();
        m_lock = nullptr;
    }

    // just forbid it for possibly misuse
    explicit ScopedLock(const ScopedLock<T> &other) = delete;
    ScopedLock &operator=(const ScopedLock<T> &other) = delete;
};

} // namespace mmkv

#include <type_traits>

/**
简洁性：确实没有中间宏也可以实现目标，代码更简洁。
灵活性与扩展性：使用中间宏的设计让未来的扩展更加灵活。
            如果将来需要给锁添加更多功能（比如条件检查或日志等），
            你可以很容易地修改中间宏，而不影响外部的 SCOPED_LOCK 宏。
调试与维护：分层宏有助于调试和维护，尤其是在宏展开较复杂的情况下。
 */
//用户直接使用的，传入一个锁对象（lock）。
// 它的作用是将用户提供的 lock 和 __COUNTER__（一个编译器自动递增的数字）传递给 _SCOPEDLOCK 宏。
//目的：简化调用，隐藏内部实现，确保用户只需要输入一个宏来创建作用域锁。
#define SCOPED_LOCK(lock) _SCOPEDLOCK(lock, __COUNTER__)
//这个宏是一个中间宏，它只是将传入的参数传递给下一级的 __SCOPEDLOCK 宏，实际上没有做任何其他事情。
//目的：它的存在是为了分层扩展宏，提供更灵活的扩展和调试能力。将宏调用链条从 SCOPED_LOCK 中解耦出来，增强了代码的可维护性和扩展性。
#define _SCOPEDLOCK(lock, counter) __SCOPEDLOCK(lock, counter)
//最终执行的宏，它会展开为 ScopedLock 类的构造函数。根据传入的 lock 对象，创建一个 ScopedLock 实例，并确保锁的加锁和解锁。
//目的：执行实际的加锁逻辑，创建 ScopedLock 对象，并确保 lock 在作用域结束时被释放。
#define __SCOPEDLOCK(lock, counter)                                                                                    \
    mmkv::ScopedLock<std::remove_pointer<decltype(lock)>::type> __scopedLock##counter(lock)

#endif
#endif //MMKV_SCOPEDLOCK_HPP
