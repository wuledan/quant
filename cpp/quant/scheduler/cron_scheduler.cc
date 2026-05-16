// cron_scheduler.cc — CronScheduler implementation
#include "cpp/quant/scheduler/cron_scheduler.h"

#include <ctime>
#include <sstream>

namespace quant::scheduler {

CronScheduler::CronScheduler(CronSchedulerConfig config)
    : config_(std::move(config)) {}

CronScheduler::~CronScheduler() { stop(); }

uint64_t CronScheduler::add_job(std::string name, std::string cron_expression,
                                  std::function<void()> action) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_job_id_++;
    CronJob job;
    job.id = id;
    job.name = std::move(name);
    job.cron_expression = std::move(cron_expression);
    job.action = std::move(action);
    job.next_run = next_match(job.cron_expression,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    jobs_[id] = std::move(job);
    return id;
}

bool CronScheduler::remove_job(uint64_t job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.erase(job_id) > 0;
}

void CronScheduler::enable_job(uint64_t job_id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(job_id);
    if (it != jobs_.end()) it->second.enabled = enabled;
}

void CronScheduler::start() {
    if (running_.exchange(true)) return;
    scheduler_thread_ = std::make_unique<std::thread>([this]() {
        while (running_.load()) {
            tick();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.tick_interval_ms));
        }
    });
}

void CronScheduler::stop() {
    running_.store(false);
    if (scheduler_thread_ && scheduler_thread_->joinable()) {
        scheduler_thread_->join();
    }
    scheduler_thread_.reset();
}

void CronScheduler::tick() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, job] : jobs_) {
        if (!job.enabled) continue;
        if (now >= job.next_run) {
            try {
                job.action();
            } catch (...) {}
            job.last_run = now;
            job.next_run = next_match(job.cron_expression, now);
        }
    }
}

std::vector<CronJob> CronScheduler::list_jobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CronJob> result;
    for (const auto& [id, job] : jobs_) {
        result.push_back(job);
    }
    return result;
}

// ── Cron expression parsing ──
// Supports: * (every), N (exact), N-M (range), N-M/S (range with step),
//           N/S (start with step), comma-separated list
// Fields: minute(0-59) hour(0-23) dom(1-31) month(1-12) dow(0-6)

namespace {

bool field_matches(const std::string& field, int value, int min_val, int max_val) {
    if (field == "*") return true;

    // Check for comma-separated list
    size_t pos = 0;
    while (pos < field.size()) {
        size_t end = field.find(',', pos);
        if (end == std::string::npos) end = field.size();
        std::string part = field.substr(pos, end - pos);

        // Check for step: N/S or N-M/S
        size_t slash = part.find('/');
        int step = 1;
        if (slash != std::string::npos) {
            step = std::stoi(part.substr(slash + 1));
            part = part.substr(0, slash);
        }

        if (part == "*") {
            if (step > 1 && (value - min_val) % step == 0) return true;
            if (step == 1) return true;
            continue;
        }

        // Check for range: N-M
        size_t dash = part.find('-');
        int lo = min_val, hi = max_val;
        if (dash != std::string::npos) {
            lo = std::stoi(part.substr(0, dash));
            hi = std::stoi(part.substr(dash + 1));
        } else {
            lo = hi = std::stoi(part);
        }

        if (value >= lo && value <= hi) {
            if (step > 1 && (value - lo) % step == 0) return true;
            if (step == 1) return true;
        }

        pos = end + 1;
    }
    return false;
}

int next_in_field(const std::string& field, int current, int min_val, int max_val) {
    for (int v = current; v <= max_val; ++v) {
        if (field_matches(field, v, min_val, max_val)) return v;
    }
    return -1;
}

int days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return days[month - 1];
}

}  // namespace

bool CronScheduler::matches(const std::string& cron_expr, int64_t timestamp) {
    std::tm* tm = std::gmtime(&timestamp);
    if (!tm) return false;

    std::istringstream ss(cron_expr);
    std::string minute_f, hour_f, dom_f, month_f, dow_f;
    ss >> minute_f >> hour_f >> dom_f >> month_f >> dow_f;
    if (ss.fail()) return false;

    return field_matches(minute_f, tm->tm_min, 0, 59) &&
           field_matches(hour_f, tm->tm_hour, 0, 23) &&
           field_matches(dom_f, tm->tm_mday, 1, 31) &&
           field_matches(month_f, tm->tm_mon + 1, 1, 12) &&
           field_matches(dow_f, tm->tm_wday, 0, 6);
}

int64_t CronScheduler::next_match(const std::string& cron_expr, int64_t after_timestamp) {
    std::istringstream ss(cron_expr);
    std::string minute_f, hour_f, dom_f, month_f, dow_f;
    ss >> minute_f >> hour_f >> dom_f >> month_f >> dow_f;
    if (ss.fail()) return -1;

    // Start from the next second after after_timestamp
    int64_t ts = after_timestamp + 1;

    // Try up to 4 years ahead (reasonable bound)
    int64_t max_ts = after_timestamp + 4LL * 365 * 86400;

    while (ts < max_ts) {
        std::time_t t = static_cast<std::time_t>(ts);
        std::tm* tm = std::gmtime(&t);
        if (!tm) break;

        int year = tm->tm_year + 1900;
        int mon = tm->tm_mon + 1;
        int day = tm->tm_mday;
        int hour = tm->tm_hour;
        int min = tm->tm_min;
        int dow = tm->tm_wday;

        // Check month
        int next_mon = next_in_field(month_f, mon, 1, 12);
        if (next_mon != mon) {
            // Advance to next matching month, day 1, 00:00:00
            if (next_mon < 0) {
                year++;
                next_mon = next_in_field(month_f, 1, 1, 12);
                if (next_mon < 0) break;
            }
            std::tm next_tm = {};
            next_tm.tm_year = year - 1900;
            next_tm.tm_mon = next_mon - 1;
            next_tm.tm_mday = 1;
            next_tm.tm_hour = 0;
            next_tm.tm_min = 0;
            next_tm.tm_sec = 0;
            ts = static_cast<int64_t>(timegm(&next_tm));
            continue;
        }

        // Check day-of-month and day-of-week
        int next_dom = next_in_field(dom_f, day, 1, days_in_month(year, mon));
        if (next_dom != day || !field_matches(dow_f, dow, 0, 6)) {
            // Advance to next matching day
            int candidate_day = (next_dom > day) ? next_dom : (day + 1);
            if (candidate_day > days_in_month(year, mon)) {
                // Next month
                candidate_day = 1;
                if (mon == 12) { year++; mon = 1; }
                else { mon++; }
            }
            std::tm next_tm = {};
            next_tm.tm_year = year - 1900;
            next_tm.tm_mon = mon - 1;
            next_tm.tm_mday = candidate_day;
            next_tm.tm_hour = 0;
            next_tm.tm_min = 0;
            next_tm.tm_sec = 0;
            ts = static_cast<int64_t>(timegm(&next_tm));
            continue;
        }

        // Check hour
        int next_hour = next_in_field(hour_f, hour, 0, 23);
        if (next_hour != hour) {
            if (next_hour < 0) {
                std::tm next_tm = {};
                next_tm.tm_year = year - 1900;
                next_tm.tm_mon = mon - 1;
                next_tm.tm_mday = day + 1;
                next_tm.tm_hour = 0;
                ts = static_cast<int64_t>(timegm(&next_tm));
                continue;
            }
            std::tm next_tm = {};
            next_tm.tm_year = year - 1900;
            next_tm.tm_mon = mon - 1;
            next_tm.tm_mday = day;
            next_tm.tm_hour = next_hour;
            next_tm.tm_min = 0;
            next_tm.tm_sec = 0;
            ts = static_cast<int64_t>(timegm(&next_tm));
            continue;
        }

        // Check minute
        int next_min = next_in_field(minute_f, min, 0, 59);
        if (next_min != min) {
            if (next_min < 0) {
                std::tm next_tm = {};
                next_tm.tm_year = year - 1900;
                next_tm.tm_mon = mon - 1;
                next_tm.tm_mday = day;
                next_tm.tm_hour = hour + 1;
                next_tm.tm_min = 0;
                ts = static_cast<int64_t>(timegm(&next_tm));
                continue;
            }
            std::tm next_tm = {};
            next_tm.tm_year = year - 1900;
            next_tm.tm_mon = mon - 1;
            next_tm.tm_mday = day;
            next_tm.tm_hour = hour;
            next_tm.tm_min = next_min;
            next_tm.tm_sec = 0;
            ts = static_cast<int64_t>(timegm(&next_tm));
            continue;
        }

        // All fields match at this timestamp
        return ts;
    }

    return -1;
}

}  // namespace quant::scheduler
