import React from 'react';
import { Outlet } from 'react-router-dom';
import { Layout as AntLayout, theme, ConfigProvider } from 'antd';
import Sidebar from './Sidebar';
import Header from './Header';
import { useAppStore } from '../../stores/appStore';

const { Content } = AntLayout;

const Layout: React.FC = () => {
  const { sidebarCollapsed, theme: appTheme } = useAppStore();
  const { token } = theme.useToken();

  return (
    <ConfigProvider
      theme={{
        algorithm:
          appTheme === 'dark'
            ? theme.darkAlgorithm
            : theme.defaultAlgorithm,
      }}
    >
      <AntLayout style={{ minHeight: '100vh' }}>
        <Sidebar collapsed={sidebarCollapsed} />
        <AntLayout>
          <Header />
          <Content
            style={{
              margin: 0,
              padding: 24,
              background: token.colorBgElevated,
              minHeight: 280,
              overflow: 'auto',
            }}
          >
            <Outlet />
          </Content>
        </AntLayout>
      </AntLayout>
    </ConfigProvider>
  );
};

export default Layout;
