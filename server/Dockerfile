FROM nvidia/cuda:12.4.1-cudnn-runtime-ubuntu22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-pip python3-venv libglib2.0-0 libgl1 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN python3 -m pip install --no-cache-dir -r requirements.txt

COPY ./protos/ ./protos/
RUN python3 -m grpc_tools.protoc \
    -I protos \
    --python_out=/app \
    --grpc_python_out=/app \
    protos/vision.proto
RUN [ -f protos/__init__.py ] || touch protos/__init__.py

COPY main.py .
COPY vision_server.py .
COPY test_server.py .

COPY onnx/model.onnx /app/model.onnx
ENV MODEL_PATH=/app/model.onnx

EXPOSE 8032
ENV PYTHONPATH=/app

CMD ["python3", "main.py"]
