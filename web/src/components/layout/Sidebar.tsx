import React from 'react';
import { useNavigate, useLocation } from 'react-router-dom';
import { Layout as AntLayout, Menu } from 'antd';
import {
  DashboardOutlined,
  ExperimentOutlined,
  FundOutlined,
  PieChartOutlined,
  SafetyCertificateOutlined,
  DatabaseOutlined,
  SettingOutlined,
  BarChartOutlined,
  ReadOutlined,
} from '@ant-design/icons';

const { Sider } = AntLayout;

interface SidebarProps {
  collapsed: boolean;
}

const menuItems = [
  { key: '/', icon: <DashboardOutlined />, label: '仪表盘' },
  {
    key: '/strategies',
    icon: <ExperimentOutlined />,
    label: '策略管理',
  },
  { key: '/market', icon: <BarChartOutlined />, label: '行情数据' },
  { key: '/backtest', icon: <FundOutlined />, label: '回测中心' },
  { key: '/news', icon: <ReadOutlined />, label: '新闻资讯' },
  { key: '/factors', icon: <DatabaseOutlined />, label: '因子数据' },
  { key: '/portfolio', icon: <PieChartOutlined />, label: '持仓监控' },
  { key: '/risk', icon: <SafetyCertificateOutlined />, label: '风控仪表盘' },
  { type: 'divider' as const },
  { key: '/settings', icon: <SettingOutlined />, label: '系统设置' },
];

const Sidebar: React.FC<SidebarProps> = ({ collapsed }) => {
  const navigate = useNavigate();
  const location = useLocation();

  // Determine selected key based on current path prefix
  const selectedKey = '/' + location.pathname.split('/').filter(Boolean)[0];

  return (
    <Sider
      trigger={null}
      collapsible
      collapsed={collapsed}
      theme="dark"
      style={{
        overflow: 'auto',
        height: '100vh',
        position: 'sticky',
        top: 0,
        left: 0,
      }}
    >
      <div
        style={{
          height: 56,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          color: '#fff',
          fontWeight: 700,
          fontSize: collapsed ? 16 : 18,
          letterSpacing: 1,
          borderBottom: '1px solid rgba(255,255,255,0.1)',
          whiteSpace: 'nowrap',
          overflow: 'hidden',
        }}
      >
        {collapsed ? 'QT' : 'Quant Invest'}
      </div>
      <Menu
        theme="dark"
        mode="inline"
        selectedKeys={[selectedKey]}
        defaultOpenKeys={[]}
        items={menuItems}
        onClick={({ key }) => navigate(key)}
        inlineCollapsed={collapsed}
      />
    </Sider>
  );
};

export default Sidebar;
