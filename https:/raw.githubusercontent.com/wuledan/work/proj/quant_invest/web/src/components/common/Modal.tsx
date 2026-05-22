import React, { useEffect, useRef } from 'react';
import { Button } from 'antd';
import { CloseOutlined } from '@ant-design/icons';

interface ModalProps {
  open: boolean;
  title: string;
  children: React.ReactNode;
  onClose: () => void;
  width?: number;
}

const overlayStyle: React.CSSProperties = {
  position: 'fixed',
  inset: 0,
  background: 'rgba(0, 0, 0, 0.45)',
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  zIndex: 1050,
};

const Modal: React.FC<ModalProps> = ({ open, title, children, onClose, width = 600 }) => {
  const contentRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && open) {
        onClose();
      }
    };
    document.addEventListener('keydown', handleKeyDown);
    return () => document.removeEventListener('keydown', handleKeyDown);
  }, [open, onClose]);

  if (!open) return null;

  const handleOverlayClick = (e: React.MouseEvent) => {
    if (contentRef.current && !contentRef.current.contains(e.target as Node)) {
      onClose();
    }
  };

  return (
    <div style={overlayStyle} onClick={handleOverlayClick} role="dialog" aria-modal="true" aria-label={title}>
      <div
        ref={contentRef}
        style={{
          background: '#fff',
          borderRadius: 8,
          width,
          maxWidth: '90vw',
          maxHeight: '85vh',
          display: 'flex',
          flexDirection: 'column',
          boxShadow: '0 6px 30px rgba(0,0,0,0.15)',
          overflow: 'hidden',
        }}
      >
        <div
          style={{
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            padding: '16px 24px',
            borderBottom: '1px solid #f0f0f0',
          }}
        >
          <h3 style={{ margin: 0, fontSize: 18, fontWeight: 600, color: '#000' }}>{title}</h3>
          <Button
            type="text"
            icon={<CloseOutlined />}
            onClick={onClose}
            aria-label="关闭"
          />
        </div>
        <div style={{ padding: 24, overflowY: 'auto', flex: 1 }}>{children}</div>
      </div>
    </div>
  );
};

export default Modal;
