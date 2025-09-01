FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY protos/ ./protos/
RUN python -m grpc_tools.protoc -I protos --python_out=. --grpc_python_out=. protos/vision.proto

COPY server.py .

EXPOSE 8032
CMD ["python", "server.py"]
