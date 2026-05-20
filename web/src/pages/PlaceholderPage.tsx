import React from 'react';
import { Typography, Card } from 'antd';

const { Title, Paragraph } = Typography;

const PlaceholderPage: React.FC<{ title: string; description?: string }> = ({
  title,
  description,
}) => {
  return (
    <div>
      <Title level={4} style={{ marginTop: 0 }}>
        {title}
      </Title>
      <Card>
        <Paragraph>{description || `${title}功能即将上线，敬请期待。`}</Paragraph>
      </Card>
    </div>
  );
};

export default PlaceholderPage;
