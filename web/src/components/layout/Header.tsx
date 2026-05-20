import React from 'react';
import { Layout as AntLayout, theme, Badge, Button } from 'antd';
import {
  MenuFoldOutlined,
  MenuUnfoldOutlined,
  BulbOutlined,
} from '@ant-design/icons';
import { useAppStore } from '../../stores/appStore';

const { Header: AntHeader } = AntLayout;

const statusColorMap: Record<string, 'success' | 'error' | 'warning' | 'default'> = {
  running: 'success',
  stopped: 'default',
  error: 'error',
};

const statusLabelMap: Record<string, string> = {
  running: '运行中',
  stopped: '已停止',
  error: '异常',
};

const Header: React.FC = () => {
  const {
    sidebarCollapsed,
    toggleSidebar,
    systemStatus,
    theme: appTheme,
    toggleTheme,
  } = useAppStore();
  const { token } = theme.useToken();

  return (
    <AntHeader
      style={{
        padding: '0 24px',
        background: token.colorBgContainer,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'space-between',
        borderBottom: `1px solid ${token.colorBorderSecondary}`,
        height: 56,
        lineHeight: '56px',
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
        <Button
          type="text"
          icon={sidebarCollapsed ? <MenuUnfoldOutlined /> : <MenuFoldOutlined />}
          onClick={toggleSidebar}
        />
        <span style={{ fontWeight: 600, fontSize: 16, color: token.colorText }}>
          量化交易系统
        </span>
      </div>

      <div style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
        <Badge
          status={statusColorMap[systemStatus.engine] ?? 'default'}
          text={statusLabelMap[systemStatus.engine] ?? '未知'}
        />
        <Button
          type="text"
          icon={<BulbOutlined />}
          onClick={toggleTheme}
          title={appTheme === 'dark' ? '切换亮色模式' : '切换暗色模式'}
        />
      </div>
    </AntHeader>
  );
};

export default Header;
