import React from 'react';
import { useParams } from 'react-router-dom';
import PlaceholderPage from '../PlaceholderPage';

const StrategyDetail: React.FC = () => {
  const { id } = useParams<{ id: string }>();
  return <PlaceholderPage title={`策略详情 #${id}`} description={`查看和编辑策略 #${id} 的详细信息、回测结果和运行状态。`} />;
};

export default StrategyDetail;
