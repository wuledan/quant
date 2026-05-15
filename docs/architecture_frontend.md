# 量化投资系统 — 前端与交互架构设计

> 版本：v1.0 | 日期：2026-05-15 | 模块：前端 / 飞书机器人 / API / 系统交互

---

## 目录

1. [前端页面设计](#1-前端页面设计)
2. [飞书机器人交互设计](#2-飞书机器人交互设计)
3. [后端API设计](#3-后端api设计)
4. [系统交互流程图](#4-系统交互流程图)

---

## 1. 前端页面设计

### 1.1 技术选型

| 层级 | 技术 | 选型理由 |
|------|------|----------|
| 框架 | **Next.js 14+ (App Router)** | SSR/SSG 支持、文件路由、API Routes 可复用 |
| 语言 | **TypeScript 5.x** | 类型安全、IDE 提示完善、降低运行时错误 |
| UI 库 | **TailwindCSS + shadcn/ui** | 原子化 CSS、组件可定制、打包体积小 |
| 状态管理 | **Zustand + TanStack Query** | 轻量全局状态 + 服务端缓存分离，避免冗余 |
| 金融图表 | **ECharts 5** | K线图、热力图、仪表盘等金融图表开箱即用 |
| 自定义图表 | **D3.js** | 因子分析散点图、相关性网络图等定制可视化 |
| 实时通信 | **WebSocket (native / socket.io-client)** | 交易信号、风控告警实时推送 |
| HTTP 客户端 | **axios + TanStack Query** | 请求拦截、重试、缓存一体化 |
| 表单 | **React Hook Form + Zod** | 类型安全的表单校验 |
| 国际化 | next-intl（预留） | 未来多语言扩展 |

### 1.2 页面功能清单与交互流程

#### 1.2.1 Dashboard — 概览仪表盘

**路由**：`/dashboard`

| 区域 | 功能 | 数据源 | 交互 |
|------|------|--------|------|
| 收益概览卡 | 总收益率 / 年化 / 最大回撤 / Sharpe | `/api/portfolio/summary` | 点击跳转详情 |
| 收益曲线图 | 基准对比净值曲线 | `/api/portfolio/equity-curve` | 时间范围切换 (1M/3M/1Y/All) |
| 持仓分布 | 扇形图：行业/市值/多空分布 | `/api/portfolio/positions` | 点击扇区下钻 |
| 实时信号流 | 最新交易信号滚动列表 | WebSocket `signal` | 点击查看信号详情 |
| 策略状态面板 | 各策略运行状态/盈亏 | `/api/strategy/list` | 快捷启停开关 |

**交互流程**：
```
用户进入 Dashboard
  → 并行加载 summary / equity-curve / positions / strategy-list
  → WebSocket 订阅 signal/position-update 频道
  → 信号到达 → 列表实时追加 + Toast 通知
  → 用户点击某策略 → 跳转 /strategy/[id]
```

#### 1.2.2 策略管理

**路由**：`/strategy` / `/strategy/[id]` / `/strategy/[id]/tune`

| 子页面 | 功能 | 交互 |
|--------|------|------|
| 策略列表 | 卡片/表格展示所有策略 | 搜索/筛选/排序 |
| 策略详情 | 参数配置、运行日志、绩效指标 | Tab 切换 |
| 参数调优 | 滑块/输入框调参 + 实时回测预览 | 提交 → 触发轻量回测 → 展示对比 |
| 策略对比 | 选择 2-4 策略叠加对比 | 收益曲线/风险指标并列 |

**交互流程**：
```
用户进入 /strategy
  → 加载策略列表
  → 选择策略 → 进入 /strategy/[id]
  → 修改参数 → 前端校验(Zod) → 调用 PUT /api/strategy/[id]/params
  → "快速回测" → POST /api/backtest/quick → 返回迷你回测结果
  → "策略对比" → 选择多策略 → /strategy/compare?ids=A,B
```

#### 1.2.3 回测中心

**路由**：`/backtest` / `/backtest/[id]`

| 子页面 | 功能 | 交互 |
|--------|------|------|
| 回测列表 | 历史回测任务列表 | 状态筛选/时间排序 |
| 新建回测 | 策略选择、参数设定、时间段、滑点/手续费 | 向导式表单 |
| 回测报告 | 收益曲线/回撤曲线/交易明细/绩效指标 | 导出 PDF/CSV |
| 参数优化 | 网格搜索 / 遗传算法参数优化 | 热力图展示结果 |
| 绩效对比 | 多次回测结果并列对比 | 选择对比 → 图表叠加 |

**交互流程**：
```
用户进入 /backtest/new
  → Step1: 选择策略 + 参数
  → Step2: 设定时间段 + 成本模型
  → Step3: 确认提交 → POST /api/backtest
  → 轮询/WS 获取进度 → 完成后跳转报告页
  → 用户可选择 "参数优化" → POST /api/backtest/optimize
```

#### 1.2.4 风控面板

**路由**：`/risk` / `/risk/alerts`

| 子页面 | 功能 | 交互 |
|--------|------|------|
| 实时监控 | 实时回撤/暴露度/VaR仪表盘 | WebSocket 实时刷新 |
| 告警历史 | 告警列表 + 状态筛选 | 标记已处理/导出 |
| 风险指标趋势 | 回撤/波动率/Beta趋势图 | 时间范围选择 |
| 风控规则 | 配置回撤阈值、暴露度限制 | 增删改规则 |

**交互流程**：
```
用户进入 /risk
  → 加载实时指标 + WebSocket 订阅 risk-alert
  → 告警到达 → 仪表盘数值闪红 + Toast + 声音提示
  → 用户点击 "处理" → 标记已读 → 调用 PUT /api/risk/alert/[id]
  → 修改风控规则 → PUT /api/risk/rules → 实时生效推送至引擎
```

#### 1.2.5 数据管理

**路由**：`/data` / `/data/quality`

| 子页面 | 功能 | 交互 |
|--------|------|------|
| 数据源列表 | 配置数据源（tushare/wind/自建） | 增删改 + 连通性测试 |
| 数据质量 | 缺失率/异常值/延迟统计 | 热力图 + 表格 |
| 更新状态 | 各数据源最近同步时间/状态 | 手动触发更新 |

**交互流程**：
```
用户进入 /data
  → 加载数据源列表 + 质量指标
  → 添加数据源 → POST /api/data/source → 测试连通性
  → 手动同步 → POST /api/data/sync → WS 进度推送
  → 查看质量报告 → 下钻到字段级详情
```

#### 1.2.6 因子研究

**路由**：`/factor` / `/factor/[id]`

| 子页面 | 功能 | 交互 |
|--------|------|------|
| 因子列表 | 所有因子概览 | 搜索/分类筛选 |
| 因子IC分析 | IC/IR/分位数收益图 | 时间窗口选择 |
| 分层回测 | 分组收益曲线 | 调整分组数 |
| 因子相关性 | 相关性矩阵热力图 + 聚类树 | 点击节点下钻 |

**交互流程**：
```
用户进入 /factor
  → 加载因子列表 + 最新IC摘要
  → 选择因子 → /factor/[id]
  → 查看IC时序 → 切换窗口
  → "分层回测" → 切换 Tab
  → "因子相关性" → 调整因子子集 → 热力图重新渲染
```

### 1.3 状态管理方案

#### 1.3.1 架构分层

```
┌──────────────────────────────────────────────────┐
│                    UI Components                  │
│  (React 组件，只负责渲染 + 触发 Action)           │
├──────────────────────────────────────────────────┤
│              Zustand Store (客户端状态)            │
│  - 全局 UI 状态：侧边栏展开、主题、通知等          │
│  - WebSocket 连接状态                             │
│  - 临时交互状态：表单草稿、选中策略等               │
├──────────────────────────────────────────────────┤
│           TanStack Query (服务端缓存)              │
│  - 所有 REST 数据的缓存与同步                     │
│  - 自动 stale-while-revalidate 策略               │
│  - 乐观更新 (Optimistic Update)                   │
├──────────────────────────────────────────────────┤
│            WebSocket Manager (实时数据)            │
│  - 按 Channel 订阅/退订                           │
│  - 消息去重 + 序列号校验                           │
│  - 断线重连 + 心跳保活                             │
├──────────────────────────────────────────────────┤
│               API Client (axios)                 │
│  - 请求/响应拦截器                                │
│  - JWT Token 自动附加与刷新                        │
│  - 错误统一处理                                    │
└──────────────────────────────────────────────────┘
```

#### 1.3.2 Zustand Store 设计

```typescript
// stores/uiStore.ts
interface UIStore {
  sidebarOpen: boolean;
  theme: 'light' | 'dark';
  toggleSidebar: () => void;
  setTheme: (theme: 'light' | 'dark') => void;
}

// stores/wsStore.ts
interface WSStore {
  connected: boolean;
  reconnectCount: number;
  subscriptions: Map<string, Set<string>>;  // channel -> handlerIds
  subscribe: (channel: string, handler: (data: any) => void) => string;
  unsubscribe: (channel: string, handlerId: string) => void;
}
```

#### 1.3.3 TanStack Query 缓存策略

| 数据类型 | staleTime | gcTime | refetchInterval |
|----------|-----------|--------|-----------------|
| 策略列表 | 30s | 5min | - |
| 持仓数据 | 10s | 2min | 5s (WS断线时退化为轮询) |
| 回测结果 | ∞ (不可变) | 30min | - |
| 风控指标 | 5s | 2min | 3s |
| 因子IC | 1min | 10min | - |

### 1.4 实时数据推送（WebSocket）

#### 1.4.1 连接管理

```
浏览器
  │
  │  1. WebSocket 连接 wss://api.example.com/ws?token=JWT
  │
  ▼
┌─────────────────────────────────────┐
│          WS Connection Manager       │
│  - 心跳：每 30s ping/pong            │
│  - 断线重连：指数退避 (1s→2s→4s→8s→16s→30s)  │
│  - 认证：连接时携带 JWT，过期自动重连 │
│  - 消息序列号：确保有序，丢包检测       │
└─────────────────────────────────────┘
```

#### 1.4.2 频道订阅协议

```jsonc
// 客户端订阅
{
  "action": "subscribe",
  "channels": ["signal", "position", "risk_alert"]
}

// 服务端推送
{
  "channel": "signal",
  "seq": 100001,
  "timestamp": "2026-05-15T10:30:00.000Z",
  "data": {
    "strategy_id": "strat_001",
    "signal_type": "BUY",
    "symbol": "600519.SH",
    "price": 1680.50,
    "confidence": 0.87
  }
}
```

#### 1.4.3 频道清单

| 频道 | 推送内容 | 频率 | 前端处理 |
|------|----------|------|----------|
| `signal` | 交易信号（买卖） | 事件驱动 | 列表追加 + Toast |
| `position` | 持仓变动 | 事件驱动 | 更新持仓图 |
| `portfolio` | 组合净值/回撤 | 5s | 刷新曲线 |
| `risk_alert` | 风控告警 | 事件驱动 | 闪红 + 声音 |
| `backtest_progress` | 回测进度 | 1s | 进度条更新 |

---

## 2. 飞书机器人交互设计

### 2.1 飞书开放平台对接方案

#### 2.1.1 架构总览

```
┌─────────┐     ┌──────────────────┐     ┌──────────┐     ┌──────────┐
│ 飞书用户 │◄───►│  飞书开放平台      │◄───►│ 后端服务  │◄───►│ C++引擎  │
│ (手机/PC)│     │  (事件/Webhook)  │     │ (Python)  │     │          │
└─────────┘     └──────────────────┘     └──────────┘     └──────────┘
                      │                        │
                      │  ┌──────────────┐      │
                      └─►│ 飞书卡片消息  │◄─────┘
                         │  模板引擎     │
                         └──────────────┘
```

#### 2.1.2 接入配置

| 配置项 | 值 | 说明 |
|--------|---|------|
| 应用类型 | 企业自建应用 | 内部使用 |
|_bot_name | 量化助手 | 机器人名称 |
| 权限范围 | `im:message` / `im:message:send_as_bot` / `im:resource` | 消息收发 |
| 加密方式 | AES-256-CBC | 事件回调验证 |
| 验证方式 | Verifying + Encrypting 双重 | 安全性保障 |

#### 2.1.3 关键流程

```
1. 创建飞书应用 → 获取 App ID / App Secret
2. 配置事件订阅 URL → https://api.example.com/feishu/event
3. 配置卡片回调 URL → https://api.example.com/feishu/card
4. 开启 Bot 能力 → 接收/发送消息
5. 配置权限范围 → 全员可见 / 指定群组
```

### 2.2 事件订阅框架

#### 2.2.1 事件处理流水线

```python
# 后端事件处理架构
class FeishuEventHandler:
    """飞书事件处理器"""

    async def handle_event(self, event: dict) -> None:
        handler = self._route(event["header"]["event_type"])
        await handler(event["event"])

    def _route(self, event_type: str):
        routes = {
            "im.message.receive_v1": self._on_message,
            "im.message.message_read_v1": self._on_read,
            "im.message.reaction.created_v1": self._on_reaction,
        }
        return routes.get(event_type, self._on_unknown)
```

#### 2.2.2 消息处理流程

```
飞书消息到达
  │
  ▼
验证签名 + 解密 (AES)
  │
  ▼
解析事件类型
  │
  ├─ im.message.receive_v1 → 解析用户指令
  │     │
  │     ▼
  │   指令解析器 (CommandParser)
  │     │
  │     ├─ 匹配指令 → 执行对应 handler
  │     ├─ 未匹配 → 返回帮助信息
  │     └─ 参数不足 → 返回参数提示
  │
  ├─ im.message.message_read_v1 → 标记已读 (忽略)
  │
  └─ 其他 → 日志记录
```

### 2.3 指令系统设计

#### 2.3.1 指令格式

```
/指令 [子命令] [参数]

示例：
  /持仓                → 查询当前持仓
  /持仓 策略A          → 查询策略A的持仓
  /收益 近30天          → 查看近30天收益
  /因子 momentum        → 查看动量因子表现
  /策略 启动 策略A      → 启动策略A
  /回测 策略A 2025-01-01 2025-12-31  → 触发回测
```

#### 2.3.2 查询类指令

| 指令 | 子命令/参数 | 说明 | 响应 |
|------|------------|------|------|
| `/持仓` | `[策略名]` | 查看当前持仓 | 持仓卡片：股票/数量/成本/盈亏 |
| `/收益` | `[时间范围]` | 查看收益概况 | 收益卡片：日/周/月/年收益+曲线缩略图 |
| `/因子` | `<因子名>` | 查看因子表现 | 因子卡片：IC/IR/分位收益 |
| `/策略` | `状态 [策略名]` | 策略运行状态 | 状态卡片：运行时长/信号数/PnL |
| `/风控` | - | 当前风控指标 | 风控卡片：回撤/VaR/暴露度 |
| `/信号` | `[数量]` | 最新交易信号 | 信号列表卡片 |
| `/回测` | `结果 <id>` | 查看回测结果 | 报告摘要卡片 |

#### 2.3.3 操作类指令

| 指令 | 参数 | 说明 | 确认机制 |
|------|------|------|----------|
| `/策略 启动` | `<策略名>` | 启动策略 | 二次确认卡片 |
| `/策略 停止` | `<策略名>` | 停止策略 | 二次确认卡片 |
| `/回测 触发` | `<策略> <开始> <结束>` | 触发回测 | 参数确认卡片 |
| `/参数 调整` | `<策略> <参数名>=<值>` | 调整策略参数 | 确认卡片展示变更 |

#### 2.3.4 管理类指令

| 指令 | 参数 | 说明 | 响应 |
|------|------|------|------|
| `/数据 更新` | `<数据源>` | 触发数据同步 | 同步任务卡片+进度 |
| `/风控 设置` | `<规则名> <阈值>` | 调整风控规则 | 确认卡片 |
| `/数据 质量` | `[数据源]` | 查看数据质量报告 | 质量报告卡片 |

### 2.4 信号推送机制

#### 2.4.1 推送通道与优先级

| 推送类型 | 通道 | 优先级 | 频率限制 | 目标 |
|----------|------|--------|----------|------|
| 交易信号 | 个人/群聊 | P1 高 | 无 | 交易员 |
| 风控告警 | 群聊 + @指定人 | P0 紧急 | 去重 5min | 风控组 |
| 日报/周报 | 群聊 | P2 中 | 每日/每周 | 全组 |
| 回测结果 | 个人 | P2 中 | 无 | 触发人 |
| 系统通知 | 管理员群 | P3 低 | 去重 1h | 管理员 |

#### 2.4.2 交易信号推送

```
C++引擎产生信号
  │
  ▼
后端信号处理服务
  │
  ├─ 信号校验（合法性、风控拦截检查）
  │
  ├─ 信号持久化（数据库存储）
  │
  └─ 推送
       ├─ WebSocket → 前端实时展示
       └─ 飞书推送 → 构建卡片消息 → 调用飞书 API
```

**信号推送卡片示例**：

```json
{
  "msg_type": "interactive",
  "card": {
    "header": {
      "title": { "tag": "plain_text", "content": "🔴 买入信号 — 茅台 600519.SH" },
      "template": "green"
    },
    "elements": [
      { "tag": "div", "text": { "tag": "lark_md", "content": "**策略**：动量轮动策略\n**信号价格**：￥1,680.50\n**置信度**：87%\n**建议仓位**：10%" }},
      { "tag": "action", "actions": [
        { "tag": "button", "text": { "tag": "plain_text", "content": "确认执行" }, "type": "primary", "value": { "action": "confirm_signal", "signal_id": "sig_001" }},
        { "tag": "button", "text": { "tag": "plain_text", "content": "忽略" }, "type": "default", "value": { "action": "dismiss_signal", "signal_id": "sig_001" }}
      ]}
    ]
  }
}
```

#### 2.4.3 风控告警推送

```
C++引擎风控模块检测到异常
  │
  ▼
后端风控告警服务
  │
  ├─ 告警分级（P0/P1/P2）
  │
  ├─ 去重检查（同一规则 5min 内不重复推送）
  │
  └─ 推送
       ├─ P0 → 群聊 + @所有人 + 声音
       ├─ P1 → 群聊 + @风控负责人
       └─ P2 → 群聊普通消息
```

**风控告警卡片示例**：

```json
{
  "msg_type": "interactive",
  "card": {
    "header": {
      "title": { "tag": "plain_text", "content": "🚨 回撤超限告警" },
      "template": "red"
    },
    "elements": [
      { "tag": "div", "text": { "tag": "lark_md", "content": "**策略**：Alpha多因子\n**当前回撤**：-12.3%\n**回撤阈值**：-10.0%\n**触发时间**：2026-05-15 10:30:00\n**建议操作**：减仓或暂停策略" }},
      { "tag": "action", "actions": [
        { "tag": "button", "text": { "tag": "plain_text", "content": "暂停策略" }, "type": "danger", "value": { "action": "stop_strategy", "strategy_id": "alpha_001" }},
        { "tag": "button", "text": { "tag": "plain_text", "content": "标记已处理" }, "type": "default", "value": { "action": "ack_alert", "alert_id": "alert_001" }}
      ]}
    ]
  }
}
```

#### 2.4.4 日报/周报自动生成

```
定时任务（每日 18:00 / 每周一 9:00）
  │
  ▼
后端报告生成服务
  │
  ├─ 汇总当日/周数据
  │   ├─ 组合收益、回撤、持仓变动
  │   ├─ 各策略表现
  │   ├─ 交易清单
  │   └─ 风控指标
  │
  ├─ 生成报告卡片
  │
  └─ 推送到指定群聊
```

**日报卡片示例**：

```json
{
  "msg_type": "interactive",
  "card": {
    "header": {
      "title": { "tag": "plain_text", "content": "📊 量化日报 — 2026-05-15" },
      "template": "blue"
    },
    "elements": [
      { "tag": "div", "text": { "tag": "lark_md", "content": "**今日收益**：+1.23%\n**累计净值**：1.3856\n**最大回撤**：-8.72%\n**Sharpe**：1.85" }},
      { "tag": "hr" },
      { "tag": "div", "text": { "tag": "lark_md", "content": "**交易信号**：3 条（买入2 / 卖出1）\n**持仓变动**：+贵州茅台 / -中国平安" }},
      { "tag": "hr" },
      { "tag": "div", "text": { "tag": "lark_md", "content": "**风控状态**：🟢 正常\n**Tomorrow Plan**：关注消费板块轮动信号" }},
      { "tag": "action", "actions": [
        { "tag": "button", "text": { "tag": "plain_text", "content": "查看详情" }, "type": "primary", "url": "https://quant.example.com/dashboard" }
      ]}
    ]
  }
}
```

#### 2.4.5 回测结果推送

**回测完成推送卡片**：

```json
{
  "msg_type": "interactive",
  "card": {
    "header": {
      "title": { "tag": "plain_text", "content": "✅ 回测完成 — 动量策略_2026Q1" },
      "template": "green"
    },
    "elements": [
      { "tag": "div", "text": { "tag": "lark_md", "content": "**回测区间**：2025-01-01 ~ 2025-12-31\n**年化收益**：28.5%\n**最大回撤**：-12.3%\n**Sharpe**：1.92\n**交易次数**：156" }},
      { "tag": "action", "actions": [
        { "tag": "button", "text": { "tag": "plain_text", "content": "查看完整报告" }, "type": "primary", "url": "https://quant.example.com/backtest/bt_001" }
      ]}
    ]
  }
}
```

### 2.5 消息模板设计（飞书卡片消息格式）

#### 2.5.1 模板管理

所有卡片消息使用飞书消息卡片（Interactive Card）格式，后端维护模板库：

```
templates/
├── signal_buy.json          # 买入信号
├── signal_sell.json         # 卖出信号
├── risk_alert_drawdown.json # 回撤告警
├── risk_alert_exposure.json # 暴露度告警
├── daily_report.json        # 日报
├── weekly_report.json       # 周报
├── backtest_result.json     # 回测结果
├── strategy_status.json     # 策略状态
├── position_overview.json   # 持仓概览
└── help_command.json        # 帮助信息
```

#### 2.5.2 模板渲染引擎

```python
class CardRenderer:
    """飞书卡片模板渲染器"""

    def __init__(self, template_dir: str = "templates/"):
        self.templates = self._load_templates(template_dir)

    def render(self, template_name: str, context: dict) -> dict:
        """
        渲染卡片消息
        - 支持 Jinja2 风格变量替换
        - 支持条件逻辑（如根据状态变色）
        - 支持数值格式化（百分比、金额）
        """
        template = self.templates[template_name]
        return self._apply_context(template, context)
```

### 2.6 权限管理

#### 2.6.1 角色定义

| 角色 | 查看数据 | 操作策略 | 风控管理 | 系统管理 |
|------|----------|----------|----------|----------|
| Admin | ✅ | ✅ | ✅ | ✅ |
| Trader | ✅ | ✅ | 🟡 仅查看 | ❌ |
| Analyst | ✅ | 🟡 仅回测 | ❌ | ❌ |
| Viewer | 🟡 部分 | ❌ | ❌ | ❌ |

#### 2.6.2 飞书权限映射

```python
# 基于飞书用户身份的权限映射
FEISHU_ROLE_MAPPING = {
    # 飞书部门/群组 → 系统角色
    "量化交易组": "Trader",
    "量化研究组": "Analyst",
    "风控组": "Trader",  # 风控组成员给予Trader角色，额外开通风控查看
    "管理层": "Viewer",
    "系统管理员": "Admin",
}

# 操作指令权限矩阵
COMMAND_PERMISSIONS = {
    # 指令 → 允许的角色列表
    "/持仓":       ["Admin", "Trader", "Analyst", "Viewer"],
    "/收益":       ["Admin", "Trader", "Analyst", "Viewer"],
    "/因子":       ["Admin", "Trader", "Analyst", "Viewer"],
    "/策略 启动":  ["Admin", "Trader"],
    "/策略 停止":  ["Admin", "Trader"],
    "/回测 触发":  ["Admin", "Trader", "Analyst"],
    "/参数 调整":  ["Admin", "Trader"],
    "/风控 设置":  ["Admin"],
    "/数据 更新":  ["Admin"],
}
```

#### 2.6.3 权限校验流程

```
飞书消息到达 → 解析用户 Open ID
  │
  ▼
查询飞书用户信息 → 获取部门/群组列表
  │
  ▼
映射为系统角色 → 查询权限矩阵
  │
  ├─ 有权限 → 执行指令
  └─ 无权限 → 返回 "您没有执行此操作的权限"
```

---

## 3. 后端API设计

### 3.1 RESTful API端点清单

#### 3.1.1 总体约定

- **基础路径**：`/api/v1`
- **认证方式**：`Authorization: Bearer <JWT_TOKEN>`
- **响应格式**：

```json
{
  "code": 0,
  "message": "success",
  "data": { ... },
  "timestamp": "2026-05-15T10:30:00.000Z"
}
```

- **错误格式**：

```json
{
  "code": 40001,
  "message": "Invalid parameter: start_date must be before end_date",
  "timestamp": "2026-05-15T10:30:00.000Z"
}
```

- **分页格式**：`?page=1&page_size=20`

#### 3.1.2 认证相关

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| POST | `/api/v1/auth/login` | 登录 | `{username, password}` | `{access_token, refresh_token, expires_in}` |
| POST | `/api/v1/auth/refresh` | 刷新Token | `{refresh_token}` | `{access_token, expires_in}` |
| POST | `/api/v1/auth/feishu` | 飞书登录 | `{code}` | `{access_token, refresh_token, user}` |
| GET | `/api/v1/auth/me` | 当前用户信息 | - | `{user_id, username, role, permissions}` |
| POST | `/api/v1/auth/logout` | 登出 | - | `{success: true}` |

#### 3.1.3 策略管理

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/strategies` | 策略列表 | - (query: `?status=running&page=1`) | `{items: Strategy[], total, page}` |
| GET | `/api/v1/strategies/{id}` | 策略详情 | - | `Strategy` |
| POST | `/api/v1/strategies` | 创建策略 | `CreateStrategyDTO` | `Strategy` |
| PUT | `/api/v1/strategies/{id}` | 更新策略 | `UpdateStrategyDTO` | `Strategy` |
| DELETE | `/api/v1/strategies/{id}` | 删除策略 | - | `{success: true}` |
| POST | `/api/v1/strategies/{id}/start` | 启动策略 | - | `{status: "running"}` |
| POST | `/api/v1/strategies/{id}/stop` | 停止策略 | - | `{status: "stopped"}` |
| PUT | `/api/v1/strategies/{id}/params` | 更新策略参数 | `{params: {...}}` | `{params: {...}}` |
| GET | `/api/v1/strategies/{id}/logs` | 策略日志 | - (query: `?level=&page=`) | `{items: LogEntry[], total}` |
| GET | `/api/v1/strategies/{id}/performance` | 策略绩效 | - (query: `?start_date=&end_date=`) | `PerformanceMetrics` |

**DTO 定义**：

```typescript
// Strategy
interface Strategy {
  id: string;
  name: string;
  type: 'momentum' | 'mean_reversion' | 'multi_factor' | 'pair_trading';
  status: 'running' | 'stopped' | 'error';
  params: Record<string, any>;
  created_at: string;
  updated_at: string;
}

// CreateStrategyDTO
interface CreateStrategyDTO {
  name: string;
  type: string;
  params: Record<string, any>;
}

// PerformanceMetrics
interface PerformanceMetrics {
  total_return: number;     // 总收益率
  annual_return: number;    // 年化收益率
  max_drawdown: number;     // 最大回撤
  sharpe_ratio: number;     // Sharpe比率
  win_rate: number;         // 胜率
  profit_loss_ratio: number; // 盈亏比
  total_trades: number;     // 总交易次数
}
```

#### 3.1.4 组合与持仓

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/portfolio/summary` | 组合概览 | - | `PortfolioSummary` |
| GET | `/api/v1/portfolio/equity-curve` | 净值曲线 | - (query: `?start=&end=&benchmark=`) | `{dates: string[], nav: number[], benchmark_nav: number[]}` |
| GET | `/api/v1/portfolio/positions` | 当前持仓 | - | `{positions: Position[]}` |
| GET | `/api/v1/portfolio/positions/{symbol}` | 单只持仓详情 | - | `Position` |
| GET | `/api/v1/portfolio/trades` | 交易记录 | - (query: `?start=&end=&strategy=&page=`) | `{items: Trade[], total}` |

**DTO 定义**：

```typescript
interface PortfolioSummary {
  total_nav: number;
  total_return: number;
  daily_return: number;
  annual_return: number;
  max_drawdown: number;
  sharpe_ratio: number;
  total_assets: number;
  available_cash: number;
  position_count: number;
}

interface Position {
  symbol: string;
  name: string;
  quantity: number;
  avg_cost: number;
  current_price: number;
  market_value: number;
  pnl: number;
  pnl_pct: number;
  weight: number;
  strategy_id: string;
}

interface Trade {
  id: string;
  symbol: string;
  side: 'buy' | 'sell';
  price: number;
  quantity: number;
  amount: number;
  commission: number;
  strategy_id: string;
  signal_id: string;
  executed_at: string;
}
```

#### 3.1.5 回测

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/backtests` | 回测列表 | - (query: `?status=&page=`) | `{items: Backtest[], total}` |
| POST | `/api/v1/backtests` | 创建回测 | `CreateBacktestDTO` | `Backtest` |
| GET | `/api/v1/backtests/{id}` | 回测详情 | - | `BacktestResult` |
| DELETE | `/api/v1/backtests/{id}` | 删除回测 | - | `{success: true}` |
| POST | `/api/v1/backtests/{id}/cancel` | 取消回测 | - | `{status: "cancelled"}` |
| POST | `/api/v1/backtests/compare` | 回测对比 | `{backtest_ids: string[]}` | `BacktestComparison` |
| POST | `/api/v1/backtests/optimize` | 参数优化 | `OptimizeDTO` | `{task_id: string}` |
| GET | `/api/v1/backtests/optimize/{task_id}` | 优化结果 | - | `OptimizeResult` |
| GET | `/api/v1/backtests/{id}/trades` | 回测交易明细 | - (query: `?page=`) | `{items: BacktestTrade[], total}` |

**DTO 定义**：

```typescript
interface CreateBacktestDTO {
  strategy_id: string;
  start_date: string;       // "2025-01-01"
  end_date: string;          // "2025-12-31"
  initial_capital: number;
  params?: Record<string, any>;
  cost_model: {
    commission_rate: number;  // 佣金费率
    slippage: number;         // 滑点 (bps)
    stamp_duty: number;        // 印花税
  };
}

interface BacktestResult {
  id: string;
  strategy_id: string;
  strategy_name: string;
  status: 'pending' | 'running' | 'completed' | 'failed';
  start_date: string;
  end_date: string;
  initial_capital: number;
  final_capital: number;
  performance: PerformanceMetrics;
  equity_curve: { date: string; nav: number }[];
  drawdown_curve: { date: string; drawdown: number }[];
  monthly_returns: { month: string; return: number }[];
}
```

#### 3.1.6 风控

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/risk/overview` | 风控概览 | - | `RiskOverview` |
| GET | `/api/v1/risk/metrics` | 风险指标 | - (query: `?start=&end=`) | `RiskMetrics` |
| GET | `/api/v1/risk/metrics/trend` | 风险指标趋势 | - (query: `?metric=&start=&end=`) | `{dates: string[], values: number[]}` |
| GET | `/api/v1/risk/alerts` | 告警列表 | - (query: `?status=&level=&page=`) | `{items: Alert[], total}` |
| GET | `/api/v1/risk/alerts/{id}` | 告警详情 | - | `Alert` |
| PUT | `/api/v1/risk/alerts/{id}/ack` | 确认告警 | `{note?: string}` | `Alert` |
| GET | `/api/v1/risk/rules` | 风控规则列表 | - | `{items: RiskRule[], total}` |
| POST | `/api/v1/risk/rules` | 创建规则 | `CreateRiskRuleDTO` | `RiskRule` |
| PUT | `/api/v1/risk/rules/{id}` | 更新规则 | `UpdateRiskRuleDTO` | `RiskRule` |
| DELETE | `/api/v1/risk/rules/{id}` | 删除规则 | - | `{success: true}` |

**DTO 定义**：

```typescript
interface RiskOverview {
  current_drawdown: number;
  max_drawdown_limit: number;
  portfolio_var_95: number;     // 95% VaR
  portfolio_var_99: number;     // 99% VaR
  sector_exposure: Record<string, number>;  // 行业暴露度
  active_alert_count: number;
  risk_level: 'low' | 'medium' | 'high' | 'critical';
}

interface Alert {
  id: string;
  rule_id: string;
  rule_name: string;
  level: 'info' | 'warning' | 'critical';
  message: string;
  metric_value: number;
  threshold: number;
  status: 'active' | 'acked' | 'resolved';
  created_at: string;
  acked_at?: string;
  acked_by?: string;
  strategy_id?: string;
}

interface RiskRule {
  id: string;
  name: string;
  type: 'drawdown' | 'exposure' | 'var' | 'custom';
  metric: string;
  threshold: number;
  action: 'notify' | 'reduce_position' | 'stop_strategy';
  enabled: boolean;
}
```

#### 3.1.7 因子研究

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/factors` | 因子列表 | - | `{items: Factor[], total}` |
| GET | `/api/v1/factors/{id}` | 因子详情 | - | `Factor` |
| GET | `/api/v1/factors/{id}/ic` | IC分析 | - (query: `?start=&end=&freq=`) | `ICAnalysis` |
| GET | `/api/v1/factors/{id}/quantile` | 分层回测 | - (query: `?n_groups=&start=&end=`) | `QuantileResult` |
| GET | `/api/v1/factors/correlation` | 因子相关性 | - (query: `?factor_ids=&start=&end=`) | `CorrelationMatrix` |
| POST | `/api/v1/factors/{id}/compute` | 触发因子计算 | `{start_date, end_date}` | `{task_id: string}` |
| GET | `/api/v1/factors/compute/{task_id}` | 计算进度 | - | `{status, progress, result?}` |

**DTO 定义**：

```typescript
interface Factor {
  id: string;
  name: string;
  category: 'value' | 'momentum' | 'quality' | 'volatility' | 'growth' | 'technical';
  description: string;
  source: string;             // 因子来源
  computation_method: string; // 计算方法描述
  last_updated: string;
}

interface ICAnalysis {
  factor_id: string;
  period: string;
  ic_mean: number;            // IC均值
  ic_std: number;             // IC标准差
  ir: number;                 // 信息比率 IC_IC.Std
  ic_series: { date: string; ic: number }[];
  ic_cumulative: { date: string; cumulative_ic: number }[];
}

interface QuantileResult {
  factor_id: string;
  n_groups: number;
  group_returns: { group: number; return: number }[];
  group_nav_series: { date: string; navs: number[] }[];
  long_short nav: { date: string; nav: number }[];
}
```

#### 3.1.8 数据管理

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/data/sources` | 数据源列表 | - | `{items: DataSource[], total}` |
| POST | `/api/v1/data/sources` | 添加数据源 | `CreateDataSourceDTO` | `DataSource` |
| PUT | `/api/v1/data/sources/{id}` | 更新数据源 | `UpdateDataSourceDTO` | `DataSource` |
| DELETE | `/api/v1/data/sources/{id}` | 删除数据源 | - | `{success: true}` |
| POST | `/api/v1/data/sources/{id}/test` | 测试连通性 | - | `{connected: boolean, latency_ms: number}` |
| POST | `/api/v1/data/sync` | 触发数据同步 | `{source_id, data_type, start_date?, end_date?}` | `{task_id: string}` |
| GET | `/api/v1/data/sync/{task_id}` | 同步进度 | - | `{status, progress, result?}` |
| GET | `/api/v1/data/quality` | 数据质量报告 | - (query: `?source=&start=&end=`) | `DataQualityReport` |

#### 3.1.9 信号

| 方法 | 端点 | 说明 | 请求体 | 响应体 |
|------|------|------|--------|--------|
| GET | `/api/v1/signals` | 信号列表 | - (query: `?strategy=&side=&start=&end=&page=`) | `{items: Signal[], total}` |
| GET | `/api/v1/signals/{id}` | 信号详情 | - | `Signal` |
| POST | `/api/v1/signals/{id}/confirm` | 确认信号 | `{note?: string}` | `Signal` |
| POST | `/api/v1/signals/{id}/dismiss` | 忽略信号 | `{reason?: string}` | `Signal` |

### 3.2 WebSocket端点设计

#### 3.2.1 连接端点

```
wss://api.example.com/api/v1/ws?token=<JWT_TOKEN>
```

#### 3.2.2 消息协议

**客户端→服务端**：

```jsonc
// 订阅频道
{
  "type": "subscribe",
  "channels": ["signal", "position", "portfolio", "risk_alert", "backtest_progress"],
  "request_id": "req_001"  // 可选，用于匹配响应
}

// 取消订阅
{
  "type": "unsubscribe",
  "channels": ["backtest_progress"],
  "request_id": "req_002"
}

// 心跳
{
  "type": "ping"
}
```

**服务端→客户端**：

```jsonc
// 订阅确认
{
  "type": "subscribed",
  "channels": ["signal", "position"],
  "request_id": "req_001"
}

// 频道消息
{
  "type": "message",
  "channel": "signal",
  "seq": 100001,
  "timestamp": "2026-05-15T10:30:00.000Z",
  "data": { ... }
}

// 心跳响应
{
  "type": "pong"
}

// 错误
{
  "type": "error",
  "code": 40001,
  "message": "Invalid channel name"
}
```

#### 3.2.3 频道消息格式

**signal 频道**：

```json
{
  "channel": "signal",
  "data": {
    "signal_id": "sig_001",
    "strategy_id": "strat_001",
    "strategy_name": "动量轮动",
    "signal_type": "BUY",
    "symbol": "600519.SH",
    "symbol_name": "贵州茅台",
    "price": 1680.50,
    "quantity": 100,
    "confidence": 0.87,
    "reason": "动量信号触发, 5日涨幅3.2%",
    "timestamp": "2026-05-15T10:30:00.000Z"
  }
}
```

**position 频道**：

```json
{
  "channel": "position",
  "data": {
    "action": "update",  // "add" | "update" | "remove"
    "symbol": "600519.SH",
    "quantity": 100,
    "avg_cost": 1650.00,
    "current_price": 1680.50,
    "pnl": 3050.00,
    "pnl_pct": 0.0185
  }
}
```

**portfolio 频道**：

```json
{
  "channel": "portfolio",
  "data": {
    "total_nav": 10368000.00,
    "daily_return": 0.0123,
    "total_return": 0.0368,
    "current_drawdown": -0.0087,
    "position_count": 15,
    "timestamp": "2026-05-15T10:30:00.000Z"
  }
}
```

**risk_alert 频道**：

```json
{
  "channel": "risk_alert",
  "data": {
    "alert_id": "alt_001",
    "level": "critical",
    "rule_name": "回撤超限",
    "message": "当前回撤-12.3%已超过阈值-10.0%",
    "metric_value": -0.123,
    "threshold": -0.10,
    "strategy_id": "alpha_001",
    "actions": ["stop_strategy"]
  }
}
```

**backtest_progress 频道**：

```json
{
  "channel": "backtest_progress",
  "data": {
    "task_id": "bt_001",
    "status": "running",
    "progress": 0.68,
    "current_step": "模拟交易日 170/250",
    "estimated_remaining": "00:02:30"
  }
}
```

#### 3.2.4 断线重连与消息补发

```
1. 客户端断线后重连，携带 last_seq 参数
   wss://api.example.com/api/v1/ws?token=JWT&last_seq=100001

2. 服务端检查 last_seq
   ├─ 差距 < 1000 条 → 补发缺失消息
   └─ 差距 >= 1000 条 → 发送快照 + 增量

3. 正常消息持续推送（序列号递增）
```

### 3.3 认证方案

#### 3.3.1 JWT Token 认证

```
┌──────────┐                    ┌──────────┐                    ┌──────────┐
│  前端     │                    │  后端     │                    │  数据库   │
│/飞书      │                    │  服务     │                    │          │
└────┬─────┘                    └────┬─────┘                    └────┬─────┘
     │                                │                               │
     │ POST /auth/login               │                               │
     │ {username, password}           │                               │
     │ ───────────────────────────────►│                               │
     │                                │ 查询用户                       │
     │                                │ ───────────────────────────────►│
     │                                │◄─────────────────────────────── │
     │                                │                               │
     │                                │ 生成 JWT                      │
     │                                │ access_token  (15min)         │
     │                                │ refresh_token (7days)         │
     │                                │                               │
     │ {access_token, refresh_token}  │                               │
     │◄───────────────────────────────│                               │
     │                                │                               │
     │ GET /api/v1/strategies         │                               │
     │ Authorization: Bearer <token>  │                               │
     │ ───────────────────────────────►│                               │
     │                                │ 验证 JWT                      │
     │                                │ 提取 user_id, role             │
     │                                │ 权限校验                       │
     │                                │ ───────────────────────────────►│
     │ {data: [...]}                  │                               │
     │◄───────────────────────────────│                               │
```

#### 3.3.2 飞书 OAuth 认证

```
┌──────────┐                    ┌──────────┐                    ┌──────────┐
│  前端     │                    │  后端     │                    │  飞书     │
└────┬─────┘                    └────┬─────┘                    └────┬─────┘
     │                                │                               │
     │ 1. 重定向到飞书授权页            │                               │
     │ ──────────────────────────────►│                               │
     │                                │                               │
     │◄───────────────────────────────│                               │
     │ (重定向URL)                     │                               │
     │                                │                               │
     │ 2. 用户在飞书授权                │                               │
     │ ────────────────────────────────────────────────────────────────►│
     │                                │                               │
     │◄────────────────────────────────────────────────────────────────│
     │ (redirect_uri?code=xxx)        │                               │
     │                                │                               │
     │ 3. POST /auth/feishu           │                               │
     │ {code: "xxx"}                  │                               │
     │ ───────────────────────────────►│                               │
     │                                │                               │
     │                                │ 4. 用 code 换取 user_token      │
     │                                │ ───────────────────────────────►│
     │                                │                               │
     │                                │ 5. 获取用户信息                  │
     │                                │ ───────────────────────────────►│
     │                                │ {open_id, name, department}    │
     │                                │◄─────────────────────────────── │
     │                                │                               │
     │                                │ 6. 映射角色 / 生成 JWT          │
     │ {access_token, refresh_token}  │                               │
     │◄───────────────────────────────│                               │
```

#### 3.3.3 Token 设计

```typescript
// Access Token Payload
interface AccessTokenPayload {
  sub: string;        // user_id
  username: string;
  role: 'admin' | 'trader' | 'analyst' | 'viewer';
  permissions: string[];
  iat: number;        // issued at
  exp: number;        // expiration (15min)
}

// Refresh Token Payload
interface RefreshTokenPayload {
  sub: string;        // user_id
  type: 'refresh';
  iat: number;
  exp: number;        // expiration (7days)
}
```

#### 3.3.4 双端认证整合

```
JWT 流程（前端 Web）
  → 登录 → JWT access_token + refresh_token
  → 请求头 Authorization: Bearer <access_token>
  → Token 过期 → 自动 refresh

飞书流程（飞书 Bot）
  → 事件到达 → 验证飞书签名 + 解密
  → 解析用户 Open ID → 映射为内部 user
  → 操作类指令 → 生成短期 operation_token (5min)
  → 确认操作 → 验证 operation_token

WebSocket
  → 连接时携带 JWT token
  → wss://...?token=<access_token>
  → Token 过期 → 关闭连接 → 客户端 refresh → 重连
```

### 3.4 API版本管理策略

#### 3.4.1 版本规则

- **URL路径版本**：`/api/v1/...`、`/api/v2/...`
- **版本升级原则**：
  - 兼容变更（新增字段、新增端点）：小版本，不升级路径版本
  - 破坏变更（删字段、改语义、改格式）：升级路径版本
- **废弃策略**：旧版本保留 6 个月，响应头添加 `Sunset` 和 `Deprecation` 标记

```http
HTTP/1.1 200 OK
Deprecation: true
Sunset: Sat, 01 Nov 2026 00:00:00 GMT
Link: </api/v2/strategies>; rel="successor-version"
```

#### 3.4.2 请求/响应版本协商

```http
# 请求指定版本（可选，默认使用 URL 版本）
GET /api/v1/strategies
Accept: application/vnd.quant.v1+json

# 响应标注版本
Content-Type: application/vnd.quant.v1+json
X-API-Version: 1.2.0
```

---

## 4. 系统交互流程图

### 4.1 用户→前端→后端→C++引擎的完整数据流

```
┌─────────┐      ┌─────────┐      ┌──────────────┐      ┌──────────┐      ┌──────────┐
│  用户    │      │  前端    │      │  后端服务      │      │  消息队列  │      │  C++引擎 │
│ (浏览器) │      │ (Next.js)│     │  (Python)     │      │ (Redis)  │      │          │
└────┬────┘      └────┬────┘      └──────┬───────┘      └────┬─────┘      └────┬─────┘
     │                 │                  │                   │                  │
     │ 1. 操作请求      │                  │                   │                  │
     │ (如：启动策略)   │                  │                   │                  │
     │────────────────►│                  │                   │                  │
     │                 │                  │                   │                  │
     │                 │ 2. HTTP/WS 请求   │                   │                  │
     │                 │─────────────────►│                   │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 3. 认证 + 权限校验  │                  │
     │                 │                  │ (JWT验证/角色校验) │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 4. 参数校验+持久化  │                  │
     │                 │                  │ (写入数据库)       │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 5. 发布指令        │                  │
     │                 │                  │──────────────────►│                  │
     │                 │                  │   Redis Pub/Sub   │                  │
     │                 │                  │   channel:        │                  │
     │                 │                  │   cmd:strategy:start                  │
     │                 │                  │                   │                  │
     │                 │                  │                   │ 6. 消费指令       │
     │                 │                  │                   │─────────────────►│
     │                 │                  │                   │                  │
     │                 │                  │                   │                  │ 7. 执行
     │                 │                  │                   │                  │ (启动策略)
     │                 │                  │                   │                  │
     │                 │                  │                   │  8. 状态反馈      │
     │                 │                  │                   │◄─────────────────│
     │                 │                  │                   │  Redis Pub/Sub   │
     │                 │                  │                   │  channel:        │
     │                 │                  │                   │  status:strategy │
     │                 │                  │                   │                  │
     │                 │                  │ 9. 消费状态更新    │                   │
     │                 │                  │◄──────────────────│                  │
     │                 │                  │                   │                  │
     │                 │                  │ 10. 更新数据库     │                   │
     │                 │                  │ + 推送 WebSocket   │                  │
     │                 │                  │                   │                  │
     │                 │ 11. WS 推送状态   │                   │                  │
     │                 │◄─────────────────│                   │                  │
     │                 │                  │                   │                  │
     │ 12. 界面更新     │                  │                   │                  │
     │◄────────────────│                  │                   │                  │
     │ "策略已启动"     │                  │                   │                  │
```

### 4.2 飞书机器人→后端→C++引擎的指令流

```
┌─────────┐      ┌─────────┐      ┌──────────────┐      ┌──────────┐      ┌──────────┐
│  飞书    │      │  飞书    │      │  后端服务      │      │  消息队列  │      │  C++引擎 │
│  用户    │      │  开放平台 │     │  (Python)     │      │ (Redis)  │      │          │
└────┬────┘      └────┬────┘      └──────┬───────┘      └────┬─────┘      └────┬─────┘
     │                 │                  │                   │                  │
     │ 1. 发送指令      │                  │                   │                  │
     │ "/持仓"         │                  │                   │                  │
     │────────────────►│                  │                   │                  │
     │                 │                  │                   │                  │
     │                 │ 2. 事件回调       │                   │                  │
     │                 │─────────────────►│                   │                  │
     │                 │ (im.message.recv) │                   │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 3. 验证飞书签名    │                  │
     │                 │                  │ + 解密             │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 4. 解析指令        │                  │
     │                 │                  │ /持仓 → 查询类     │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 5. 权限校验        │                  │
     │                 │                  │ (Open ID → 角色)   │                  │
     │                 │                  │                   │                  │
     │                 │                  │  6a. 查询类指令     │                  │
     │                 │                  │  → 查数据库/缓存   │                  │
     │                 │                  │                     │                  │
     │                 │                  │  6b. 操作类指令     │                  │
     │                 │                  │  → 发送确认卡片     │                  │
     │                 │                  │                     │                  │
     │                 │ 7a. 查询结果卡片  │                   │                  │
     │                 │◄─────────────────│                   │                  │
     │                 │                  │                   │                  │
     │ 8a. 展示卡片     │                  │                   │                  │
     │◄────────────────│                  │                   │                  │
     │                 │                  │                   │                  │
     │                 │                  │                   │                  │
     │  === 操作类指令继续 ===             │                   │                  │
     │                 │                  │                   │                  │
     │ 9. 用户点击"确认"│                  │                   │                  │
     │────────────────►│                  │                   │                  │
     │                 │                  │                   │                  │
     │                 │ 10. 卡片回调      │                   │                  │
     │                 │─────────────────►│                   │                  │
     │                 │                  │                   │                  │
     │                 │                  │ 11. 验证 operation_token            │
     │                 │                  │                   │                  │
     │                 │                  │ 12. 发布指令到队列  │                  │
     │                 │                  │──────────────────►│                  │
     │                 │                  │                   │                  │
     │                 │                  │                   │ 13. 消费指令       │
     │                 │                  │                   │─────────────────►│
     │                 │                  │                   │                  │
     │                 │                  │                   │                  │ 14. 执行
     │                 │                  │                   │                  │
     │                 │                  │                   │ 15. 状态反馈      │
     │                 │                  │                   │◄─────────────────│
     │                 │                  │                   │                  │
     │                 │                  │ 16. 消费状态       │                   │
     │                 │                  │◄──────────────────│                  │
     │                 │                  │                   │                  │
     │                 │                  │ 17. 发送结果卡片   │                   │
     │                 │                  │──────────────────►│                  │
     │                 │                  │                   │                  │
     │ 18. 展示结果    │                  │                   │                  │
     │◄────────────────│                  │                   │                  │
     │ "策略A已停止"   │                  │                   │                  │
```

### 4.3 C++引擎→后端→前端/飞书的信号推送流

```
┌──────────┐      ┌──────────┐      ┌──────────────┐      ┌─────────┐      ┌─────────┐
│  C++引擎  │      │  消息队列  │      │  后端服务      │      │  前端    │      │  飞书    │
│          │      │ (Redis)   │      │  (Python)     │      │ (Next.js)│      │  开放平台 │
└────┬─────┘      └────┬─────┘      └──────┬───────┘      └────┬────┘      └────┬────┘
     │                 │                  │                   │                │
     │ 1. 产生交易信号  │                  │                   │                │
     │ (引擎内部逻辑)  │                  │                   │                │
     │                 │                  │                   │                │
     │ 2. 发布信号      │                  │                   │                │
     │────────────────►│                  │                   │                │
     │ Redis Pub/Sub   │                  │                   │                │
     │ channel: signal │                  │                   │                │
     │                 │                  │                   │                │
     │                 │ 3. 消费信号       │                   │                │
     │                 │─────────────────►│                   │                │
     │                 │                  │                   │                │
     │                 │                  │ 4. 信号处理流水线   │                │
     │                 │                  │                   │                │
     │                 │                  │ ┌─────────────────────────────────────┐
     │                 │                  │ │ 4a. 信号校验（风控规则匹配）          │
     │                 │                  │ │ 4b. 信号持久化（数据库）              │
     │                 │                  │ │ 4c. 信号分级（P0/P1/P2）             │
     │                 │                  │ │ 4d. 推送目标路由（谁该收到）          │
     │                 │                  │ └─────────────────────────────────────┘
     │                 │                  │                   │                │
     │                 │                  │ 5a. WebSocket 推送  │                │
     │                 │                  │──────────────────►│                │
     │                 │                  │   channel: signal │                │
     │                 │                  │                   │                │
     │                 │                  │ 5b. 飞书推送       │                │
     │                 │                  │───────────────────┼───────────────►│
     │                 │                  │   构建卡片消息     │                │
     │                 │                  │                   │                │
     │                 │                  │                   │                │ 6. 推送到
     │                 │                  │                   │                │ 用户/群
     │                 │                  │                   │                │
     │                 │                  │                   │ 7. 前端更新     │
     │                 │                  │                   │ 信号列表        │
     │                 │                  │                   │ + Toast通知     │
     │                 │                  │                   │                │
     │                 │                  │                   │                │
     │ ==== 风控告警推送流（类似） ===     │                   │                │
     │                 │                  │                   │                │
     │ 8. 风控模块检测  │                  │                   │                │
     │ 回撤超限        │                  │                   │                │
     │                 │                  │                   │                │
     │ 9. 发布告警      │                  │                   │                │
     │────────────────►│                  │                   │                │
     │ channel:        │                  │                   │                │
     │ risk_alert      │                  │                   │                │
     │                 │                  │                   │                │
     │                 │ 10. 消费告警      │                   │                │
     │                 │─────────────────►│                   │                │
     │                 │                  │                   │                │
     │                 │                  │ 11. 告警处理       │                │
     │                 │                  │ (去重/分级/路由)   │                │
     │                 │                  │                   │                │
     │                 │                  │ 12a. WS推送告警    │                │
     │                 │                  │──────────────────►│                │
     │                 │                  │                   │                │
     │                 │                  │ 12b. 飞书推送告警  │                │
     │                 │                  │───────────────────┼───────────────►│
     │                 │                  │ (P0: @所有人)     │                │
     │                 │                  │ (P1: @负责人)    │                │
     │                 │                  │                   │                │
     │                 │                  │                   │ 13. 前端弹窗+声音│
     │                 │                  │                   │ + 仪表盘闪红   │
     │                 │                  │                   │                │
     │                 │                  │                   │                │ 14. 飞书
     │                 │                  │                   │                │ 群消息+@
     │                 │                  │                   │                │
```

### 4.4 完整系统交互全景图

```
                                    ┌─────────────────────────────────────────┐
                                    │            用户交互层                     │
                                    │                                         │
                                    │  ┌───────────┐    ┌───────────────────┐  │
                                    │  │  Web 前端  │    │   飞书机器人       │  │
                                    │  │ (Next.js)  │    │   (Bot)           │  │
                                    │  └─────┬─────┘    └────────┬──────────┘  │
                                    │        │                    │             │
                                    │   HTTP/WS            飞书API/回调          │
                                    │        │                    │             │
                                    └────────┼────────────────────┼─────────────┘
                                             │                    │
                                    ┌────────▼────────────────────▼─────────────┐
                                    │              API 网关层                     │
                                    │                                            │
                                    │   ┌────────────┐    ┌──────────────────┐   │
                                    │   │  JWT 认证   │    │  飞书签名验证     │   │
                                    │   │  + 权限校验  │    │  + 事件路由       │   │
                                    │   └────────────┘    └──────────────────┘   │
                                    │                                            │
                                    │   ┌────────────────────────────────────┐   │
                                    │   │         限流 / 熔断 / 日志          │   │
                                    │   └────────────────────────────────────┘   │
                                    └────────────────────┬───────────────────────┘
                                                         │
                                    ┌────────────────────▼───────────────────────┐
                                    │            业务服务层 (Python)                │
                                    │                                            │
                                    │  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
                                    │  │ 策略服务  │ │ 回测服务  │ │ 风控服务   │   │
                                    │  └──────────┘ └──────────┘ └──────────┘   │
                                    │  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
                                    │  │ 因子服务  │ │ 数据服务  │ │ 信号服务   │   │
                                    │  └──────────┘ └──────────┘ └──────────┘   │
                                    │  ┌──────────┐ ┌──────────┐               │
                                    │  │ 组合服务  │ │ 飞书服务  │               │
                                    │  └──────────┘ └──────────┘               │
                                    │                                            │
                                    │  ┌────────────────────────────────────┐   │
                                    │  │      WebSocket 推送管理器           │   │
                                    │  └────────────────────────────────────┘   │
                                    └────────────────────┬───────────────────────┘
                                                         │
                                              Redis Pub/Sub
                                                         │
                                    ┌────────────────────▼───────────────────────┐
                                    │          核心引擎层 (C++)                    │
                                    │                                            │
                                    │  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
                                    │  │ 行情引擎  │ │ 策略引擎  │ │ 风控引擎   │   │
                                    │  └──────────┘ └──────────┘ └──────────┘   │
                                    │  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
                                    │  │ 执行引擎  │ │ 因子引擎  │ │ 回测引擎   │   │
                                    │  └──────────┘ └──────────┘ └──────────┘   │
                                    │  ┌──────────┐ ┌──────────┐               │
                                    │  │ 风控网关  │ │ 调度器    │               │
                                    │  └──────────┘ └──────────┘               │
                                    └────────────────────┬───────────────────────┘
                                                         │
                                    ┌────────────────────▼───────────────────────┐
                                    │              数据层                          │
                                    │                                            │
                                    │  ┌──────────┐ ┌──────────┐ ┌──────────┐   │
                                    │  │  PostgreSQL│ │  Redis   │  │  行情数据  │   │
                                    │  │  (业务库) │ │ (缓存/MQ) │  │  (TSDB)  │   │
                                    │  └──────────┘ └──────────┘ └──────────┘   │
                                    │  ┌──────────┐ ┌──────────┐               │
                                    │  │  MinIO    │ │  ClickHouse│              │
                                    │  │  (文件)   │ │  (分析库) │               │
                                    │  └──────────┘ └──────────┘               │
                                    └────────────────────────────────────────────┘
```

---

## 附录

### A. 错误码定义

| 错误码范围 | 分类 | 示例 |
|-----------|------|------|
| 0 | 成功 | `code: 0` |
| 10001-19999 | 认证错误 | 10001=Token过期, 10002=权限不足 |
| 20001-29999 | 参数错误 | 20001=缺少必填字段, 20002=参数格式错误 |
| 30001-39999 | 业务错误 | 30001=策略不存在, 30002=策略已运行 |
| 40001-49999 | 系统错误 | 40001=内部错误, 40002=服务不可用 |
| 50001-59999 | 引擎错误 | 50001=引擎超时, 50002=回测失败 |

### B. 飞书卡片消息模板规范

- 所有卡片使用飞书消息卡片 (Interactive Card) v2 格式
- 颜色规范：`green`=买入/正常, `red`=卖出/告警, `blue`=信息, `orange`=警告
- 操作按钮需携带 `value` 字段用于回调处理
- 卡片宽度适配飞书 PC 端和移动端

### C. WebSocket 频道权限

| 频道 | Admin | Trader | Analyst | Viewer |
|------|-------|--------|---------|--------|
| signal | ✅ | ✅ | ✅ | ✅ |
| position | ✅ | ✅ | ✅ | 🟡 脱敏 |
| portfolio | ✅ | ✅ | ✅ | 🟡 脱敏 |
| risk_alert | ✅ | ✅ | 🟡 查看 | ❌ |
| backtest_progress | ✅ | ✅ | ✅ | ✅ |