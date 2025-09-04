#!/bin/bash

set -euo pipefail
IFS=$'\n\t'

rm -rf protos
mkdir -p ./protos
cp -a ../protos/. ./protos/

docker build -t grpc-ml:latest .

docker rm -f grpc-ml >/dev/null 2>&1 || true
docker run --gpus all -p 8032:8032 --name grpc-ml grpc-ml:latest