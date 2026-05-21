import React from 'react';
import { Form, Input, Select, Button } from 'antd';
import type { StrategyCreatePayload } from '../../api/strategy';

interface StrategyFormProps {
  onSubmit: (values: StrategyCreatePayload) => void;
  initialValues?: Partial<StrategyCreatePayload>;
  loading?: boolean;
}

const StrategyForm: React.FC<StrategyFormProps> = ({ onSubmit, initialValues, loading }) => {
  const [form] = Form.useForm();

  return (
    <Form
      form={form}
      layout="vertical"
      onFinish={onSubmit}
      initialValues={initialValues}
    >
      <Form.Item name="name" label="策略名称" rules={[{ required: true, message: '请输入策略名称' }]}>
        <Input placeholder="例如：ma_cross" />
      </Form.Item>

      <Form.Item name="status" label="状态">
        <Select>
          <Select.Option value="draft">草稿</Select.Option>
          <Select.Option value="active">运行中</Select.Option>
          <Select.Option value="paused">已暂停</Select.Option>
        </Select>
      </Form.Item>

      <Form.Item name="graph_content" label="Graph JSON">
        <Input.TextArea rows={6} placeholder='{"nodes": [...], "edges": [...]}' style={{ fontFamily: 'monospace' }} />
      </Form.Item>

      <Form.Item>
        <Button type="primary" htmlType="submit" loading={loading}>
          保存
        </Button>
      </Form.Item>
    </Form>
  );
};

export default StrategyForm;