type MessageCallback = (data: WsEnvelope) => void;
type EventCallback = (event: Event) => void;

/** WsEventBridge JSON envelope format */
export interface WsEnvelope {
  channel: WsChannel;
  data: unknown;
  ts: number;
}

export type WsChannel = 'kline' | 'factor' | 'order' | 'risk' | 'signal' | 'market';

export class MarketWebSocket {
  private ws: WebSocket | null = null;
  private url = '';
  private reconnectAttempts = 0;
  private maxReconnectAttempts = 10;
  private reconnectDelay = 1000;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private isManualDisconnect = false;
  private subscribedChannels: Set<string> = new Set();
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;
  private heartbeatInterval = 30000;

  private messageCallbacks: MessageCallback[] = [];
  private openCallbacks: EventCallback[] = [];
  private closeCallbacks: EventCallback[] = [];
  private errorCallbacks: EventCallback[] = [];

  /** Default WS URL pointing to C++ WsEventBridge on :8282 */
  static readonly DEFAULT_URL = 'ws://192.168.3.10:8282';

  get isConnected(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  connect(url?: string): void {
    this.url = url ?? MarketWebSocket.DEFAULT_URL;
    this.isManualDisconnect = false;
    this.reconnectAttempts = 0;
    this.createConnection();
  }

  disconnect(): void {
    this.isManualDisconnect = true;
    this.clearReconnectTimer();
    this.stopHeartbeat();
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }

  subscribe(channel: string): void {
    this.subscribedChannels.add(channel);
    this.send({ action: 'subscribe', channels: [channel] });
  }

  subscribeChannels(channels: string[]): void {
    for (const ch of channels) {
      this.subscribedChannels.add(ch);
    }
    this.send({ action: 'subscribe', channels });
  }

  unsubscribe(channel: string): void {
    this.subscribedChannels.delete(channel);
    this.send({ action: 'unsubscribe', channels: [channel] });
  }

  send(data: unknown): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(data));
    }
  }

  /* -- Callback registration -- */

  onMessage(cb: MessageCallback): () => void {
    this.messageCallbacks.push(cb);
    return () => {
      this.messageCallbacks = this.messageCallbacks.filter((c) => c !== cb);
    };
  }

  onOpen(cb: EventCallback): () => void {
    this.openCallbacks.push(cb);
    return () => {
      this.openCallbacks = this.openCallbacks.filter((c) => c !== cb);
    };
  }

  onClose(cb: EventCallback): () => void {
    this.closeCallbacks.push(cb);
    return () => {
      this.closeCallbacks = this.closeCallbacks.filter((c) => c !== cb);
    };
  }

  onError(cb: EventCallback): () => void {
    this.errorCallbacks.push(cb);
    return () => {
      this.errorCallbacks = this.errorCallbacks.filter((c) => c !== cb);
    };
  }

  /* -- Private helpers -- */

  private createConnection(): void {
    if (!this.url) return;

    this.ws = new WebSocket(this.url);

    this.ws.onopen = (_event: Event) => {
      this.reconnectAttempts = 0;
      this.startHeartbeat();
      // Re-subscribe to previously subscribed channels
      if (this.subscribedChannels.size > 0) {
        this.send({ action: 'subscribe', channels: Array.from(this.subscribedChannels) });
      }
      this.openCallbacks.forEach((cb) => cb(_event));
    };

    this.ws.onmessage = (event: MessageEvent) => {
      try {
        const parsed = JSON.parse(event.data) as WsEnvelope;
        this.messageCallbacks.forEach((cb) => cb(parsed));
      } catch {
        console.warn('[WS] Failed to parse message:', event.data);
      }
    };

    this.ws.onclose = (event: Event) => {
      this.stopHeartbeat();
      this.closeCallbacks.forEach((cb) => cb(event));
      if (!this.isManualDisconnect) {
        this.scheduleReconnect();
      }
    };

    this.ws.onerror = (event: Event) => {
      this.errorCallbacks.forEach((cb) => cb(event));
    };
  }

  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.heartbeatTimer = setInterval(() => {
      this.send({ action: 'ping' });
    }, this.heartbeatInterval);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }

  private scheduleReconnect(): void {
    if (this.reconnectAttempts >= this.maxReconnectAttempts) {
      console.warn('[WS] Max reconnect attempts reached');
      return;
    }

    const delay = this.reconnectDelay * Math.pow(1.5, this.reconnectAttempts);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectAttempts++;
      this.createConnection();
    }, Math.min(delay, 30000));
  }

  private clearReconnectTimer(): void {
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }
}
