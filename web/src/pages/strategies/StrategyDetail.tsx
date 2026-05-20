import React, { useEffect, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { Card, Descriptions, Tag, Button, Spin, Alert, Empty, Typography, Space, Divider, message, Modal } from 'antd';
import { ArrowLeftOutlined, PlayCircleOutlined, EditOutlined, DeleteOutlined, BarChartOutlined } from '@ant-design/icons';
import type { Strategy } from '../../api/strategy';

const { Title, Text, Paragraph } = Typography;

const statusColor: Record<string, string> = {
  running: 'processing', stopped: 'default', error: 'error', completed: 'success',
};
const statusLabel: Record<string, string> = {
  running: '运行中', stopped: '已停止', error: '异常', completed: '已完成',
};

const MOCK_STRATEGY: Strategy = {
  id: 1, name: '均线交叉策略', type: 'trend',
  params: { fast: 5, slow: 20, stop_loss: 0.05, position_size: 0.2 },
  status: 'running', created_at: '2026-05-10', updated_at: '2026-05-20',
};

const StrategyDetail: React.FC = () => {
  const { id } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [strategy, setStrategy] = useState<Strategy | null>(null);

  useEffect(() => {
    setLoading(true);
    setTimeout(() => {
      setStrategy({ ...MOCK_STRATEGY, id: Number(id ?? 1) });
      setLoading(false);
    }, 300);
  }, [id]);

  const handleDelete = () => {
    Modal.confirm({
      title: '确认删除',
      content: `确定要删除策略「${strategy?.name}」吗？`,
      okText: '删除', okType: 'danger', cancelText: '取消',
      onOk: () => { message.success('策略已删除'); navigate('/strategies'); },
    });
  };

  const handleRun = () => {
    message.loading({ content: '正在提交回测任务...', key: 'run' });
    setTimeout(() => {
      message.success({ content: '回测任务已提交', key: 'run' });
    }, 1500);
  };

  if (loading) {
    return <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}><Spin size="large" tip="加载策略详情..."><div style={{ padding: 50 }} /></Spin></div>;
  }

  if (error) {
    return <Alert message="加载失败" description={error} type="error" showIcon action={<Button onClick={() => navigate('/strategies')}>返回列表</Button>} />;
  }

  if (!strategy) {
    return <Empty description="未找到策略"><Button onClick={() => navigate('/strategies')}>返回列表</Button></Empty>;
  }

  return (
    <div style={{ maxWidth: 800 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 16 }}>
        <Button icon={<ArrowLeftOutlined />} shape="circle" size="small" onClick={() => navigate('/strategies')} />
        <Title level={4} style={{ margin: 0 }}>策略详情</Title>
      </div>

      <Card>
        <Descriptions bordered size="small" column={2}>
          <Descriptions.Item label="策略ID"><Text code>{strategy.id}</Text></Descriptions.Item>
          <Descriptions.Item label="状态"><Tag color={statusColor[strategy.status]}>{statusLabel[strategy.status]}</Tag></Descriptions.Item>
          <Descriptions.Item label="策略名称">{strategy.name}</Descriptions.Item>
          <Descriptions.Item label="类型"><Tag>{strategy.type}</Tag></Descriptions.Item>
          <Descriptions.Item label="创建时间">{strategy.created_at}</Descriptions.Item>
          <Descriptions.Item label="更新时间">{strategy.updated_at}</Descriptions.Item>
        </Descriptions>

        <Divider />

        <Title level={5}>策略参数</Title>
        <pre style={{ background: '#f5f5f5', padding: 12, borderRadius: 4 }}>{JSON.stringify(strategy.params, null, 2)}</pre>

        <Divider />

        <Space>
          <Button type="primary" icon={<PlayCircleOutlined />} onClick={handleRun}>运行回测</Button>
          <Button icon={<BarChartOutlined />} onClick={() => navigate('/backtest')}>查看回测结果</Button>
          <Button icon={<EditOutlined />}>编辑策略</Button>
          <Button danger icon={<DeleteOutlined />} onClick={handleDelete}>删除策略</Button>
        </Space>
      </Card>
    </div>
  );
};

export default StrategyDetail;