#!/usr/bin/env python3
"""A股标的关联提取器：从新闻文本中提取股票代码/名称."""

from __future__ import annotations

import re

# A股代码正则：6位数字 + 市场后缀（可选）
STOCK_CODE_PATTERN = re.compile(r"(\d{6})(?:\.(SH|SZ|BJ))?")

# 常见A股名称（可扩展）
COMMON_STOCK_NAMES: dict[str, str] = {
    "贵州茅台": "600519.SH",
    "中国平安": "601318.SH",
    "招商银行": "600036.SH",
    "宁德时代": "300750.SZ",
    "五粮液": "000858.SZ",
    "比亚迪": "002594.SZ",
    "药明康德": "603259.SH",
    "隆基绿能": "601012.SH",
    "中信证券": "600030.SH",
    "东方财富": "300059.SZ",
    "工商银行": "601398.SH",
    "建设银行": "601939.SH",
    "中国石油": "601857.SH",
    "中国移动": "600941.SH",
    "华为": "",  # 未上市
    "字节跳动": "",  # 未上市
}


class SymbolExtractor:
    """从新闻文本中提取关联标的."""

    def __init__(self, stock_names: dict[str, str] | None = None) -> None:
        """
        Args:
            stock_names: 自定义股票名称映射，合并到默认词典.
        """
        self._names: dict[str, str] = {**COMMON_STOCK_NAMES}
        if stock_names:
            self._names.update(stock_names)

    def extract_codes(self, text: str) -> list[str]:
        """提取新闻文本中的A股股票代码.

        Returns:
            股票代码列表，如 ['600519.SH', '000858.SZ'].
        """
        codes: set[str] = set()
        for match in STOCK_CODE_PATTERN.finditer(text):
            code = match.group(1)
            suffix = match.group(2)
            if suffix:
                codes.add(f"{code}.{suffix}")
            else:
                # 无后缀时根据首位判断市场
                if code.startswith(("6", "9")):
                    codes.add(f"{code}.SH")
                elif code.startswith(("0", "3", "2")):
                    codes.add(f"{code}.SZ")
                elif code.startswith("4"):
                    codes.add(f"{code}.BJ")
        return sorted(codes)

    def extract_names(self, text: str) -> list[str]:
        """提取新闻文本中的股票名称，并返回对应代码.

        Returns:
            股票代码列表.
        """
        found: list[str] = []
        for name, code in self._names.items():
            if name in text and code:
                found.append(code)
        # 去重并保持顺序
        seen: set[str] = set()
        deduped: list[str] = []
        for c in found:
            if c not in seen:
                seen.add(c)
                deduped.append(c)
        return deduped

    def extract_all(self, text: str) -> list[str]:
        """同时使用代码匹配和名称匹配提取标的.

        Returns:
            去重后的股票代码列表.
        """
        codes = set(self.extract_codes(text))
        codes.update(self.extract_names(text))
        return sorted(codes)

    def add_stock_name(self, name: str, code: str) -> None:
        """添加自定义股票名称映射."""
        self._names[name] = code
