import React from 'react';
import { Typography, Card, Form, Select, Switch, Button, Divider } from 'antd';

const { Title } = Typography;

const Settings: React.FC = () => {
  return (
    <div>
      <Title level={4} style={{ marginTop: 0 }}>
        系统设置
      </Title>

      <Card title="通用设置" style={{ maxWidth: 600 }}>
        <Form layout="vertical">
          <Form.Item label="语言">
            <Select defaultValue="zh-CN" style={{ width: 200 }}>
              <Select.Option value="zh-CN">简体中文</Select.Option>
              <Select.Option value="en-US">English</Select.Option>
            </Select>
          </Form.Item>
          <Form.Item label="数据刷新间隔">
            <Select defaultValue={5000} style={{ width: 200 }}>
              <Select.Option value={1000}>1秒</Select.Option>
              <Select.Option value={5000}>5秒</Select.Option>
              <Select.Option value={15000}>15秒</Select.Option>
              <Select.Option value={60000}>60秒</Select.Option>
            </Select>
          </Form.Item>
          <Form.Item label="自动同步" valuePropName="checked">
            <Switch defaultChecked />
          </Form.Item>
          <Divider />
          <Button type="primary">保存设置</Button>
        </Form>
      </Card>
    </div>
  );
};

export default Settings;
