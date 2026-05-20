import React from 'react';
import { Empty as AntEmpty } from 'antd';

interface EmptyProps {
  description?: string;
  image?: React.ReactNode;
}

const Empty: React.FC<EmptyProps> = ({
  description = '暂无数据',
  image,
}) => {
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
      <AntEmpty description={description} image={image} />
    </div>
  );
};

export default Empty;
