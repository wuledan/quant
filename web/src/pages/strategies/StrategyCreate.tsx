import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Card, Typography, Form, Input, Select, Button, message, Alert } from 'antd';
import { ArrowLeftOutlined } from '@ant-design/icons';

const { Title, Text } = Typography;
const { TextArea } = Input;

const STRATEGY_TYPES = [
  { value: 'trend', label: '趋势策略' },
  { value: 'momentum', label: '动量策略' },
  { value: 'mean_reversion', label: '均值回归' },
  { value: 'alpha', label: 'Alpha因子' },
  { value: 'arbitrage', label: '套利策略' },
  { value: 'machine_learning', label: '机器学习' },
];

const StrategyCreate: React.FC = () => {
  const navigate = useNavigate();
  const [form] = Form.useForm();
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const handleSubmit = async (values: Record<string, unknown>) => {
    setSubmitting(true);
    setError(null);
    try {
      // Simulate API call
      await new Promise((r) => setTimeout(r, 500));
      message.success(`策略「${values.name}」创建成功`);
      navigate('/strategies');
    } catch (err) {
      setError(err instanceof Error ? err.message : '创建失败');
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <div style={{ maxWidth: 720 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 16 }}>
        <Button icon={<ArrowLeftOutlined />} shape="circle" size="small" onClick={() => navigate('/strategies')} />
        <Title level={4} style={{ margin: 0 }}>创建策略</Title>
      </div>

      {error && <Alert message={error} type="error" showIcon closable onClose={() => setError(null)} style={{ marginBottom: 16 }} />}

      <Card>
        <Form
          form={form}
          layout="vertical"
          onFinish={handleSubmit}
          initialValues={{ type: 'trend' }}
        >
          <Form.Item name="name" label="策略名称" rules={[{ required: true, message: '请输入策略名称' }]}>
            <Input placeholder="例如：双均线策略" />
          </Form.Item>

          <Form.Item name="type" label="策略类型" rules={[{ required: true }]}>
            <Select options={STRATEGY_TYPES} />
          </Form.Item>

          <Form.Item name="description" label="策略描述">
            <TextArea rows={3} placeholder="简要描述策略逻辑..." />
          </Form.Item>

          <Form.Item name="params" label="策略参数 (JSON)">
            <TextArea rows={6} placeholder='{"fast": 5, "slow": 20}' />
          </Form.Item>

          <Button type="primary" htmlType="submit" loading={submitting}>创建策略</Button>
        </Form>
      </Card>
    </div>
  );
};

export default StrategyCreate;