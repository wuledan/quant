type MessageCallback = (data: unknown) => void;
type EventCallback = (event: Event) => void;

export interface WebSocketMessage {
  channel: string;
  data: unknown;
  type: string;
}

export class MarketWebSocket {
  private ws: WebSocket | null = null;
  private url = '';
  private reconnectAttempts = 0;
  private maxReconnectAttempts = 10;
  private reconnectDelay = 1000;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private isManualDisconnect = false;

  private messageCallbacks: MessageCallback[] = [];
  private openCallbacks: EventCallback[] = [];
  private closeCallbacks: EventCallback[] = [];
  private errorCallbacks: EventCallback[] = [];

  get isConnected(): boolean {
    return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
  }

  connect(url: string): void {
    this.url = url;
    this.isManualDisconnect = false;
    this.reconnectAttempts = 0;
    this.createConnection();
  }

  disconnect(): void {
    this.isManualDisconnect = true;
    this.clearReconnectTimer();
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }

  subscribe(channel: string): void {
    this.send({ type: 'subscribe', channel });
  }

  unsubscribe(channel: string): void {
    this.send({ type: 'unsubscribe', channel });
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

    this.ws.onopen = (event: Event) => {
      this.reconnectAttempts = 0;
      this.openCallbacks.forEach((cb) => cb(event));
    };

    this.ws.onmessage = (event: MessageEvent) => {
      try {
        const parsed = JSON.parse(event.data) as WebSocketMessage;
        this.messageCallbacks.forEach((cb) => cb(parsed));
      } catch {
        console.warn('[WS] Failed to parse message:', event.data);
      }
    };

    this.ws.onclose = (event: Event) => {
      this.closeCallbacks.forEach((cb) => cb(event));
      if (!this.isManualDisconnect) {
        this.scheduleReconnect();
      }
    };

    this.ws.onerror = (event: Event) => {
      this.errorCallbacks.forEach((cb) => cb(event));
    };
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
