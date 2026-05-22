// coroutine.h -- Folly-based coroutine primitives for quant system
//
// Provides CoTask<T>, CoBaton, CoMutex, and other type aliases
// wrapping folly::coro equivalents. CoBaton is implemented as AffinityBaton
// (executor-routed, thread-affine replacement for folly::coro::Baton).
#pragma once

#include <folly/coro/Task.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/CurrentExecutor.h>
#include "affinity_baton.h"
#include "affinity_mutex.h"
#include <folly/coro/BlockingWait.h>
#include <folly/futures/Future.h>

namespace quant::infra {

// ── Coroutine task type ──
template<typename T = void>
using CoTask = folly::coro::Task<T>;

// ── Coroutine synchronization ──
using CoBaton = AffinityBaton;

using CoMutex = AffinityMutex;
// CoSemaphore removed in Folly v2025; use BoundedQueue/UnboundedQueue instead if needed
using AsyncScope = folly::coro::AsyncScope;

// ── Coroutine utilities ──
using folly::coro::collectAll;
using folly::coro::collectAllRange;
using folly::coro::sleep;
using folly::coro::co_withExecutor;
using folly::coro::co_current_executor;
using folly::coro::blockingWait;

}  // namespace quant::infra
