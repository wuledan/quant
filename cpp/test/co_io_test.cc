// co_io_test.cc — Tests for CoIouring (io_uring coroutine I/O)
#include "cpp/quant/network/co_io.h"
#include "cpp/quant/infra/coroutine.h"
#include "cpp/quant/infra/work_stealing_executor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace quant::network {
namespace {

using namespace quant::infra;

// ── RAII pipe ──
class PipePair {
public:
    PipePair() {
        int fds[2];
        int rc = ::pipe(fds);
        if (rc != 0) throw std::runtime_error("pipe failed");
        read_fd_ = fds[0];
        write_fd_ = fds[1];
    }
    ~PipePair() {
        if (read_fd_ >= 0) ::close(read_fd_);
        if (write_fd_ >= 0) ::close(write_fd_);
    }
    int read_fd() const noexcept { return read_fd_; }
    int write_fd() const noexcept { return write_fd_; }
private:
    int read_fd_{-1};
    int write_fd_{-1};
};

// ═══════════════════════════════════════════════════════════
// CoIouring tests
// ═══════════════════════════════════════════════════════════

TEST(CoIouringTest, CreateAndDestroy) {
    CoIouring ring(64);
    EXPECT_FALSE(ring.is_running());
}

TEST(CoIouringTest, StartAndStop) {
    WorkStealingExecutor executor(2, "coio-test");
    executor.start();

    CoIouring ring(64);
    ring.start(&executor);
    EXPECT_TRUE(ring.is_running());

    ring.stop();
    EXPECT_FALSE(ring.is_running());

    executor.stop();
}

TEST(CoIouringTest, CoReadFromPipe) {
    WorkStealingExecutor executor(2, "coio-test");
    executor.start();
    CoIouring ring(64);
    ring.start(&executor);

    PipePair pipe;
    const std::string test_data = "Hello from io_uring!";

    auto nw = ::write(pipe.write_fd(), test_data.data(), test_data.size());
    ASSERT_EQ(nw, static_cast<ssize_t>(test_data.size()));

    std::string result;

    auto read_task = [&]() -> CoTask<void> {
        char buf[1024];
        auto n = co_await ring.co_read(pipe.read_fd(), buf, sizeof(buf));
        EXPECT_GT(n, 0);
        result.assign(buf, static_cast<size_t>(n));
    };

    blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor), read_task()));
    EXPECT_EQ(result, test_data);

    ring.stop();
    executor.stop();
}

TEST(CoIouringTest, CoWriteToPipe) {
    WorkStealingExecutor executor(2, "coio-test");
    executor.start();
    CoIouring ring(64);
    ring.start(&executor);

    PipePair pipe;
    const std::string test_data = "Write via io_uring!";

    auto write_task = [&]() -> CoTask<void> {
        auto n = co_await ring.co_write(pipe.write_fd(),
                                         test_data.data(),
                                         test_data.size());
        EXPECT_EQ(static_cast<size_t>(n), test_data.size());
    };

    blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor), write_task()));

    char buf[1024];
    ::memset(buf, 0, sizeof(buf));
    auto nr = ::read(pipe.read_fd(), buf, sizeof(buf));
    ASSERT_GT(nr, 0);
    std::string result(buf, static_cast<size_t>(nr));
    EXPECT_EQ(result, test_data);

    ring.stop();
    executor.stop();
}

TEST(CoIouringTest, CoReadWriteFile) {
    WorkStealingExecutor executor(2, "coio-test");
    executor.start();
    CoIouring ring(64);
    ring.start(&executor);

    // Create temp file
    char tmpl[] = "/tmp/co_io_file_XXXXXX";
    int tmp_fd = ::mkstemp(tmpl);
    ASSERT_GE(tmp_fd, 0);
    ::unlink(tmpl);

    const std::string test_data = "File I/O with io_uring!";

    auto write_task = [&]() -> CoTask<void> {
        auto n = co_await ring.co_write(tmp_fd,
                                         test_data.data(),
                                         test_data.size(),
                                         0);
        EXPECT_EQ(static_cast<size_t>(n), test_data.size());
    };

    blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor), write_task()));

    auto read_task = [&]() -> CoTask<std::string> {
        char buf[1024];
        ::memset(buf, 0, sizeof(buf));
        auto n = co_await ring.co_read(tmp_fd, buf, sizeof(buf), 0);
        co_return std::string(buf, static_cast<size_t>(n));
    };

    auto result = blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor), read_task()));
    EXPECT_EQ(result, test_data);

    ::close(tmp_fd);
    ring.stop();
    executor.stop();
}

TEST(CoIouringTest, CoWriteReadSequential) {
    // Single coroutine: write then read on a pipe
    WorkStealingExecutor executor(2, "coio-test");
    executor.start();
    CoIouring ring(64);
    ring.start(&executor);

    PipePair pipe;
    const std::string test_data = "Sequential I/O with io_uring!";

    auto wr_task = [&]() -> CoTask<std::string> {
        // Write first
        auto nw = co_await ring.co_write(pipe.write_fd(),
                                          test_data.data(),
                                          test_data.size());
        EXPECT_EQ(static_cast<size_t>(nw), test_data.size());

        // Then read
        char buf[1024];
        ::memset(buf, 0, sizeof(buf));
        auto nr = co_await ring.co_read(pipe.read_fd(), buf, sizeof(buf));
        co_return std::string(buf, static_cast<size_t>(nr));
    };

    auto result = blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor), wr_task()));
    EXPECT_EQ(result, test_data);

    ring.stop();
    executor.stop();
}

TEST(CoIouringTest, CoConnectAccept) {
    WorkStealingExecutor executor(4, "coio-test");
    executor.start();
    CoIouring ring(64);
    ring.start(&executor);

    int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(listen_fd, 0);
    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    ASSERT_EQ(::bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, 1), 0);

    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_fd, (struct sockaddr*)&addr, &addr_len), 0);
    int port = ntohs(addr.sin_port);

    std::atomic<int> accepted_fd{-1};
    std::atomic<bool> accept_done{false};

    // Accept coroutine
    auto accept_task = [&]() -> CoTask<void> {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int fd = co_await ring.co_accept(
            listen_fd, (struct sockaddr*)&client_addr, &client_len);
        accepted_fd.store(fd, std::memory_order_release);
        accept_done.store(true, std::memory_order_release);
    };

    // Launch accept as background task
    auto scope = std::make_shared<AsyncScope>();
    auto ka = folly::Executor::getKeepAliveToken(executor);
    scope->add(co_withExecutor(ka, accept_task()));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Sync connect
    int conn_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(conn_fd, 0);

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(port);

    int rc = ::connect(conn_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    EXPECT_TRUE(rc == 0 || errno == EINPROGRESS);

    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd{conn_fd, POLLOUT, 0};
        ::poll(&pfd, 1, 5000);
    }
    ::close(conn_fd);

    // Wait for accept
    for (int i = 0; i < 50 && !accept_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(accept_done.load(std::memory_order_acquire));
    int client_fd = accepted_fd.load(std::memory_order_acquire);
    EXPECT_GE(client_fd, 0);
    if (client_fd >= 0) {
        ::close(client_fd);
    }

    // Clean up scope
    blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor),
        scope->joinAsync()));

    ::close(listen_fd);
    ring.stop();
    executor.stop();
}

TEST(CoIouringTest, Stats) {
    WorkStealingExecutor executor(2, "coio-test");
    executor.start();
    CoIouring ring(64);
    ring.start(&executor);

    PipePair pipe;
    const std::string data = "stats";
    auto nw = ::write(pipe.write_fd(), data.data(), data.size());
    EXPECT_GT(nw, 0);

    auto read_task = [&]() -> CoTask<void> {
        char buf[64];
        co_await ring.co_read(pipe.read_fd(), buf, sizeof(buf));
    };
    blockingWait(co_withExecutor(
        folly::Executor::getKeepAliveToken(executor), read_task()));

    auto s = ring.stats();
    EXPECT_GE(s.submissions, 1u);
    EXPECT_GE(s.completions, 1u);

    ring.stop();
    executor.stop();
}

}  // namespace
}  // namespace quant::network
