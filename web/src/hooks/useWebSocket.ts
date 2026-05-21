import { useEffect, useRef, useState, useCallback } from 'react';
import { MarketWebSocket } from '../api/websocket';
import type { WsEnvelope, WsChannel } from '../api/websocket';

export type { WsEnvelope, WsChannel };
export { MarketWebSocket };

interface UseWebSocketOptions {
  url?: string;
  channels?: WsChannel[];
  autoConnect?: boolean;
}

interface UseWebSocketReturn {
  isConnected: boolean;
  lastMessage: WsEnvelope | null;
  connect: () => void;
  disconnect: () => void;
  subscribe: (channel: string) => void;
  unsubscribe: (channel: string) => void;
  send: (data: unknown) => void;
}

export const useWebSocket = (options: UseWebSocketOptions = {}): UseWebSocketReturn => {
  const { url, channels = [], autoConnect = true } = options;
  const wsRef = useRef<MarketWebSocket | null>(null);
  const [isConnected, setIsConnected] = useState(false);
  const [lastMessage, setLastMessage] = useState<WsEnvelope | null>(null);

  const connect = useCallback(() => {
    if (!wsRef.current) {
      wsRef.current = new MarketWebSocket();
    }
    wsRef.current.connect(url);
  }, [url]);

  const disconnect = useCallback(() => {
    wsRef.current?.disconnect();
    setIsConnected(false);
  }, []);

  const subscribe = useCallback((channel: string) => {
    wsRef.current?.subscribe(channel);
  }, []);

  const unsubscribe = useCallback((channel: string) => {
    wsRef.current?.unsubscribe(channel);
  }, []);

  const send = useCallback((data: unknown) => {
    wsRef.current?.send(data);
  }, []);

  useEffect(() => {
    const ws = new MarketWebSocket();
    wsRef.current = ws;

    const unsubOpen = ws.onOpen(() => setIsConnected(true));
    const unsubClose = ws.onClose(() => setIsConnected(false));
    const unsubMessage = ws.onMessage((envelope: WsEnvelope) => {
      setLastMessage(envelope);
    });

    if (autoConnect) {
      ws.connect(url);
      if (channels.length > 0) {
        ws.subscribeChannels(channels);
      }
    }

    return () => {
      unsubOpen();
      unsubClose();
      unsubMessage();
      ws.disconnect();
      wsRef.current = null;
    };
  }, [url, autoConnect]); // eslint-disable-line react-hooks/exhaustive-deps

  return { isConnected, lastMessage, connect, disconnect, subscribe, unsubscribe, send };
};

export default useWebSocket;
