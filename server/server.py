import asyncio
import time
import grpc

import vision_pb2 as pb
import vision_pb2_grpc as pb_grpc

class VisionServicer(pb_grpc.VisionServicer):
    async def Detect(self, request, context):
        if not request.width or not request.height:
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, "width/height required")
        det = pb.Detection(box=pb.Box(x=0.35, y=0.35, w=0.30, h=0.30), class_id=0, score=0.99)
        return pb.DetectResponse(detections=[det])

async def serve() -> None:
    server = grpc.aio.server(options=[
        ("grpc.max_send_message_length", 100 * 1024 * 1024),
        ("grpc.max_receive_message_length", 100 * 1024 * 1024),
    ])
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
