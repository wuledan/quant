import React from 'react';
import { Spin } from 'antd';

const Loading: React.FC<{ tip?: string }> = ({ tip = '加载中...' }) => {
  return (
    <div
      style={{
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        minHeight: 240,
        width: '100%',
      }}
    >
      <Spin tip={tip} size="large">
        <div style={{ padding: 50 }} />
      </Spin>
    </div>
  );
};

export default Loading;
