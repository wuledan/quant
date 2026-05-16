"""飞书机器人指令处理器."""

from quant_invest.bot.commands.base import CommandHandler
from quant_invest.bot.commands.help_command import HelpCommand
from quant_invest.bot.commands.performance import BacktestCommand, FactorCommand, PerformanceCommand
from quant_invest.bot.commands.portfolio import PortfolioCommand
from quant_invest.bot.commands.strategy_ctrl import StrategyCommand

__all__ = [
    "CommandHandler",
    "PortfolioCommand",
    "PerformanceCommand",
    "BacktestCommand",
    "FactorCommand",
    "StrategyCommand",
    "HelpCommand",
]
