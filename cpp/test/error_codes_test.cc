// error_codes_test.cc — Tests for error code system
#include "cpp/quant/infra/error_codes.h"
#include <gtest/gtest.h>
#include <system_error>

namespace quant::infra {
namespace {

TEST(ErrorCodeTest, BasicErrorCode) {
    auto ec = make_error_code(ErrorCode::InvalidArgument);
    EXPECT_EQ(ec.value(), static_cast<int>(ErrorCode::InvalidArgument));
    EXPECT_EQ(ec.category().name(), std::string_view("quant"));
}

TEST(ErrorCodeTest, ErrorCategoryMessage) {
    auto ec = make_error_code(ErrorCode::ThreadPoolStopped);
    EXPECT_EQ(ec.message(), "Thread pool is stopped");
}

TEST(ErrorCodeTest, OKIsZero) {
    EXPECT_EQ(static_cast<int>(ErrorCode::OK), 0);
}

TEST(ErrorNodeTest, BasicConstruction) {
    auto node = make_error(ErrorCode::InvalidArgument, "invalid arg");
    EXPECT_EQ(node->code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(node->message(), "invalid arg");
    EXPECT_EQ(node->cause(), nullptr);
}

TEST(ErrorNodeTest, ErrorChain) {
    auto cause = make_error(ErrorCode::DataNotFound, "data not found");
    auto root = chain_error(ErrorCode::StrategyRuntimeError,
                            "strategy failed",
                            std::move(cause));
    EXPECT_EQ(root->code(), ErrorCode::StrategyRuntimeError);
    EXPECT_NE(root->cause(), nullptr);
    EXPECT_EQ(root->cause()->code(), ErrorCode::DataNotFound);
}

TEST(ErrorNodeTest, ToString) {
    auto cause = make_error(ErrorCode::DataNotFound, "data not found");
    auto root = chain_error(ErrorCode::StrategyRuntimeError,
                            "strategy failed",
                            std::move(cause));
    std::string str = root->to_string();
    EXPECT_TRUE(str.find("Strategy runtime error") != std::string::npos);
    EXPECT_TRUE(str.find("Data not found") != std::string::npos);
    EXPECT_TRUE(str.find("Caused by") != std::string::npos);
}

TEST(ResultTest, Success) {
    Result<int> r(42);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, Failure) {
    Result<int> r(ErrorCode::DataNotFound, "not found");
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_NE(r.error(), nullptr);
}

TEST(ResultTest, VoidSuccess) {
    Result<void> r;
    EXPECT_TRUE(r.ok());
}

TEST(ResultTest, VoidFailure) {
    Result<void> r(ErrorCode::Unknown, "error");
    EXPECT_FALSE(r.ok());
}

TEST(ResultTest, Map) {
    Result<int> r(42);
    auto r2 = r.map([](int v) { return v * 2; });
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(r2.value(), 84);
}

TEST(ResultTest, MapOnError) {
    Result<int> r(ErrorCode::Unknown, "error");
    auto r2 = r.map([](int v) { return v * 2; });
    EXPECT_FALSE(r2.ok());
}

TEST(ResultTest, AndThen) {
    Result<int> r(42);
    auto r2 = r.and_then([](int v) -> Result<std::string> {
        return std::to_string(v);
    });
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(r2.value(), "42");
}

TEST(QuantExceptionTest, ThrowAndCatch) {
    try {
        throw QuantException(ErrorCode::InvalidArgument, "bad input");
    } catch (const QuantException& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("Invalid argument") != std::string::npos);
        EXPECT_TRUE(msg.find("bad input") != std::string::npos);
    }
}

}  // namespace
}  // namespace quant::infra
