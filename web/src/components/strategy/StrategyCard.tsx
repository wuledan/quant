import React from 'react';
import { Card, Tag, Typography, Space } from 'antd';
import {
  ExperimentOutlined,
  ClockCircleOutlined,
} from '@ant-design/icons';
import type { Strategy } from '../../api/strategy';

const { Text } = Typography;

const statusColors: Record<string, string> = {
  draft: 'default',
  active: 'green',
  paused: 'orange',
  deleted: 'red',
};

const statusLabels: Record<string, string> = {
  draft: '草稿',
  active: '运行中',
  paused: '已暂停',
  deleted: '已删除',
};

interface StrategyCardProps {
  strategy: Strategy;
  onClick?: () => void;
}

const StrategyCard: React.FC<StrategyCardProps> = ({ strategy, onClick }) => {
  return (
    <Card
      hoverable={!!onClick}
      onClick={onClick}
      style={{ borderRadius: 8, textAlign: 'left' }}
      styles={{ body: { padding: 16 } }}
    >
      <Space direction="vertical" size="small" style={{ width: '100%' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <Text strong style={{ fontSize: 16, color: 'var(--text-h)' }}>
            <ExperimentOutlined style={{ marginRight: 6 }} />
            {strategy.name}
          </Text>
          <Tag color={statusColors[strategy.status] || 'default'}>
            {statusLabels[strategy.status] || strategy.status}
          </Tag>
        </div>
        <div>
          <Text type="secondary" style={{ fontSize: 13 }}>
            ID: {strategy.id}
          </Text>
        </div>
        <div>
          <Text type="secondary" style={{ fontSize: 12 }}>
            <ClockCircleOutlined style={{ marginRight: 4 }} />
            Updated: {new Date(strategy.updated_at).toLocaleString()}
          </Text>
        </div>
      </Space>
    </Card>
  );
};

export default StrategyCard;