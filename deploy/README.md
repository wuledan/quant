# Quant Investment System - Docker Deployment

Docker Compose configuration for the quant investment system infrastructure.

## Services

| Service | Port | Purpose |
|---------|------|---------|
| **etcd** | 2379 (client), 2380 (peer) | Strategy center - C++ engine watches for strategy changes |
| **MinIO** | 9010 (API), 9011 (Console) | S3-compatible object storage for historical market data (Parquet files) |

## Quick Start

```bash
# Navigate to deploy directory
cd /home/wuledan/work/proj/quant_invest/deploy

# Start all services
docker-compose up -d

# Check service status
docker-compose ps

# View logs
docker-compose logs -f
```

> **Note**: The docker-compose.yml uses `learn-path-etcd:latest` image which should be available locally.
> If the image is missing, you can:
> 1. Pull from quay.io: `docker pull quay.io/coreos/etcd:v3.5.11 && docker tag quay.io/coreos/etcd:v3.5.11 learn-path-etcd:latest`
> 2. Or use bitnami/etcd: `docker pull bitnami/etcd:3.5.11` and update the image in docker-compose.yml

## Verify Services

### etcd

```bash
# Test etcd connectivity
docker exec quant-etcd etcdctl put test key
docker exec quant-etcd etcdctl get test

# Expected output:
# test
# key

# Clean up test key
docker exec quant-etcd etcdctl del test
```

### MinIO

1. **Web Console**: Open http://localhost:9011 in your browser
   - Username: `quantadmin`
   - Password: `quantadmin123`

2. **Initialize Buckets**: Run the init container to create buckets:
   ```bash
   docker-compose --profile init up minio-init
   ```
   This creates:
   - `quant-kline` - K-line (candlestick) data
   - `quant-factor` - Factor data
   - `quant-corporate-actions` - Corporate actions data

3. **CLI Test** (using mc client):
```bash
# Install mc locally or use Docker
docker run --rm -it --network deploy_quant-net minio/mc:RELEASE.2024-01-16T17-11-14Z \
  mc alias set local http://minio:9000 quantadmin quantadmin123

# List buckets
docker run --rm -it --network deploy_quant-net minio/mc:RELEASE.2024-01-16T17-11-14Z \
  mc ls local
```

## Service Endpoints

| Service | Endpoint | Description |
|---------|----------|-------------|
| etcd Client | `http://localhost:2379` | etcd client API |
| MinIO API | `http://localhost:9010` | S3-compatible API |
| MinIO Console | `http://localhost:9011` | Web UI |

## Stop Services

```bash
# Stop all services (keep data)
docker-compose down

# Stop and remove data volumes
docker-compose down -v
# Note: This will delete all data in ./data/
```

## Data Persistence

All data is persisted in `./data/`:
- `./data/etcd/` - etcd data directory
- `./data/minio/` - MinIO object storage

> **Warning**: The `./data/` directory is excluded from git. Data persists across container restarts.

## Configuration

Environment variables are defined in `.env`:

| Variable | Default | Description |
|----------|---------|-------------|
| `ETCD_ENDPOINTS` | `http://etcd:2379` | etcd client endpoint |
| `MINIO_ENDPOINT` | `http://minio:9000` | MinIO API endpoint |
| `MINIO_ACCESS_KEY` | `quantadmin` | MinIO access key |
| `MINIO_SECRET_KEY` | `quantadmin123` | MinIO secret key |

## Network

All services are connected via `quant-net` bridge network. Future services (C++ engine, API server, etc.) can join this network for inter-service communication.

## Troubleshooting

### etcd health check failed
```bash
# Check etcd logs
docker-compose logs etcd

# Manual health check
docker exec quant-etcd etcdctl endpoint health
```

### MinIO not accessible
```bash
# Check MinIO logs
docker-compose logs minio

# Check if bucket init succeeded
docker-compose logs minio-init
```

### Port conflicts
If ports are already in use, modify the port mappings in `docker-compose.yml`:
```yaml
ports:
  - "12379:2379"  # Use host port 12379 instead
```

## Production Considerations

For production deployment, consider:

1. **Security**:
   - Change default credentials in `.env`
   - Enable TLS for etcd and MinIO
   - Use Docker secrets or external secret management

2. **High Availability**:
   - Run etcd cluster (3 or 5 nodes)
   - Configure MinIO in distributed mode

3. **Backup**:
   - Regular etcd snapshots: `etcdctl snapshot save`
   - MinIO data backup strategy

4. **Monitoring**:
   - Prometheus metrics for etcd (port 2379/metrics)
   - MinIO console for storage metrics
