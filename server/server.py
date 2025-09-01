import asyncio
import time
import grpc
from concurrent import futures

import vision_pb2 as pb
import vision_pb2_grpc as pb_grpc

class VisionServicer(pb_grpc.VisionServicer):
    async def Detect(self, request, context):
        t0 = time.perf_counter()
        if not request.width or not request.height:
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, "width/height required")
        # Dummy inference: one centered box.
        det = pb.Detection(
            box=pb.Box(x=0.35, y=0.35, w=0.30, h=0.30),  # normalized
            class_id=0,
            score=0.99,
        )
        infer_us = int((time.perf_counter() - t0) * 1e6)
        return pb.DetectResponse(detections=[det])
    
async def serve() -> None:
    server = grpc.aio.server(
        options=[
            ("grpc.max_send_message_length", 100 * 1024 * 1024),
            ("grpc.max_receive_message_length", 100 * 1024 * 1024),
        ]
    )
    pb_grpc.add_VisionServicer_to_server(VisionServicer(), server)
    server.add_insecure_port("[::]:8032")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    try:
        asyncio.run(serve())
    except KeyboardInterrupt:
        pass
