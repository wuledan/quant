import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import {
  Card, Typography, Form, Input, Button, Upload, message, Alert, Radio, Space, Divider,
} from 'antd';
import { ArrowLeftOutlined, UploadOutlined, InboxOutlined } from '@ant-design/icons';
import type { UploadFile } from 'antd/es/upload/interface';
import { uploadPythonFile, createStrategy } from '../../api/strategy';

const { Title, Text } = Typography;
const { Dragger } = Upload;
const { TextArea } = Input;

type UploadMode = 'python' | 'graph';

const StrategyUpload: React.FC = () => {
  const navigate = useNavigate();
  const [mode, setMode] = useState<UploadMode>('python');
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [fileList, setFileList] = useState<UploadFile[]>([]);
  const [form] = Form.useForm();

  const handlePythonUpload = async () => {
    if (fileList.length === 0) {
      message.warning('请先选择 .py 文件');
      return;
    }
    const file = fileList[0].originFileObj as File;
    setSubmitting(true);
    setError(null);
    try {
      const resp = await uploadPythonFile(file);
      message.success(`策略「${resp.name}」上传成功，ID: ${resp.strategy_id}`);
      navigate(`/strategies/${resp.strategy_id}`);
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : '上传失败';
      setError(msg);
    } finally {
      setSubmitting(false);
    }
  };

  const handleGraphSubmit = async (values: { name: string; graph_content: string }) => {
    setSubmitting(true);
    setError(null);
    try {
      const resp = await createStrategy({
        name: values.name,
        graph_content: values.graph_content,
      });
      message.success(`策略「${resp.name}」创建成功，ID: ${resp.id}`);
      navigate(`/strategies/${resp.id}`);
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : '创建失败';
      setError(msg);
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <div style={{ maxWidth: 720 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 16 }}>
        <Button icon={<ArrowLeftOutlined />} shape="circle" size="small" onClick={() => navigate('/strategies')} />
        <Title level={4} style={{ margin: 0 }}>上传策略</Title>
      </div>

      {error && <Alert message={error} type="error" showIcon closable onClose={() => setError(null)} style={{ marginBottom: 16 }} />}

      <Card>
        <Radio.Group value={mode} onChange={(e) => { setMode(e.target.value); setError(null); }} style={{ marginBottom: 24 }}>
          <Radio.Button value="python">上传 Python 文件</Radio.Button>
          <Radio.Button value="graph">直接上传 Graph JSON</Radio.Button>
        </Radio.Group>

        {mode === 'python' ? (
          <div>
            <Text type="secondary" style={{ display: 'block', marginBottom: 16 }}>
              上传 .py 策略文件，Python FastAPI 将编译为 IR 后转发到 C++ 服务注册。
            </Text>
            <Dragger
              accept=".py"
              maxCount={1}
              fileList={fileList}
              beforeUpload={() => false}
              onChange={({ fileList: newFileList }) => setFileList(newFileList)}
              onRemove={() => { setFileList([]); return true; }}
            >
              <p className="ant-upload-drag-icon"><InboxOutlined /></p>
              <p className="ant-upload-text">点击或拖拽 .py 文件到此处</p>
              <p className="ant-upload-hint">支持 Python 策略脚本文件</p>
            </Dragger>
            <Divider />
            <Button
              type="primary"
              icon={<UploadOutlined />}
              loading={submitting}
              disabled={fileList.length === 0}
              onClick={handlePythonUpload}
            >
              上传并编译
            </Button>
          </div>
        ) : (
          <Form
            form={form}
            layout="vertical"
            onFinish={handleGraphSubmit}
          >
            <Form.Item name="name" label="策略名称" rules={[{ required: true, message: '请输入策略名称' }]}>
              <Input placeholder="例如：ma_cross" />
            </Form.Item>
            <Form.Item
              name="graph_content"
              label="Graph JSON 内容"
              rules={[{ required: true, message: '请输入 Graph JSON' }]}
            >
              <TextArea
                rows={12}
                placeholder='{"nodes": [...], "edges": [...]}'
                style={{ fontFamily: 'monospace' }}
              />
            </Form.Item>
            <Space>
              <Button type="primary" htmlType="submit" loading={submitting}>
                注册策略
              </Button>
              <Upload
                accept=".json,.graph"
                maxCount={1}
                showUploadList={false}
                beforeUpload={(file) => {
                  const reader = new FileReader();
                  reader.onload = (e) => {
                    const text = e.target?.result as string;
                    form.setFieldsValue({ graph_content: text });
                  };
                  reader.readAsText(file);
                  return false;
                }}
              >
                <Button icon={<UploadOutlined />}>从文件导入</Button>
              </Upload>
            </Space>
          </Form>
        )}
      </Card>
    </div>
  );
};

export default StrategyUpload;
