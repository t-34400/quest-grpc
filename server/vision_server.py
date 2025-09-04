import os

import grpc
import numpy as np
import cv2
import onnxruntime as ort

import vision_pb2 as pb
import vision_pb2_grpc as pb_grpc

MODEL_PATH = os.getenv("MODEL_PATH", "/app/model.onnx")


def _imdecode_rgb(jpeg_bytes: bytes):
    arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        raise RuntimeError("Failed to decode image")
    img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    return img


def _preprocess(img_rgb: np.ndarray, size=(640, 640)):
    h0, w0 = img_rgb.shape[:2]
    img = cv2.resize(img_rgb, size, interpolation=cv2.INTER_LINEAR)
    x = (img.astype(np.float32) / 255.0).transpose(2, 0, 1)[None, ...].copy()
    orig = np.array([[h0, w0]], dtype=np.int64)
    return x, orig, (h0, w0)


def _to_center_xywh(xyxy: np.ndarray):
    x1, y1, x2, y2 = xyxy
    w = max(0.0, x2 - x1)
    h = max(0.0, y2 - y1)
    cx = x1 + w * 0.5
    cy = y1 + h * 0.5
    return np.array([cx, cy, w, h], dtype=np.float32)


class VisionServicer(pb_grpc.VisionServicer):
    def __init__(self):
        super().__init__()
        prov = ["CUDAExecutionProvider", "CPUExecutionProvider"]
        self.session = ort.InferenceSession(MODEL_PATH, providers=prov)

        self.in_images = "images"
        self.in_orig = "orig_target_sizes"

        self.out_labels = "labels"
        self.out_boxes = "boxes"
        self.out_scores = "scores"

        print("Vision server ready on :8032 (ONNX Runtime)")

    def _run_onnx(self, img_bytes: bytes):
        img = _imdecode_rgb(img_bytes)
        x, orig, (h0, w0) = _preprocess(img, (640, 640))

        feeds = {
            self.in_images: x,                # float32 [1,3,640,640]
            self.in_orig: orig               # int64   [1,2] (h,w)
        }
        outs = self.session.run([self.out_labels, self.out_boxes, self.out_scores], feeds)

        labels, boxes, scores = outs

        if boxes.ndim == 3 and boxes.shape[0] == 1:
            boxes = boxes[0]
        if scores.ndim == 2 and scores.shape[0] == 1:
            scores = scores[0]
        if labels.ndim >= 2 and labels.shape[0] == 1:
            labels = labels[0]

        if boxes.ndim != 2 or boxes.shape[-1] != 4:
            raise RuntimeError("Unexpected boxes shape")

        if scores.ndim != 1 and scores.ndim != 2:
            raise RuntimeError("Unexpected scores shape")

        if scores.ndim == 2 and scores.shape[0] == 1:
            scores = scores[0]
        if labels.ndim == 2 and labels.shape[0] == 1:
            labels = labels[0]

        conf_th = float(os.environ.get("AIV_SCORE_TH", "0.25"))
        topk = int(os.environ.get("AIV_TOPK", "100"))

        if scores.ndim == 1:
            conf = scores
        else:
            conf = scores.max(axis=1)

        keep = np.where(conf >= conf_th)[0]
        if keep.size == 0:
            return []

        conf = conf[keep]
        boxes = boxes[keep]

        if labels.ndim == 1:
            cls_ids = labels[keep].astype(np.int64)
        else:
            cls_ids = labels[keep].argmax(axis=1).astype(np.int64)

        order = np.argsort(-conf)[:topk]
        boxes, conf, cls_ids = boxes[order], conf[order], cls_ids[order]

        maxval = float(np.max(boxes))
        if maxval <= 1.5:
            abs_boxes = boxes * np.array([w0, h0, w0, h0], dtype=np.float32)
        else:
            abs_boxes = boxes.astype(np.float32)

        xyxy_like = np.mean(abs_boxes[:, 2] > abs_boxes[:, 0]) > 0.8 and np.mean(abs_boxes[:, 3] > abs_boxes[:, 1]) > 0.8
        out = []
        for b, s, c in zip(abs_boxes, conf, cls_ids):
            if xyxy_like:
                cx, cy, w, h = _to_center_xywh(b)
            else:
                cx, cy, w, h = b
            x = float(np.clip(cx / max(w0, 1), 0.0, 1.0))
            y = float(np.clip(cy / max(h0, 1), 0.0, 1.0))
            wn = float(np.clip(w / max(w0, 1), 0.0, 1.0))
            hn = float(np.clip(h / max(h0, 1), 0.0, 1.0))
            out.append(pb.Detection(box=pb.Box(x=x, y=y, w=wn, h=hn), class_id=int(c), score=float(s)))
        return out

    async def Detect(self, request, context):
        if not request.width or not request.height:
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, "width/height required")
        if not request.data:
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, "image data required")
        try:
            dets = self._run_onnx(request.data)
            return pb.DetectResponse(detections=dets)
        except Exception as e:
            await context.abort(grpc.StatusCode.INTERNAL, f"inference failed: {e}")

    async def StreamDetect(self, request_iterator, context):
        frame_count = 0
        async for req in request_iterator:
            frame_count += 1
            try:
                dets = self._run_onnx(req.data)
                res = pb.Result(
                    stream_id=req.stream_id,
                    frame_index=req.frame_index,
                    timestamp_ns=req.timestamp_ns,
                    detections=dets,
                )
                yield res
            except Exception as e:
                print(f"[error] inference failed at frame #{frame_count}: {e}")
                yield pb.Result(
                    frame_index=req.frame_index,
                    timestamp_ns=req.timestamp_ns,
                    detections=[],
                )
