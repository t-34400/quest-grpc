#!/bin/bash

set -euo pipefail
IFS=$'\n\t'

docker build -t grpc-ml:latest .

docker run --rm -p 8032:8032 --name grpc-ml grpc-ml:latest
