import React from 'react';
import { Select, Space, Typography } from 'antd';
import { useMarketStore } from '../../stores/marketStore';

const { Text } = Typography;

const SymbolSelector: React.FC = () => {
  const symbols = useMarketStore((s) => s.symbols);
  const currentSymbol = useMarketStore((s) => s.currentSymbol);
  const loadingSymbols = useMarketStore((s) => s.loadingSymbols);
  const symbolsError = useMarketStore((s) => s.symbolsError);
  const selectSymbol = useMarketStore((s) => s.selectSymbol);
  const fetchKline = useMarketStore((s) => s.fetchKline);
  const fetchDepth = useMarketStore((s) => s.fetchDepth);

  const handleChange = (value: string) => {
    selectSymbol(value);
    fetchKline(value);
    fetchDepth(value);
  };

  const options = symbols.map((sym) => ({
    value: sym.symbol,
    label: sym.name ? `${sym.symbol} - ${sym.name}` : sym.symbol,
  }));

  return (
    <Space>
      <Text strong style={{ fontSize: 14 }}>
        交易标的
      </Text>
      <Select
        showSearch
        value={currentSymbol || undefined}
        placeholder="选择标的"
        loading={loadingSymbols}
        status={symbolsError ? 'error' : undefined}
        onChange={handleChange}
        options={options}
        filterOption={(input, option) =>
          (option?.value ?? '').toLowerCase().includes(input.toLowerCase()) ||
          (option?.label ?? '').toString().toLowerCase().includes(input.toLowerCase())
        }
        style={{ width: 240 }}
        notFoundContent={
          symbolsError ? `加载失败: ${symbolsError}` : '暂无数据'
        }
      />
    </Space>
  );
};

export default SymbolSelector;
