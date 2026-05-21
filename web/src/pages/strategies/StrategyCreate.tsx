import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Card, Typography, Form, Input, Button, message, Alert } from 'antd';
import { ArrowLeftOutlined } from '@ant-design/icons';
import { useStrategyStore } from '../../stores/strategyStore';

const { Title } = Typography;
const { TextArea } = Input;

const StrategyCreate: React.FC = () => {
  const navigate = useNavigate();
  const [form] = Form.useForm();
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const { createStrategy } = useStrategyStore();

  const handleSubmit = async (values: { name: string; graph_content?: string; params?: string }) => {
    setSubmitting(true);
    setError(null);
    try {
      let params: Record<string, unknown> = {};
      if (values.params) {
        try {
          params = JSON.parse(values.params);
        } catch {
          setError('策略参数 JSON 格式错误');
          setSubmitting(false);
          return;
        }
      }
      const strategy = await createStrategy({
        name: values.name,
        graph_content: values.graph_content,
        params,
      });
      message.success(`策略「${strategy.name}」创建成功`);
      navigate(`/strategies/${strategy.id}`);
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
        >
          <Form.Item name="name" label="策略名称" rules={[{ required: true, message: '请输入策略名称' }]}>
            <Input placeholder="例如：ma_cross" />
          </Form.Item>

          <Form.Item name="graph_content" label="Graph JSON 内容">
            <TextArea
              rows={8}
              placeholder='{"nodes": [...], "edges": [...]}'
              style={{ fontFamily: 'monospace' }}
            />
          </Form.Item>

          <Form.Item name="params" label="策略参数 (JSON)">
            <TextArea rows={4} placeholder='{"fast": 5, "slow": 20}' style={{ fontFamily: 'monospace' }} />
          </Form.Item>

          <Button type="primary" htmlType="submit" loading={submitting}>创建策略</Button>
        </Form>
      </Card>
    </div>
  );
};

export default StrategyCreate;