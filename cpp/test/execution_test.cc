// execution_test.cc — Tests for execution engine components
#include "cpp/quant/execution/order.h"
#include "cpp/quant/execution/order_state_machine.h"
#include "cpp/quant/execution/order_manager.h"
#include "cpp/quant/execution/algorithmic_trader.h"
#include "cpp/quant/execution/broker_interface.h"
#include <gtest/gtest.h>
#include <chrono>

namespace quant::execution {
namespace {

// ── OrderStateMachine tests ──

TEST(OrderStateMachine, ValidTransitions) {
    // PendingNew -> New (accepted)
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kPendingNew, OrderStatus::kNew));
    // PendingNew -> Rejected (rejected)
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kPendingNew, OrderStatus::kRejected));
    // New -> PartialFilled
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kNew, OrderStatus::kPartialFilled));
    // New -> Filled
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kNew, OrderStatus::kFilled));
    // New -> PendingCancel
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kNew, OrderStatus::kPendingCancel));
    // New -> Expired
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kNew, OrderStatus::kExpired));
    // PartialFilled -> Filled
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kPartialFilled, OrderStatus::kFilled));
    // PartialFilled -> PendingCancel
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kPartialFilled, OrderStatus::kPendingCancel));
    // PartialFilled -> Cancelled
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kPartialFilled, OrderStatus::kCancelled));
    // PendingCancel -> Cancelled
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kPendingCancel, OrderStatus::kCancelled));
    // Suspended -> New
    EXPECT_TRUE(OrderStateMachine::is_valid_transition(
        OrderStatus::kSuspended, OrderStatus::kNew));
}

TEST(OrderStateMachine, InvalidTransitions) {
    // Terminal states cannot transition
    EXPECT_FALSE(OrderStateMachine::is_valid_transition(
        OrderStatus::kFilled, OrderStatus::kNew));
    EXPECT_FALSE(OrderStateMachine::is_valid_transition(
        OrderStatus::kCancelled, OrderStatus::kNew));
    EXPECT_FALSE(OrderStateMachine::is_valid_transition(
        OrderStatus::kRejected, OrderStatus::kNew));
    // Cannot go from New back to PendingNew
    EXPECT_FALSE(OrderStateMachine::is_valid_transition(
        OrderStatus::kNew, OrderStatus::kPendingNew));
    // Cannot go from Filled to anything
    EXPECT_FALSE(OrderStateMachine::is_valid_transition(
        OrderStatus::kFilled, OrderStatus::kCancelled));
}

TEST(OrderStateMachine, TerminalAndActiveStates) {
    EXPECT_TRUE(OrderStateMachine::is_terminal(OrderStatus::kFilled));
    EXPECT_TRUE(OrderStateMachine::is_terminal(OrderStatus::kCancelled));
    EXPECT_TRUE(OrderStateMachine::is_terminal(OrderStatus::kRejected));
    EXPECT_TRUE(OrderStateMachine::is_terminal(OrderStatus::kExpired));
    EXPECT_FALSE(OrderStateMachine::is_terminal(OrderStatus::kNew));
    EXPECT_FALSE(OrderStateMachine::is_terminal(OrderStatus::kPartialFilled));

    EXPECT_TRUE(OrderStateMachine::is_active(OrderStatus::kNew));
    EXPECT_TRUE(OrderStateMachine::is_active(OrderStatus::kPartialFilled));
    EXPECT_FALSE(OrderStateMachine::is_active(OrderStatus::kFilled));
    EXPECT_FALSE(OrderStateMachine::is_active(OrderStatus::kPendingNew));
}

TEST(OrderStateMachine, CanCancelAndModify) {
    EXPECT_TRUE(OrderStateMachine::can_cancel(OrderStatus::kNew));
    EXPECT_TRUE(OrderStateMachine::can_cancel(OrderStatus::kPartialFilled));
    EXPECT_FALSE(OrderStateMachine::can_cancel(OrderStatus::kFilled));
    EXPECT_FALSE(OrderStateMachine::can_cancel(OrderStatus::kCancelled));

    EXPECT_TRUE(OrderStateMachine::can_modify(OrderStatus::kNew));
    EXPECT_FALSE(OrderStateMachine::can_modify(OrderStatus::kFilled));
    EXPECT_FALSE(OrderStateMachine::can_modify(OrderStatus::kPartialFilled));
}

TEST(OrderStateMachine, ApplyTransitionSuccess) {
    auto result = OrderStateMachine::apply_transition(
        OrderStatus::kPendingNew, OrderStatus::kNew);
    EXPECT_TRUE(result.ok());
}

TEST(OrderStateMachine, ApplyTransitionFailure) {
    auto result = OrderStateMachine::apply_transition(
        OrderStatus::kFilled, OrderStatus::kNew);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error(), nullptr);
}

// ── OrderManager tests ──

TEST(OrderManager, CreateOrder) {
    OrderManager mgr;
    OrderRequest req;
    req.symbol = "600000.SS";
    req.side = OrderSide::kBuy;
    req.type = OrderType::kLimit;
    req.price = 100000;  // 10.00 * 10000
    req.quantity = 1000;

    auto result = mgr.create_order(req);
    EXPECT_TRUE(result.ok());
    EXPECT_GT(result.value(), 0u);

    const Order* order = mgr.find_order(result.value());
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->symbol, "600000.SS");
    EXPECT_EQ(order->side, OrderSide::kBuy);
    EXPECT_EQ(order->status, OrderStatus::kPendingNew);
    EXPECT_EQ(order->quantity, 1000);
}

TEST(OrderManager, CreateOrderValidation) {
    OrderManager mgr;
    OrderRequest req;
    req.symbol = "600000.SS";
    req.quantity = 0;  // invalid

    auto result = mgr.create_order(req);
    EXPECT_FALSE(result.ok());

    req.quantity = 100;
    req.symbol = "";  // empty symbol
    result = mgr.create_order(req);
    EXPECT_FALSE(result.ok());
}

TEST(OrderManager, OrderLifecycle) {
    OrderManager mgr;
    OrderRequest req;
    req.symbol = "000001.SZ";
    req.side = OrderSide::kSell;
    req.type = OrderType::kLimit;
    req.price = 150000;
    req.quantity = 500;

    auto result = mgr.create_order(req);
    ASSERT_TRUE(result.ok());
    OrderId id = result.value();

    // PendingNew -> Rejected
    auto reject_result = mgr.on_order_rejected(id, "insufficient funds");
    EXPECT_TRUE(reject_result.ok());
    EXPECT_EQ(mgr.find_order(id)->status, OrderStatus::kRejected);

    // Create new order and accept
    OrderRequest req2;
    req2.symbol = "000001.SZ";
    req2.side = OrderSide::kBuy;
    req2.price = 150000;
    req2.quantity = 500;
    auto id2 = mgr.create_order(req2);
    ASSERT_TRUE(id2.ok());

    // PendingNew -> New (accepted)
    auto accept_result = mgr.on_order_accepted(id2.value(), "BROKER-12345");
    EXPECT_TRUE(accept_result.ok());
    EXPECT_EQ(mgr.find_order(id2.value())->status, OrderStatus::kNew);
    EXPECT_EQ(mgr.find_order(id2.value())->broker_order_id, "BROKER-12345");
}

TEST(OrderManager, OrderFill) {
    OrderManager mgr;
    OrderRequest req;
    req.symbol = "600519.SS";
    req.side = OrderSide::kBuy;
    req.type = OrderType::kLimit;
    req.price = 1800000;
    req.quantity = 1000;

    auto result = mgr.create_order(req);
    ASSERT_TRUE(result.ok());
    OrderId id = result.value();
    mgr.on_order_accepted(id, "B-001");

    // Partial fill
    auto fill_result = mgr.on_order_fill(id, 300, 1800500);
    EXPECT_TRUE(fill_result.ok());
    const Order* order = mgr.find_order(id);
    EXPECT_EQ(order->status, OrderStatus::kPartialFilled);
    EXPECT_EQ(order->filled_quantity, 300);

    // Full fill
    fill_result = mgr.on_order_fill(id, 700, 1801000);
    EXPECT_TRUE(fill_result.ok());
    order = mgr.find_order(id);
    EXPECT_EQ(order->status, OrderStatus::kFilled);
    EXPECT_EQ(order->filled_quantity, 1000);
}

TEST(OrderManager, FindBySymbolAndStatus) {
    OrderManager mgr;
    OrderRequest req1, req2, req3;
    req1.symbol = "600000.SS"; req1.side = OrderSide::kBuy; req1.quantity = 100;
    req2.symbol = "600000.SS"; req2.side = OrderSide::kSell; req2.quantity = 200;
    req3.symbol = "000001.SZ"; req3.side = OrderSide::kBuy; req3.quantity = 300;

    auto id1 = mgr.create_order(req1);
    auto id2 = mgr.create_order(req2);
    auto id3 = mgr.create_order(req3);

    ASSERT_TRUE(id1.ok());
    ASSERT_TRUE(id2.ok());
    ASSERT_TRUE(id3.ok());

    auto symbol_orders = mgr.find_orders_by_symbol("600000.SS");
    EXPECT_EQ(symbol_orders.size(), 2u);

    auto pending = mgr.find_orders_by_status(OrderStatus::kPendingNew);
    EXPECT_EQ(pending.size(), 3u);

    EXPECT_EQ(mgr.total_order_count(), 3u);
}

TEST(OrderManager, ModifyOrder) {
    OrderManager mgr;
    OrderRequest req;
    req.symbol = "600000.SS";
    req.side = OrderSide::kBuy;
    req.price = 100000;
    req.quantity = 100;
    auto id = mgr.create_order(req);
    ASSERT_TRUE(id.ok());

    mgr.on_order_accepted(id.value(), "B-100");

    OrderModifyRequest mod;
    mod.order_id = id.value();
    mod.new_price = 101000;
    mod.new_quantity = 200;
    auto mod_result = mgr.modify_order(mod);
    EXPECT_TRUE(mod_result.ok());

    const Order* order = mgr.find_order(id.value());
    EXPECT_EQ(order->price, 101000);
    EXPECT_EQ(order->quantity, 200);
}

TEST(OrderManager, CancelOrder) {
    OrderManager mgr;
    OrderRequest req;
    req.symbol = "600000.SS";
    req.side = OrderSide::kBuy;
    req.price = 100000;
    req.quantity = 100;
    auto id = mgr.create_order(req);
    ASSERT_TRUE(id.ok());

    mgr.on_order_accepted(id.value(), "B-200");

    auto cancel_result = mgr.cancel_order(id.value());
    EXPECT_TRUE(cancel_result.ok());
    EXPECT_EQ(mgr.find_order(id.value())->status, OrderStatus::kPendingCancel);
}

TEST(OrderManager, OrderNotFound) {
    OrderManager mgr;
    auto result = mgr.on_order_accepted(99999, "B-404");
    EXPECT_FALSE(result.ok());
}

// ── AlgorithmicTrader tests ──

TEST(AlgoTrader, TwapBasicFlow) {
    OrderManager mgr;
    AlgoOrderConfig config;
    config.symbol = "600000.SS";
    config.side = OrderSide::kBuy;
    config.total_quantity = 1000;
    config.limit_price = 100000;
    config.start_time_ns = 1000000000;
    config.end_time_ns = 2000000000;

    TwapTrader trader(mgr, config, std::chrono::milliseconds(0));
    EXPECT_TRUE(trader.start());
    EXPECT_TRUE(trader.is_running());

    auto stats = trader.stats();
    EXPECT_GT(stats.parent_order_id, 0u);

    trader.stop();
    EXPECT_FALSE(trader.is_running());
}

TEST(AlgoTrader, VwapWithVolumeProfile) {
    OrderManager mgr;
    AlgoOrderConfig config;
    config.symbol = "000001.SZ";
    config.side = OrderSide::kSell;
    config.total_quantity = 1000;
    config.limit_price = 150000;

    VwapTrader trader(mgr, config, std::chrono::milliseconds(0));
    trader.set_volume_profile({0.1, 0.2, 0.3, 0.25, 0.15});
    EXPECT_TRUE(trader.start());
    trader.stop();
}

TEST(AlgoTrader, PovBasicFlow) {
    OrderManager mgr;
    AlgoOrderConfig config;
    config.symbol = "600519.SS";
    config.side = OrderSide::kBuy;
    config.total_quantity = 500;
    config.limit_price = 1800000;

    PovTrader trader(mgr, config, 0.05);
    EXPECT_TRUE(trader.start());
    trader.stop();
}

// ── BrokerInterface tests ──

TEST(BrokerInterface, MockBrokerConnect) {
    MockBroker broker;
    EXPECT_EQ(broker.status(), ConnectionStatus::kDisconnected);

    auto status = broker.connect();
    EXPECT_EQ(status, ConnectionStatus::kConnected);
    EXPECT_EQ(broker.status(), ConnectionStatus::kConnected);

    broker.disconnect();
    EXPECT_EQ(broker.status(), ConnectionStatus::kDisconnected);
}

TEST(BrokerInterface, MockBrokerAuthenticate) {
    MockBroker broker;
    broker.connect();

    EXPECT_TRUE(broker.authenticate("valid_token"));
    EXPECT_EQ(broker.status(), ConnectionStatus::kAuthenticated);

    EXPECT_FALSE(broker.authenticate(""));
}

TEST(BrokerInterface, MockBrokerSubmitOrder) {
    MockBroker broker;
    broker.connect();

    Order order;
    order.order_id = 1;
    order.symbol = "600000.SS";
    order.side = OrderSide::kBuy;
    order.type = OrderType::kLimit;
    order.price = 100000;
    order.quantity = 100;

    EXPECT_TRUE(broker.submit_order(order));
    EXPECT_EQ(broker.submitted().size(), 1u);
    EXPECT_EQ(broker.submitted()[0].symbol, "600000.SS");
}

TEST(BrokerInterface, MockBrokerCancelOrder) {
    MockBroker broker;
    broker.connect();

    EXPECT_TRUE(broker.cancel_order(42));
    EXPECT_EQ(broker.cancelled().size(), 1u);
    EXPECT_EQ(broker.cancelled()[0], 42u);
}

TEST(BrokerInterface, MockBrokerQueryOrders) {
    MockBroker broker;
    broker.connect();

    Order o1, o2;
    o1.symbol = "600000.SS"; o1.side = OrderSide::kBuy; o1.quantity = 100;
    o2.symbol = "000001.SZ"; o2.side = OrderSide::kSell; o2.quantity = 200;

    broker.submit_order(o1);
    broker.submit_order(o2);

    auto result = broker.query_orders("600000.SS");
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].symbol, "600000.SS");
}

// ── Order to_string tests ──

TEST(Order, OrderSideToString) {
    EXPECT_EQ(to_string(OrderSide::kBuy), std::string_view("Buy"));
    EXPECT_EQ(to_string(OrderSide::kSell), std::string_view("Sell"));
}

TEST(Order, OrderTypeToString) {
    EXPECT_EQ(to_string(OrderType::kMarket), std::string_view("Market"));
    EXPECT_EQ(to_string(OrderType::kLimit), std::string_view("Limit"));
    EXPECT_EQ(to_string(OrderType::kStop), std::string_view("Stop"));
}

TEST(Order, OrderStatusToString) {
    EXPECT_EQ(to_string(OrderStatus::kPendingNew), std::string_view("PendingNew"));
    EXPECT_EQ(to_string(OrderStatus::kFilled), std::string_view("Filled"));
    EXPECT_EQ(to_string(OrderStatus::kCancelled), std::string_view("Cancelled"));
}

}  // namespace
}  // namespace quant::execution