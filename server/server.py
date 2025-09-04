import asyncio
import json
import os
import re
import time
from pathlib import Path

import grpc
import vision_pb2 as pb
import vision_pb2_grpc as pb_grpc


SAVE_ROOT = Path(os.environ.get("AIV_SAVE_DIR", "./received")).resolve()


def _safe(s: str) -> str:
    # Avoid invalid path characters.
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", s)[:128] or "unk"


class VisionServicer(pb_grpc.VisionServicer):
    async def Detect(self, request, context):
        if not request.width or not request.height:
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, "width/height required")
        det = pb.Detection(box=pb.Box(x=0.35, y=0.35, w=0.30, h=0.30), class_id=0, score=0.99)
        return pb.DetectResponse(detections=[det])

    async def StreamDetect(self, request_iterator, context):
        frame_count = 0
        async for req in request_iterator:
            frame_count += 1
            print(
                f"[recv] #{frame_count} "
                f"stream_id={req.stream_id} camera_id={req.camera_id} "
                f"idx={req.frame_index} ts_ns={req.timestamp_ns} "
                f"size={req.width}x{req.height} fmt={req.format} data_len={len(req.data)}"
            )

            # Persist JPEG and minimal metadata.
            sid = _safe(req.stream_id) or "default"
            cid = _safe(req.camera_id) or "cam"
            d = SAVE_ROOT / sid / cid
            d.mkdir(parents=True, exist_ok=True)
            base = f"img_{int(req.frame_index)}_{int(req.timestamp_ns)}"
            jpg_path = d / f"{base}.jpg"
            meta_path = d / f"{base}.json"

            try:
                with open(jpg_path, "wb") as f:
                    f.write(req.data)
                meta = {
                    "stream_id": req.stream_id,
                    "camera_id": req.camera_id,
                    "frame_index": int(req.frame_index),
                    "timestamp_ns": int(req.timestamp_ns),
                    "width": int(req.width),
                    "height": int(req.height),
                    "format": int(req.format),
                    "saved_at": time.time(),
                    "jpeg_path": str(jpg_path),
                }
                with open(meta_path, "w", encoding="utf-8") as f:
                    json.dump(meta, f, ensure_ascii=False, indent=2)
            except Exception as e:
                # Do not abort the stream on I/O errors; just report.
                print(f"[error] failed to save frame #{frame_count}: {e}")

            det = pb.Detection(box=pb.Box(x=0.35, y=0.35, w=0.30, h=0.30), class_id=0, score=0.99)
            res = pb.Result(
                frame_index=req.frame_index,
                timestamp_ns=req.timestamp_ns,
                detections=[det],
            )
            yield res


async def serve() -> None:
    server = grpc.aio.server(options=[
        ("grpc.max_send_message_length", 100 * 1024 * 1024),
        ("grpc.max_receive_message_length", 100 * 1024 * 1024),
        ("grpc.keepalive_time_ms", 15000),
        ("grpc.keepalive_timeout_ms", 5000),
        ("grpc.keepalive_permit_without_calls", 1),
    ])
    pb_grpc.add_VisionServicer_to_server(VisionServicer(), server)

    server.add_insecure_port("0.0.0.0:8032")
    server.add_insecure_port("[::]:8032")

    SAVE_ROOT.mkdir(parents=True, exist_ok=True)
    print(f"Vision server listening on :8032; saving to {SAVE_ROOT}")

    await server.start()
    try:
        await server.wait_for_termination()
    finally:
        await server.stop(grace=5)


if __name__ == "__main__":
    asyncio.run(serve())
