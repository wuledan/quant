import React from 'react';
import { Form, Input, Select, Button, Space } from 'antd';
import StrategyParams from './StrategyParams';
import type { StrategyCreatePayload, StrategyUpdatePayload } from '../../api/strategy';

type FormValues = {
  name: string;
  type: string;
  params: Record<string, unknown>;
};

interface StrategyFormProps {
  initialValues?: Partial<FormValues>;
  loading?: boolean;
  onSubmit: (values: StrategyCreatePayload | StrategyUpdatePayload) => void | Promise<void>;
  onCancel: () => void;
  submitLabel?: string;
}

const STRATEGY_TYPES = [
  { label: '趋势跟踪', value: 'trend_following' },
  { label: '均值回归', value: 'mean_reversion' },
  { label: '动量策略', value: 'momentum' },
  { label: '统计套利', value: 'stat_arb' },
  { label: '机器学习', value: 'ml' },
  { label: '自定义', value: 'custom' },
];

const StrategyForm: React.FC<StrategyFormProps> = ({
  initialValues,
  loading = false,
  onSubmit,
  onCancel,
  submitLabel = 'Submit',
}) => {
  const [form] = Form.useForm<FormValues>();

  const handleFinish = (values: FormValues) => {
    const payload: StrategyCreatePayload | StrategyUpdatePayload = {
      name: values.name,
      type: values.type,
      params: values.params ?? {},
    };
    onSubmit(payload);
  };

  return (
    <Form
      form={form}
      layout="vertical"
      initialValues={{
        name: '',
        type: 'custom',
        params: {},
        ...initialValues,
      }}
      onFinish={handleFinish}
      style={{ maxWidth: 600 }}
    >
      <Form.Item
        label="Strategy Name"
        name="name"
        rules={[
          { required: true, message: 'Strategy name is required' },
          { min: 2, message: 'Name must be at least 2 characters' },
        ]}
      >
        <Input placeholder="Enter strategy name" />
      </Form.Item>

      <Form.Item
        label="Strategy Type"
        name="type"
        rules={[{ required: true, message: 'Please select a strategy type' }]}
      >
        <Select placeholder="Select strategy type" options={STRATEGY_TYPES} />
      </Form.Item>

      <Form.Item label="Parameters" name="params">
        <StrategyParams />
      </Form.Item>

      <Form.Item>
        <Space>
          <Button type="primary" htmlType="submit" loading={loading}>
            {submitLabel}
          </Button>
          <Button onClick={onCancel}>Cancel</Button>
        </Space>
      </Form.Item>
    </Form>
  );
};

export default StrategyForm;
