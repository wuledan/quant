import React, { useEffect, useState, useCallback } from 'react';
import dayjs from 'dayjs';
import { useNavigate } from 'react-router-dom';
import { Card, Table, Button, Typography, Tag, Space, Form, Select, InputNumber, DatePicker, message, Spin, Progress } from 'antd';
import { PlayCircleOutlined, BarChartOutlined, ReloadOutlined } from '@ant-design/icons';

const { Title, Text } = Typography;
const { RangePicker } = DatePicker;

const API_BASE = '/api/v1';

interface StrategyItem {
  id: string;
  name: string;
  type: string;
}

interface BacktestTask {
  task_id: string;
  status: string;
  progress: number;
  message: string;
  strategy_id?: string;
}

const statusColor: Record<string, string> = {
  pending: 'default', running: 'processing', completed: 'success', failed: 'error',
};
const statusLabel: Record<string, string> = {
  pending: '等待中', running: '运行中', completed: '已完成', failed: '失败',
};

const Backtest: React.FC = () => {
  const navigate = useNavigate();
  const [form] = Form.useForm();
  const [submitting, setSubmitting] = useState(false);
  const [strategies, setStrategies] = useState<StrategyItem[]>([]);
  const [tasks, setTasks] = useState<BacktestTask[]>([]);
  const [loading] = useState(false);
  const [pollingIds, setPollingIds] = useState<Set<string>>(new Set());

  const fetchStrategies = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/strategy/list`);
      const data = await res.json();
      setStrategies(data.strategies || []);
    } catch {
      setStrategies([
        { id: 'strat-001', name: '均线交叉策略', type: 'ma_cross' },
        { id: 'strat-002', name: '动量选股策略', type: 'momentum' },
      ]);
    }
  }, []);

  const fetchTasks = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/backtest/list`);
      const data = await res.json();
      setTasks((data.backtests || []).reverse());
    } catch {
      // keep existing tasks on error
    }
  }, []);

  useEffect(() => {
    fetchStrategies();
    fetchTasks();
  }, [fetchStrategies, fetchTasks]);

  // Poll running tasks
  useEffect(() => {
    if (pollingIds.size === 0) return;
    const timer = setInterval(async () => {
      const stillRunning = new Set<string>();
      for (const tid of pollingIds) {
        try {
          const res = await fetch(`${API_BASE}/backtest/${tid}/status`);
          const s = await res.json();
          setTasks(prev => prev.map(t => t.task_id === tid ? { ...t, ...s } : t));
          if (s.status === 'running' || s.status === 'pending') {
            stillRunning.add(tid);
          } else if (s.status === 'completed') {
            message.success(`回测 ${tid} 已完成`);
          } else if (s.status === 'failed') {
            message.error(`回测 ${tid} 失败: ${s.message}`);
          }
        } catch {
          stillRunning.add(tid);
        }
      }
      setPollingIds(stillRunning);
      if (stillRunning.size === 0) fetchTasks();
    }, 3000);
    return () => clearInterval(timer);
  }, [pollingIds, fetchTasks]);

  const handleRun = async (values: Record<string, unknown>) => {
    const dateRange = values.dateRange as unknown as [dayjs.Dayjs, dayjs.Dayjs];
    if (!dateRange || dateRange.length < 2) {
      message.error('请选择回测日期范围');
      return;
    }

    setSubmitting(true);
    try {
      const stratId = values.strategy as string;
      const res = await fetch(`${API_BASE}/backtest/run`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          strategy_id: stratId,
          symbols: ['000300.SH'],
          start_date: dateRange[0].format('YYYY-MM-DD'),
          end_date: dateRange[1].format('YYYY-MM-DD'),
          initial_cash: values.initial_capital || 1000000,
        }),
      });
      const data = await res.json();
      if (data.backtest_id) {
        message.success('回测任务已提交');
        const newTask: BacktestTask = {
          task_id: data.backtest_id,
          status: 'pending',
          progress: 0,
          message: '已排队',
          strategy_id: stratId,
        };
        setTasks(prev => [newTask, ...prev]);
        setPollingIds(prev => new Set(prev).add(data.backtest_id));
        form.resetFields();
      } else {
        message.error(data.detail || '提交失败');
      }
    } catch (e) {
      message.error('网络错误');
    } finally {
      setSubmitting(false);
    }
  };

  const columns = [
    {
      title: '任务ID', dataIndex: 'task_id', key: 'task_id', width: 140,
      render: (id: string) => <Text code>{id}</Text>,
    },
    {
      title: '策略', dataIndex: 'strategy_id', key: 'strategy_id', width: 120,
      render: (id: string) => {
        const s = strategies.find(s => s.id === id);
        return s?.name ?? id ?? '-';
      },
    },
    {
      title: '状态', dataIndex: 'status', key: 'status', width: 120,
      render: (s: string) => <Tag color={statusColor[s] || 'default'}>{statusLabel[s] || s}</Tag>,
    },
    {
      title: '进度', dataIndex: 'progress', key: 'progress', width: 120,
      render: (p: number, record: BacktestTask) => {
        if (record.status === 'completed') return <Tag color="success">100%</Tag>;
        if (record.status === 'failed') return <Tag color="error">-</Tag>;
        return <Progress percent={Math.round(p)} size="small" />;
      },
    },
    {
      title: '信息', dataIndex: 'message', key: 'message', ellipsis: true,
    },
    {
      title: '操作', key: 'actions', width: 100,
      render: (_: unknown, record: BacktestTask) => (
        <Button
          size="small"
          type="link"
          icon={<BarChartOutlined />}
          disabled={record.status !== 'completed'}
          onClick={() => navigate(`/backtest/${record.task_id}/result`)}
        >
          查看结果
        </Button>
      ),
    },
  ];

  return (
    <div>
      <Title level={4} style={{ marginTop: 0 }}>回测中心</Title>

      <Card title="新建回测任务" style={{ marginBottom: 24 }}>
        <Form
          form={form}
          layout="inline"
          onFinish={handleRun}
          initialValues={{ strategy: undefined, initial_capital: 1000000 }}
          style={{ flexWrap: 'wrap', gap: 8 }}
        >
          <Form.Item name="strategy" label="策略" rules={[{ required: true, message: '请选择策略' }]}>
            <Select
              style={{ width: 200 }}
              placeholder="选择策略"
              showSearch
              optionFilterProp="label"
              options={strategies.map(s => ({ value: s.id, label: s.name }))}
            />
          </Form.Item>
          <Form.Item name="dateRange" label="回测区间" rules={[{ required: true, message: '请选择日期范围' }]}>
            <RangePicker />
          </Form.Item>
          <Form.Item name="initial_capital" label="初始资金">
            <InputNumber min={100000} max={100000000} step={100000} style={{ width: 160 }} addonAfter="¥" />
          </Form.Item>
          <Form.Item>
            <Space>
              <Button type="primary" htmlType="submit" icon={<PlayCircleOutlined />} loading={submitting}>
                开始回测
              </Button>
              <Button icon={<ReloadOutlined />} onClick={() => { fetchTasks(); }}>刷新</Button>
            </Space>
          </Form.Item>
        </Form>
      </Card>

      <Card title="回测任务列表">
        <Spin spinning={loading}>
          <Table
            dataSource={tasks}
            columns={columns}
            rowKey="task_id"
            pagination={{ pageSize: 10, showSizeChanger: false }}
            size="middle"
            bordered
            locale={{ emptyText: '暂无回测任务，请提交新任务' }}
          />
        </Spin>
      </Card>
    </div>
  );
};

export default Backtest;