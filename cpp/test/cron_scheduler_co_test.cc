// cron_scheduler_co_test.cc — Tests for CronScheduler coroutine lifecycle
#include "cron_scheduler.h"
#include "timer_scheduler.h"
#include "work_stealing_executor.h"
#include "coroutine.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <folly/coro/BlockingWait.h>

using namespace quant::scheduler;
using namespace quant::infra;

static WorkStealingExecutor make_executor(size_t workers = 2) {
    return WorkStealingExecutor(workers, "cron_co_test");
}

TEST(CronSchedulerCoTest, StartAsyncTickLoop) {
    auto executor = make_executor();
    executor.start();

    TimerScheduler timer(executor);
    timer.start();

    CronScheduler cron;
    std::atomic<int> tick_count{0};

    cron.add_job("test", "* * * * *", [&] { tick_count++; });

    folly::CancellationSource cancel_source;
    auto cron_task = cron.start_async(timer, executor, cancel_source.getToken());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cancel_source.requestCancellation();
    blockingWait(std::move(cron_task));

    EXPECT_TRUE(cron.is_running());

    cron.stop();
    timer.stop();
    executor.stop();
}

TEST(CronSchedulerCoTest, SyncApiStillWorks) {
    CronScheduler cron;
    std::atomic<int> count{0};

    auto id = cron.add_job("test", "* * * * *", [&] { count++; });
    EXPECT_GT(id, 0u);

    auto jobs = cron.list_jobs();
    EXPECT_EQ(jobs.size(), 1u);
    EXPECT_EQ(jobs[0].name, "test");

    EXPECT_TRUE(cron.remove_job(id));
    EXPECT_EQ(cron.list_jobs().size(), 0u);
}

TEST(CronSchedulerCoTest, CronExpressionParsing) {
    int64_t ts = 1700000000;
    EXPECT_TRUE(CronScheduler::matches("* * * * *", ts));
    EXPECT_FALSE(CronScheduler::matches("30 * * * *", 1700000000));

    int64_t next = CronScheduler::next_match("30 * * * *", 1700000000);
    EXPECT_GT(next, 1700000000);
}
