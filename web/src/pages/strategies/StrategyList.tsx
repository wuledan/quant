import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Button, Table, Tag, Space, Typography, Input, Spin, Alert, Empty } from 'antd';
import { PlusOutlined, SearchOutlined, PlayCircleOutlined, EditOutlined, DeleteOutlined } from '@ant-design/icons';
import type { Strategy } from '../../api/strategy';
import { useStrategyStore } from '../../stores/strategyStore';

const { Title } = Typography;

const statusColor: Record<string, string> = {
  running: 'processing',
  stopped: 'default',
  error: 'error',
  completed: 'success',
};

const statusLabel: Record<string, string> = {
  running: '运行中',
  stopped: '已停止',
  error: '异常',
  completed: '已完成',
};

const MOCK_STRATEGIES: Strategy[] = [
  { id: 1, name: '均线交叉策略', type: 'trend', params: { fast: 5, slow: 20 }, status: 'running', created_at: '2026-05-10', updated_at: '2026-05-20' },
  { id: 2, name: '动量选股策略', type: 'momentum', params: { window: 20, top_n: 10 }, status: 'stopped', created_at: '2026-04-15', updated_at: '2026-05-18' },
  { id: 3, name: '均值回归策略', type: 'mean_reversion', params: { window: 30, std: 2 }, status: 'error', created_at: '2026-03-01', updated_at: '2026-05-19' },
  { id: 4, name: 'Alpha因子选股', type: 'alpha', params: { factors: ['momentum', 'value', 'quality'] }, status: 'completed', created_at: '2026-01-10', updated_at: '2026-05-15' },
];

const StrategyList: React.FC = () => {
  const navigate = useNavigate();
  const { strategies, loading, error, fetchStrategies, deleteStrategy, runStrategy } = useStrategyStore();
  const [search, setSearch] = useState('');
  const [localData, setLocalData] = useState<Strategy[]>([]);

  useEffect(() => {
    fetchStrategies().catch(() => {
      setLocalData(MOCK_STRATEGIES);
    });
  }, []);

  const displayData = strategies.length > 0 ? strategies : localData;
  const filtered = search
    ? displayData.filter((s) => s.name.includes(search) || s.type.includes(search))
    : displayData;

  if (loading && displayData.length === 0) {
    return <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}><Spin size="large" tip="加载策略列表..."><div style={{ padding: 50 }} /></Spin></div>;
  }

  if (error && displayData.length === 0) {
    return <Alert message="加载失败" description={error} type="error" showIcon action={<Button onClick={() => window.location.reload()}>重试</Button>} />;
  }

  const columns = [
    { title: 'ID', dataIndex: 'id', key: 'id', width: 60 },
    { title: '策略名称', dataIndex: 'name', key: 'name', render: (text: string, record: Strategy) => <a onClick={() => navigate(`/strategies/${record.id}`)}>{text}</a> },
    { title: '类型', dataIndex: 'type', key: 'type', width: 140, render: (t: string) => <Tag>{t}</Tag> },
    {
      title: '状态', dataIndex: 'status', key: 'status', width: 100,
      render: (s: string) => <Tag color={statusColor[s] ?? 'default'}>{statusLabel[s] ?? s}</Tag>,
    },
    { title: '创建时间', dataIndex: 'created_at', key: 'created_at', width: 120 },
    { title: '更新时间', dataIndex: 'updated_at', key: 'updated_at', width: 120 },
    {
      title: '操作', key: 'actions', width: 200,
      render: (_: unknown, record: Strategy) => (
        <Space>
          <Button size="small" icon={<PlayCircleOutlined />} onClick={() => runStrategy(record.id).catch(() => {})}>运行</Button>
          <Button size="small" icon={<EditOutlined />} onClick={() => navigate(`/strategies/${record.id}`)}>编辑</Button>
          <Button size="small" danger icon={<DeleteOutlined />} onClick={() => deleteStrategy(record.id).catch(() => {})}>删除</Button>
        </Space>
      ),
    },
  ];

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16, marginTop: 0 }}>
        <Title level={4} style={{ margin: 0 }}>策略管理</Title>
        <Button type="primary" icon={<PlusOutlined />} onClick={() => navigate('/strategies/new')}>创建策略</Button>
      </div>

      <Input
        placeholder="搜索策略名称或类型..."
        prefix={<SearchOutlined />}
        value={search}
        onChange={(e) => setSearch(e.target.value)}
        style={{ marginBottom: 16, maxWidth: 320 }}
        allowClear
      />

      {filtered.length === 0 ? (
        <Empty description={search ? '未找到匹配的策略' : '暂无策略，点击上方按钮创建'}>
          {!search && <Button type="primary" onClick={() => navigate('/strategies/new')}>创建策略</Button>}
        </Empty>
      ) : (
        <Table
          dataSource={filtered}
          columns={columns}
          rowKey="id"
          pagination={{ pageSize: 10, showSizeChanger: false }}
          size="middle"
          bordered
        />
      )}
    </div>
  );
};

export default StrategyList;
