import axios from 'axios';

// C++ HTTP service (StrategyApi) — port 9090
const apiClient = axios.create({
  baseURL: '/api/cpp',
  timeout: 15000,
  headers: { 'Content-Type': 'application/json' },
});

// Python FastAPI — port 8000 (strategy upload / compile)
const pythonClient = axios.create({
  baseURL: '/api/py/v2',
  timeout: 30000,
  headers: { 'Content-Type': 'application/json' },
});

apiClient.interceptors.response.use(
  (response) => response,
  (error) => {
    console.warn(`[C++ API Error] ${error.config?.url}`, error.message);
    return Promise.reject(error);
  }
);

pythonClient.interceptors.response.use(
  (response) => response,
  (error) => {
    console.warn(`[Python API Error] ${error.config?.url}`, error.message);
    return Promise.reject(error);
  }
);

export { pythonClient };
export default apiClient;
