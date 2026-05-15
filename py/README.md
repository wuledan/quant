# A股量化投资系统 - Python策略研究层

Python层作为策略研究、数据采集、回测验证、ML/AI管道和交互接口的完整解决方案，与C++/Rust高性能层紧密协作。

## 快速开始

```bash
# 安装 uv（如未安装）
curl -LsSf https://astral.sh/uv/install.sh | sh

# 创建虚拟环境并安装依赖
cd py
uv venv --python 3.11
source .venv/bin/activate
uv pip install -e ".[dev]"

# 代码质量检查
ruff format .
ruff check . --fix
mypy src/quant_invest

# 运行测试
pytest

# 启动API服务
uvicorn quant_invest.api.app:app --reload
```

## 项目结构

```
py/
├── pyproject.toml           # 项目配置 & 依赖
├── src/quant_invest/        # 主包
│   ├── config/              # 全局配置
│   ├── data/                # 数据采集模块
│   ├── backtest/            # 回测框架
│   ├── strategy/            # 策略研究框架
│   ├── ml/                  # ML/AI管道
│   ├── api/                 # API服务层
│   └── bot/                 # 飞书机器人
├── tests/                   # 测试
└── scripts/                 # 脚本
```