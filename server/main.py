import asyncio

import grpc
import vision_pb2_grpc as pb_grpc


async def serve() -> None:
    server = grpc.aio.server(options=[
        ("grpc.max_send_message_length", 100 * 1024 * 1024),
        ("grpc.max_receive_message_length", 100 * 1024 * 1024),
        ("grpc.keepalive_time_ms", 15000),
        ("grpc.keepalive_timeout_ms", 5000),
        ("grpc.keepalive_permit_without_calls", 1),
    ])

    test = False
    if test:
        from test_server import TestVisionServicer
        pb_grpc.add_VisionServicer_to_server(TestVisionServicer(), server)
    else:
        from vision_server import VisionServicer
        pb_grpc.add_VisionServicer_to_server(VisionServicer(), server)

    server.add_insecure_port("0.0.0.0:8032")
    server.add_insecure_port("[::]:8032")

    await server.start()
    try:
        await server.wait_for_termination()
    finally:
        await server.stop(grace=5)


if __name__ == "__main__":
    asyncio.run(serve())
