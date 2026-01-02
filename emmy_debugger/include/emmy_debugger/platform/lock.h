/*
* Copyright (c) 2019. tangzx(love.tangzx@qq.com)
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#pragma once

#include <mutex>

#ifdef _WIN32
// 避免 winsock.h 和 winsock2.h 冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// RAII 包装器，用于 SRWLOCK（类似 std::lock_guard）
class SRWLockGuard {
public:
    explicit SRWLockGuard(SRWLOCK& lock) : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~SRWLockGuard() {
        ReleaseSRWLockExclusive(&lock_);
    }
    SRWLockGuard(const SRWLockGuard&) = delete;
    SRWLockGuard& operator=(const SRWLockGuard&) = delete;
private:
    SRWLOCK& lock_;
};

// unique_lock 风格的 SRWLOCK 包装器，支持手动解锁
class SRWUniqueLock {
public:
    explicit SRWUniqueLock(SRWLOCK& lock) : lock_(&lock), owns_(true) {
        AcquireSRWLockExclusive(lock_);
    }
    ~SRWUniqueLock() {
        if (owns_) {
            ReleaseSRWLockExclusive(lock_);
        }
    }
    void unlock() {
        if (owns_) {
            ReleaseSRWLockExclusive(lock_);
            owns_ = false;
        }
    }
    void lock() {
        if (!owns_) {
            AcquireSRWLockExclusive(lock_);
            owns_ = true;
        }
    }
    SRWLOCK* mutex() { return lock_; }
    SRWUniqueLock(const SRWUniqueLock&) = delete;
    SRWUniqueLock& operator=(const SRWUniqueLock&) = delete;
private:
    SRWLOCK* lock_;
    bool owns_;
};

// 条件变量等待（带谓词）
template<typename Predicate>
inline void EmmyCondWait(CONDITION_VARIABLE& cv, SRWUniqueLock& lock, Predicate pred) {
    while (!pred()) {
        SleepConditionVariableSRW(&cv, lock.mutex(), INFINITE, 0);
    }
}

// 条件变量通知
inline void EmmyCondNotifyAll(CONDITION_VARIABLE& cv) {
    WakeAllConditionVariable(&cv);
}

inline void EmmyCondNotifyOne(CONDITION_VARIABLE& cv) {
    WakeConditionVariable(&cv);
}

// 跨平台宏定义
#define EMMY_LOCK_GUARD(mtx) SRWLockGuard _lock_guard_##mtx(mtx)
#define EMMY_UNIQUE_LOCK(mtx) SRWUniqueLock _unique_lock_##mtx(mtx)
#define EMMY_COND_WAIT(cv, lock, pred) EmmyCondWait(cv, lock, pred)
#define EMMY_COND_NOTIFY_ALL(cv) EmmyCondNotifyAll(cv)
#define EMMY_COND_NOTIFY_ONE(cv) EmmyCondNotifyOne(cv)

// 锁类型定义
using EmmyMutex = SRWLOCK;
using EmmyCondVar = CONDITION_VARIABLE;
#define EMMY_MUTEX_INIT SRWLOCK_INIT
#define EMMY_CONDVAR_INIT CONDITION_VARIABLE_INIT

#else
// 非 Windows 平台使用标准库

template<typename Predicate>
inline void EmmyCondWait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, Predicate pred) {
    cv.wait(lock, pred);
}

inline void EmmyCondNotifyAll(std::condition_variable& cv) {
    cv.notify_all();
}

inline void EmmyCondNotifyOne(std::condition_variable& cv) {
    cv.notify_one();
}

#define EMMY_LOCK_GUARD(mtx) std::lock_guard<std::mutex> _lock_guard_##mtx(mtx)
#define EMMY_UNIQUE_LOCK(mtx) std::unique_lock<std::mutex> _unique_lock_##mtx(mtx)
#define EMMY_COND_WAIT(cv, lock, pred) EmmyCondWait(cv, lock, pred)
#define EMMY_COND_NOTIFY_ALL(cv) EmmyCondNotifyAll(cv)
#define EMMY_COND_NOTIFY_ONE(cv) EmmyCondNotifyOne(cv)

using EmmyMutex = std::mutex;
using EmmyCondVar = std::condition_variable;
#define EMMY_MUTEX_INIT
#define EMMY_CONDVAR_INIT

#endif

