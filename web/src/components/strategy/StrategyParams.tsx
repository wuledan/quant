import React from 'react';
import { Button, Input, Space } from 'antd';
import { PlusOutlined, DeleteOutlined } from '@ant-design/icons';

type ParamsRecord = Record<string, unknown>;

interface StrategyParamsProps {
  value?: ParamsRecord;
  onChange?: (value: ParamsRecord) => void;
  readonly?: boolean;
}

const StrategyParams: React.FC<StrategyParamsProps> = ({ value = {}, onChange, readonly = false }) => {
  const entries = Object.entries(value ?? {});

  const handleKeyChange = (oldKey: string, newKey: string) => {
    if (!onChange) return;
    const next: ParamsRecord = {};
    for (const [k, v] of Object.entries(value)) {
      next[k === oldKey ? newKey : k] = v;
    }
    onChange(next);
  };

  const handleValueChange = (key: string, newValue: string) => {
    if (!onChange) return;
    let parsed: unknown = newValue;
    try {
      parsed = JSON.parse(newValue);
    } catch {
      parsed = newValue;
    }
    onChange({ ...value, [key]: parsed });
  };

  const handleAdd = () => {
    if (!onChange) return;
    onChange({ ...value, '': '' });
  };

  const handleRemove = (key: string) => {
    if (!onChange) return;
    const next = { ...value };
    delete next[key];
    onChange(next);
  };

  if (readonly) {
    if (entries.length === 0) {
      return <span style={{ color: 'var(--text)' }}>No parameters</span>;
    }
    return (
      <table style={{ width: '100%', borderCollapse: 'collapse' }}>
        <tbody>
          {entries.map(([key, val]) => (
            <tr key={key}>
              <td style={{
                padding: '4px 8px', fontWeight: 500, color: 'var(--text-h)',
                borderBottom: '1px solid var(--border)', width: '40%',
              }}>{key}</td>
              <td style={{
                padding: '4px 8px', color: 'var(--text)',
                borderBottom: '1px solid var(--border)',
                fontFamily: 'var(--mono)', fontSize: 14,
              }}>{JSON.stringify(val)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    );
  }

  return (
    <div>
      {entries.map(([key, val]) => (
        <Space key={key} style={{ display: 'flex', marginBottom: 8 }} align="start">
          <Input
            style={{ width: 160 }}
            placeholder="Key"
            value={key}
            onChange={(e) => handleKeyChange(key, e.target.value)}
          />
          <Input
            style={{ width: 240 }}
            placeholder="Value (JSON or string)"
            value={typeof val === 'string' ? val : JSON.stringify(val)}
            onChange={(e) => handleValueChange(key, e.target.value)}
          />
          <Button
            icon={<DeleteOutlined />}
            size="small"
            danger
            onClick={() => handleRemove(key)}
          />
        </Space>
      ))}
      <Button
        type="dashed"
        onClick={handleAdd}
        icon={<PlusOutlined />}
        size="small"
      >
        Add Parameter
      </Button>
    </div>
  );
};

export default StrategyParams;
