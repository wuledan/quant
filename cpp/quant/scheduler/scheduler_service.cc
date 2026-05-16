// scheduler_service.cc — SchedulerService implementation
#include "cpp/quant/scheduler/scheduler_service.h"

namespace quant::scheduler {

SchedulerService::SchedulerService()
    : graph_(std::make_unique<TaskGraph>())
    , wave_(std::make_unique<WaveScheduler>())
    , cron_(std::make_unique<CronScheduler>()) {}

SchedulerService::~SchedulerService() { stop(); }

WaveExecutionResult SchedulerService::run_graph() {
    return wave_->execute(*graph_);
}

uint64_t SchedulerService::schedule_cron(
    std::string name, std::string cron_expr,
    std::function<void()> action) {
    return cron_->add_job(std::move(name), std::move(cron_expr), std::move(action));
}

void SchedulerService::start() {
    if (started_) return;
    started_ = true;
    cron_->start();
}

void SchedulerService::stop() {
    if (!started_) return;
    started_ = false;
    cron_->stop();
}

}  // namespace quant::scheduler
