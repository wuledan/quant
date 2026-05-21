import React, { useEffect, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import {
  Card, Descriptions, Tag, Button, Spin, Alert, Empty, Typography, Space, Divider, message, Modal, Popconfirm,
} from 'antd';
import {
  ArrowLeftOutlined, PlayCircleOutlined, PauseCircleOutlined,
  DeleteOutlined, BarChartOutlined, CopyOutlined,
} from '@ant-design/icons';
import type { Strategy, BacktestResult } from '../../api/strategy';
import { useStrategyStore } from '../../stores/strategyStore';
import BacktestChart from './BacktestChart';

const { Title, Text } = Typography;

const statusColor: Record<string, string> = {
  draft: 'default', active: 'processing', paused: 'warning', deleted: 'error',
};
const statusLabel: Record<string, string> = {
  draft: '草稿', active: '运行中', paused: '已暂停', deleted: '已删除',
};

const StrategyDetail: React.FC = () => {
  const { id } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const strategyId = Number(id ?? 0);

  const {
    currentStrategy, loading, error,
    fetchStrategy, deleteStrategy, activateStrategy, pauseStrategy, triggerBacktest, cloneStrategy,
  } = useStrategyStore();

  const [backtestResult, setBacktestResult] = useState<BacktestResult | null>(null);
  const [backtestLoading, setBacktestLoading] = useState(false);

  useEffect(() => {
    if (strategyId > 0) {
      fetchStrategy(strategyId);
    }
  }, [strategyId, fetchStrategy]);

  const handleDelete = () => {
    Modal.confirm({
      title: '确认删除',
      content: `确定要删除策略「${currentStrategy?.name}」吗？`,
      okText: '删除', okType: 'danger', cancelText: '取消',
      onOk: async () => {
        try {
          await deleteStrategy(strategyId);
          message.success('策略已删除');
          navigate('/strategies');
        } catch {
          message.error('删除失败');
        }
      },
    });
  };

  const handleActivate = async () => {
    try {
      await activateStrategy(strategyId);
      message.success('策略已激活');
      fetchStrategy(strategyId);
    } catch {
      message.error('激活失败');
    }
  };

  const handlePause = async () => {
    try {
      await pauseStrategy(strategyId);
      message.success('策略已暂停');
      fetchStrategy(strategyId);
    } catch {
      message.error('暂停失败');
    }
  };

  const handleBacktest = async () => {
    setBacktestLoading(true);
    try {
      const result = await triggerBacktest(strategyId);
      setBacktestResult(result);
      message.success('回测完成');
    } catch {
      message.error('回测失败');
    } finally {
      setBacktestLoading(false);
    }
  };

  const handleClone = async () => {
    try {
      const cloned = await cloneStrategy(strategyId);
      message.success(`策略已克隆，新 ID: ${cloned.id}`);
      navigate(`/strategies/${cloned.id}`);
    } catch {
      message.error('克隆失败');
    }
  };

  if (loading && !currentStrategy) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
        <Spin size="large" tip="加载策略详情..."><div style={{ padding: 50 }} /></Spin>
      </div>
    );
  }

  if (error) {
    return (
      <Alert
        message="加载失败"
        description={error}
        type="error"
        showIcon
        action={<Button onClick={() => navigate('/strategies')}>返回列表</Button>}
      />
    );
  }

  if (!currentStrategy) {
    return <Empty description="未找到策略"><Button onClick={() => navigate('/strategies')}>返回列表</Button></Empty>;
  }

  const strategy: Strategy = currentStrategy;

  return (
    <div style={{ maxWidth: 900 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 16 }}>
        <Button icon={<ArrowLeftOutlined />} shape="circle" size="small" onClick={() => navigate('/strategies')} />
        <Title level={4} style={{ margin: 0 }}>策略详情</Title>
      </div>

      <Card>
        <Descriptions bordered size="small" column={2}>
          <Descriptions.Item label="策略ID"><Text code>{strategy.id}</Text></Descriptions.Item>
          <Descriptions.Item label="状态">
            <Tag color={statusColor[strategy.status]}>{statusLabel[strategy.status]}</Tag>
          </Descriptions.Item>
          <Descriptions.Item label="策略名称">{strategy.name}</Descriptions.Item>
          <Descriptions.Item label="Graph路径">
            {strategy.graph_path ?? <Text type="secondary">—</Text>}
          </Descriptions.Item>
          <Descriptions.Item label="创建时间">{strategy.created_at}</Descriptions.Item>
          <Descriptions.Item label="更新时间">{strategy.updated_at}</Descriptions.Item>
        </Descriptions>

        <Divider />

        <Title level={5}>策略参数</Title>
        <pre style={{ background: '#f5f5f5', padding: 12, borderRadius: 4, overflow: 'auto' }}>
          {JSON.stringify(strategy.params, null, 2)}
        </pre>

        <Divider />

        <Space wrap>
          {strategy.status === 'draft' || strategy.status === 'paused' ? (
            <Button type="primary" icon={<PlayCircleOutlined />} onClick={handleActivate}>激活</Button>
          ) : null}
          {strategy.status === 'active' ? (
            <Button icon={<PauseCircleOutlined />} onClick={handlePause}>暂停</Button>
          ) : null}
          <Button
            icon={<BarChartOutlined />}
            loading={backtestLoading}
            onClick={handleBacktest}
          >
            运行回测
          </Button>
          <Button icon={<CopyOutlined />} onClick={handleClone}>克隆</Button>
          <Popconfirm title="确定删除该策略？" onConfirm={handleDelete} okText="删除" cancelText="取消">
            <Button danger icon={<DeleteOutlined />}>删除</Button>
          </Popconfirm>
        </Space>
      </Card>

      {/* Backtest results */}
      {backtestResult && (
        <Card title="回测结果" style={{ marginTop: 16 }}>
          <BacktestChart result={backtestResult} />
        </Card>
      )}
    </div>
  );
};

export default StrategyDetail;