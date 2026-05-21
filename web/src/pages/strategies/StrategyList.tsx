import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Button, Table, Tag, Space, Typography, Input, Spin, Alert, Empty, Popconfirm, message } from 'antd';
import {
  PlusOutlined, SearchOutlined, PlayCircleOutlined, PauseCircleOutlined,
  DeleteOutlined, BarChartOutlined, CopyOutlined, UploadOutlined,
} from '@ant-design/icons';
import type { Strategy } from '../../api/strategy';
import { useStrategyStore } from '../../stores/strategyStore';

const { Title } = Typography;

const statusColor: Record<string, string> = {
  draft: 'default',
  active: 'processing',
  paused: 'warning',
  deleted: 'error',
};

const statusLabel: Record<string, string> = {
  draft: '草稿',
  active: '运行中',
  paused: '已暂停',
  deleted: '已删除',
};

const StrategyList: React.FC = () => {
  const navigate = useNavigate();
  const {
    strategies, loading, error,
    fetchStrategies, deleteStrategy, activateStrategy, pauseStrategy, cloneStrategy,
  } = useStrategyStore();
  const [search, setSearch] = useState('');

  useEffect(() => {
    fetchStrategies();
  }, [fetchStrategies]);

  const filtered = search
    ? strategies.filter((s) => s.name.includes(search) || s.status.includes(search))
    : strategies;

  const handleActivate = async (id: number) => {
    try {
      await activateStrategy(id);
      message.success('策略已激活');
      fetchStrategies();
    } catch {
      message.error('激活失败');
    }
  };

  const handlePause = async (id: number) => {
    try {
      await pauseStrategy(id);
      message.success('策略已暂停');
      fetchStrategies();
    } catch {
      message.error('暂停失败');
    }
  };

  const handleDelete = async (id: number) => {
    try {
      await deleteStrategy(id);
      message.success('策略已删除');
    } catch {
      message.error('删除失败');
    }
  };

  const handleClone = async (id: number) => {
    try {
      const cloned = await cloneStrategy(id);
      message.success(`策略已克隆，新 ID: ${cloned.id}`);
      fetchStrategies();
    } catch {
      message.error('克隆失败');
    }
  };

  if (loading && strategies.length === 0) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
        <Spin size="large" tip="加载策略列表..."><div style={{ padding: 50 }} /></Spin>
      </div>
    );
  }

  if (error && strategies.length === 0) {
    return (
      <Alert
        message="加载失败"
        description={error}
        type="error"
        showIcon
        action={<Button onClick={() => fetchStrategies()}>重试</Button>}
      />
    );
  }

  const columns = [
    { title: 'ID', dataIndex: 'id', key: 'id', width: 60 },
    {
      title: '策略名称', dataIndex: 'name', key: 'name',
      render: (text: string, record: Strategy) => (
        <a onClick={() => navigate(`/strategies/${record.id}`)}>{text}</a>
      ),
    },
    {
      title: '状态', dataIndex: 'status', key: 'status', width: 100,
      render: (s: string) => <Tag color={statusColor[s] ?? 'default'}>{statusLabel[s] ?? s}</Tag>,
    },
    { title: '创建时间', dataIndex: 'created_at', key: 'created_at', width: 120 },
    { title: '更新时间', dataIndex: 'updated_at', key: 'updated_at', width: 120 },
    {
      title: '操作', key: 'actions', width: 280,
      render: (_: unknown, record: Strategy) => (
        <Space size="small" wrap>
          {record.status === 'draft' || record.status === 'paused' ? (
            <Button size="small" type="primary" icon={<PlayCircleOutlined />} onClick={() => handleActivate(record.id)}>
              激活
            </Button>
          ) : null}
          {record.status === 'active' ? (
            <Button size="small" icon={<PauseCircleOutlined />} onClick={() => handlePause(record.id)}>
              暂停
            </Button>
          ) : null}
          <Button size="small" icon={<BarChartOutlined />} onClick={() => navigate(`/strategies/${record.id}`)}>
            详情
          </Button>
          <Button size="small" icon={<CopyOutlined />} onClick={() => handleClone(record.id)}>
            克隆
          </Button>
          <Popconfirm title="确定删除该策略？" onConfirm={() => handleDelete(record.id)} okText="删除" cancelText="取消">
            <Button size="small" danger icon={<DeleteOutlined />}>删除</Button>
          </Popconfirm>
        </Space>
      ),
    },
  ];

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16, marginTop: 0 }}>
        <Title level={4} style={{ margin: 0 }}>策略管理</Title>
        <Space>
          <Button icon={<UploadOutlined />} onClick={() => navigate('/strategies/upload')}>上传策略</Button>
          <Button type="primary" icon={<PlusOutlined />} onClick={() => navigate('/strategies/new')}>创建策略</Button>
        </Space>
      </div>

      <Input
        placeholder="搜索策略名称或状态..."
        prefix={<SearchOutlined />}
        value={search}
        onChange={(e) => setSearch(e.target.value)}
        style={{ marginBottom: 16, maxWidth: 320 }}
        allowClear
      />

      {filtered.length === 0 ? (
        <Empty description={search ? '未找到匹配的策略' : '暂无策略，点击上方按钮创建'}>
          {!search && <Button type="primary" onClick={() => navigate('/strategies/upload')}>上传策略</Button>}
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