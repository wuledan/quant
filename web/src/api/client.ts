import axios from 'axios';

const apiClient = axios.create({
  baseURL: '/api/v1',
  timeout: 15000,
  headers: { 'Content-Type': 'application/json' },
});

apiClient.interceptors.response.use(
  (response) => response,
  (error) => {
    console.warn(`API Error: ${error.config?.url}`, error.message);
    return Promise.reject(error);
  }
);

export default apiClient;
