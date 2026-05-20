import React from 'react';
import { useNavigate, useLocation } from 'react-router-dom';
import { Menu } from 'antd';
import {
  FundOutlined,
  ExperimentOutlined,
  PieChartOutlined,
  SafetyCertificateOutlined,
  DatabaseOutlined,
} from '@ant-design/icons';

const menuItems = [
  { key: '/strategies', icon: <ExperimentOutlined />, label: '策略管理' },
  { key: '/backtest', icon: <FundOutlined />, label: '回测中心' },
  { key: '/portfolio', icon: <PieChartOutlined />, label: '持仓监控' },
  { key: '/risk', icon: <SafetyCertificateOutlined />, label: '风控仪表盘' },
  { key: '/data', icon: <DatabaseOutlined />, label: '数据浏览' },
];

const NavMenu: React.FC<{ collapsed: boolean }> = ({ collapsed }) => {
  const navigate = useNavigate();
  const location = useLocation();

  return (
    <Menu
      theme="dark"
      mode="inline"
      selectedKeys={[location.pathname]}
      items={menuItems}
      onClick={({ key }) => navigate(key)}
      inlineCollapsed={collapsed}
    />
  );
};

export default NavMenu;