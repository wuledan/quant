import { useEffect, useRef, useState, useCallback } from 'react';
import { MarketWebSocket } from '../api/websocket';
import type { WebSocketMessage } from '../api/websocket';

interface UseWebSocketReturn {
  isConnected: boolean;
  lastMessage: WebSocketMessage | null;
  send: (data: unknown) => void;
  subscribe: (channel: string) => void;
  subscribeChannels: (channels: string[]) => void;
  unsubscribe: (channel: string) => void;
}

export function useWebSocket(url: string, initialChannels?: string[]): UseWebSocketReturn {
  const wsRef = useRef<MarketWebSocket | null>(null);
  const [isConnected, setIsConnected] = useState(false);
  const [lastMessage, setLastMessage] = useState<WebSocketMessage | null>(null);

  useEffect(() => {
    const ws = new MarketWebSocket();
    wsRef.current = ws;

    const unsubOpen = ws.onOpen(() => setIsConnected(true));
    const unsubClose = ws.onClose(() => setIsConnected(false));
    const unsubMsg = ws.onMessage((data) => {
      setLastMessage(data as WebSocketMessage);
    });

    ws.connect(url);

    // Auto-subscribe to initial channels after connection
    if (initialChannels && initialChannels.length > 0) {
      const unsubOpenSub = ws.onOpen(() => {
        ws.subscribeChannels(initialChannels);
      });
      return () => {
        unsubOpen();
        unsubClose();
        unsubMsg();
        unsubOpenSub();
        ws.disconnect();
        wsRef.current = null;
      };
    }

    return () => {
      unsubOpen();
      unsubClose();
      unsubMsg();
      ws.disconnect();
      wsRef.current = null;
    };
  }, [url]);

  const send = useCallback((data: unknown) => {
    wsRef.current?.send(data);
  }, []);

  const subscribe = useCallback((channel: string) => {
    wsRef.current?.subscribe(channel);
  }, []);

  const subscribeChannels = useCallback((channels: string[]) => {
    wsRef.current?.subscribeChannels(channels);
  }, []);

  const unsubscribe = useCallback((channel: string) => {
    wsRef.current?.unsubscribe(channel);
  }, []);

  return { isConnected, lastMessage, send, subscribe, subscribeChannels, unsubscribe };
}
