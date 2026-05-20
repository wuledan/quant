import React from 'react';

interface HeatmapCell {
  row: string;
  col: string;
  value: number;
}

interface HeatmapProps {
  data: HeatmapCell[];
  rowLabel?: string;
  colLabel?: string;
  title?: string;
  colorScale?: 'green-red' | 'blue-white' | 'red-yellow-green';
}

function getColor(value: number, scale: NonNullable<HeatmapProps['colorScale']>): string {
  const clamped = Math.max(0, Math.min(1, value));

  switch (scale) {
    case 'green-red':
    case 'red-yellow-green': {
      const r = clamped < 0.5 ? 255 : Math.round(255 * (1 - (clamped - 0.5) * 2));
      const g = clamped < 0.5 ? Math.round(255 * clamped * 2) : 255;
      return `rgb(${r},${g},60)`;
    }
    case 'blue-white':
    default: {
      const intensity = Math.round(clamped * 200);
      return `rgb(${255 - intensity},${255 - intensity},255)`;
    }
  }
}

function formatValue(v: number): string {
  if (Math.abs(v) >= 1000) return v.toFixed(0);
  if (Math.abs(v) >= 1) return v.toFixed(2);
  return v.toFixed(4);
}

const Heatmap: React.FC<HeatmapProps> = ({
  data,
  rowLabel = '行',
  colLabel = '列',
  title,
  colorScale = 'green-red',
}) => {
  if (!data || data.length === 0) {
    return (
      <div style={{ textAlign: 'center', padding: 40, color: '#999' }}>
        暂无热力图数据
      </div>
    );
  }

  const rows = [...new Set(data.map((d) => d.row))];
  const cols = [...new Set(data.map((d) => d.col))];

  const valueMap = new Map<string, number>();
  const allValues = data.map((d) => d.value);
  const minVal = Math.min(...allValues);
  const maxVal = Math.max(...allValues);
  const range = maxVal - minVal || 1;

  data.forEach((d) => {
    valueMap.set(`${d.row}|${d.col}`, d.value);
  });

  const cellStyle: React.CSSProperties = {
    padding: '8px 4px',
    textAlign: 'center',
    fontSize: 12,
    border: '1px solid rgba(0,0,0,0.06)',
    minWidth: 80,
    whiteSpace: 'nowrap',
  };

  const headerCellStyle: React.CSSProperties = {
    ...cellStyle,
    fontWeight: 600,
    background: '#fafafa',
    color: '#333',
    position: 'sticky' as const,
    top: 0,
    zIndex: 1,
  };

  return (
    <div>
      {title && (
        <h4 style={{ margin: '0 0 12px', fontWeight: 600, fontSize: 15 }}>{title}</h4>
      )}
      <div style={{ overflowX: 'auto' }}>
        <div
          style={{
            display: 'grid',
            gridTemplateColumns: `auto repeat(${cols.length}, 1fr)`,
            fontSize: 13,
          }}
        >
          <div style={headerCellStyle}>{rowLabel} \\ {colLabel}</div>
          {cols.map((col) => (
            <div key={col} style={headerCellStyle}>
              {col}
            </div>
          ))}

          {rows.map((row) => (
            <React.Fragment key={row}>
              <div
                style={{
                  ...cellStyle,
                  fontWeight: 600,
                  background: '#fafafa',
                  textAlign: 'left',
                  paddingLeft: 12,
                  position: 'sticky' as const,
                  left: 0,
                  zIndex: 1,
                }}
              >
                {row}
              </div>
              {cols.map((col) => {
                const value = valueMap.get(`${row}|${col}`);
                const normalized = value !== undefined ? (value - minVal) / range : 0;
                const bgColor = value !== undefined ? getColor(normalized, colorScale) : '#f5f5f5';

                return (
                  <div
                    key={`${row}|${col}`}
                    style={{
                      ...cellStyle,
                      background: bgColor,
                      color: normalized > 0.6 ? '#fff' : '#333',
                      fontWeight: value !== undefined ? 500 : 400,
                    }}
                    title={value !== undefined ? `${row} / ${col}: ${formatValue(value)}` : '无数据'}
                  >
                    {value !== undefined ? formatValue(value) : '-'}
                  </div>
                );
              })}
            </React.Fragment>
          ))}
        </div>
      </div>
    </div>
  );
};

export default Heatmap;