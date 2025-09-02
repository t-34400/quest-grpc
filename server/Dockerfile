FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY ./protos/ ./protos/
RUN python -m grpc_tools.protoc -I protos --python_out=/app --grpc_python_out=/app protos/vision.proto
RUN [ -f protos/__init__.py ] || touch protos/__init__.py

COPY server.py .

EXPOSE 8032
ENV PYTHONPATH=/app
CMD ["python", "server.py"]
