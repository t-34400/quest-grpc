#nullable enable

using System.Threading;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.UI;
using TMPro;
using System.Linq;

namespace Grpc.Sample
{
    public class DetectionOverlay : MonoBehaviour
    {

        [SerializeField] private GrpcStereoRunner runner = null!;
        [SerializeField]
        private List<string> labels = new()
        {
            "none",
            "person",
            "bicycle",
            "car",
            "motorcycle",
            "airplane",
            "bus",
            "train",
            "truck",
            "boat",
            "traffic light",
            "fire hydrant",
            "stop sign",
            "parking meter",
            "bench",
            "bird",
            "cat",
            "dog",
            "horse",
            "sheep",
            "cow",
            "elephant",
            "bear",
            "zebra",
            "giraffe",
            "backpack",
            "umbrella",
            "handbag",
            "tie",
            "suitcase",
            "frisbee",
            "skis",
            "snowboard",
            "sports ball",
            "kite",
            "baseball bat",
            "baseball glove",
            "skateboard",
            "surfboard",
            "tennis racket",
            "bottle",
            "wine glass",
            "cup",
            "fork",
            "knife",
            "spoon",
            "bowl",
            "banana",
            "apple",
            "sandwich",
            "orange",
            "broccoli",
            "carrot",
            "hot dog",
            "pizza",
            "donut",
            "cake",
            "chair",
            "couch",
            "potted plant",
            "bed",
            "dining table",
            "toilet",
            "tv",
            "laptop",
            "mouse",
            "remote",
            "keyboard",
            "cell phone",
            "microwave",
            "oven",
            "toaster",
            "sink",
            "refrigerator",
            "book",
            "clock",
            "vase",
            "scissors",
            "teddy bear",
            "hair drier",
            "toothbrush",
        };

        [Header("Inputs")]
        public Camera MainCamera = null!;
        public float planeDepthMeters = 0.5f;

        [Header("Style")]
        public float rectLineThickness = 0.01f; // meters
        public Color rectColor = new Color(1, 1, 1, 0.8f);
        public Color boxColor = new Color(0, 1, 0, 0.8f);
        public Color labelColor = Color.white;
        public int labelFontSize = 28; // pixels

        class Payload { public Result R; public CameraParams? C; public Payload(Result r, CameraParams? c){ R=r; C=c; } }
        Payload? _latest;

        Canvas _canvas = null!;
        RectTransform _canvasRt = null!;
        RectTransform _imageRt = null!;
        readonly List<GameObject> _pool = new();
        readonly List<RectTransform> _active = new();

        readonly List<double> _tsHist = new List<double>(10);
        readonly List<double> _latHist = new List<double>(10);
        RectTransform _statsRt = null!;
        TextMeshProUGUI _statsTmp = null!;

        void OnEnable()
        {
            if (MainCamera == null) MainCamera = Camera.main;
            runner.ResultReceived += OnResult;
        }

        void OnDisable()
        {
            runner.ResultReceived -= OnResult;
        }

        void OnResult(Result result, CameraParams? cameraParams)
        {
            Interlocked.Exchange(ref _latest, new Payload(result, cameraParams));
        }

        void Update()
        {
            var p = Interlocked.Exchange(ref _latest, null);

#if UNITY_EDITOR
            p = new Payload(
                new Result()
                {
                    ImageId = "Test",
                    FrameIndex = -1,
                    TimestampSec = 1.0,
                    ReceivedTimeSec = 1.1,
                    Detections = new Detection[]
                    {
                        new Detection()
                        {
                            Box = new Box(){ x = 0.25f, y = 0.25f, w = 0.1f, h = 0.3f },
                            ClassId = 1, Score = 1.0f,
                        },
                        new Detection()
                        {
                            Box = new Box(){ x = 0.65f, y = 0.5f, w = 0.2f, h = 0.2f },
                            ClassId = 2, Score = 0.85f,
                        }
                    }
                },
                new CameraParams()
                {
                    Rect = new CameraRect(){ left = 0, top = 0, width = 1280, height = 960 },
                    K = new Intrinsics(){ fx = 800f, fy = 800f, cx = 640f, cy = 480f, skew = 0f },
                    Pose = new Pose(){ Position = new Vector3(0, 0, 0), Rotation = Quaternion.identity },
                }
            );
#endif

            if (p != null)
            {
                ProcessResultOnMainThread(p.R, p.C);
            }
        }

        static Matrix4x4 TRS(Pose p) => Matrix4x4.TRS(p.Position, p.Rotation, Vector3.one);

        static void Decompose(Matrix4x4 m, out Vector3 pos, out Quaternion rot)
        {
            pos = m.GetColumn(3);
            rot = Quaternion.LookRotation(m.GetColumn(2), m.GetColumn(1));
        }

        void EnsureCanvas()
        {
            if (_canvas != null) return;

            var go = new GameObject("DetectionsCanvas");
            _canvas = go.AddComponent<Canvas>();
            _canvas.renderMode = RenderMode.WorldSpace;
            _canvas.worldCamera = MainCamera;
            _canvas.sortingOrder = 5000;

            _canvasRt = _canvas.GetComponent<RectTransform>();
            _canvasRt.pivot = new Vector2(0.5f, 0.5f);
            _canvasRt.anchorMin = _canvasRt.anchorMax = new Vector2(0.5f, 0.5f);

            var scaler = go.AddComponent<CanvasScaler>();
            scaler.dynamicPixelsPerUnit = 1f;

            var imgGo = new GameObject("ImageRect");
            imgGo.transform.SetParent(_canvas.transform, false);
            _imageRt = imgGo.AddComponent<RectTransform>();
            _imageRt.pivot = new Vector2(0.5f, 0.5f);
            _imageRt.anchorMin = _imageRt.anchorMax = new Vector2(0.5f, 0.5f);

            var fill = imgGo.AddComponent<Image>();
            fill.sprite = Sprite.Create(Texture2D.whiteTexture, new Rect(0,0,1,1), new Vector2(0.5f,0.5f));
            fill.color = new Color(1,1,1,0);
            fill.raycastTarget = false;
        }

        GameObject GetPooled(string name, Transform parent)
        {
            GameObject go = null!;
            for (int i = 0; i < _pool.Count; i++)
            {
                if (!_pool[i].activeSelf)
                {
                    go = _pool[i];
                    break;
                }
            }
            if (go == null)
            {
                go = new GameObject(name);
                _pool.Add(go);
            }
            go.name = name;
            go.transform.SetParent(parent, false);
            go.SetActive(true);
            return go;
        }

        void ClearActive()
        {
            for (int i = 0; i < _active.Count; i++)
                if (_active[i]) _active[i].gameObject.SetActive(false);
            _active.Clear();
        }

        void PushFixed(List<double> list, double v, int max)
        {
            list.Add(v);
            if (list.Count > max) list.RemoveAt(0);
        }

        void UpdateStats(Result r)
        {
            double lat = r.ReceivedTimeSec - r.TimestampSec;
            PushFixed(_latHist, lat, 10);
            PushFixed(_tsHist,  r.TimestampSec, 10);
        }

        double ComputeHz(List<double> ts)
        {
            if (ts.Count < 2) return 0.0;
            double sum = 0.0; int n = 0;
            for (int i = 1; i < ts.Count; i++)
            {
                double d = ts[i] - ts[i - 1];
                if (d > 0) { sum += d; n++; }
            }
            if (n == 0) return 0.0;
            double meanDt = sum / n;
            return 1.0 / meanDt;
        }

        void EnsureStatsLabel()
        {
            if (_statsTmp != null) return;

            Transform t = _imageRt.Find("Stats");
            GameObject go;
            if (t != null) go = t.gameObject; else go = new GameObject("Stats");
            if (go.transform.parent != _imageRt) go.transform.SetParent(_imageRt, false);

            _statsRt = go.GetComponent<RectTransform>();
            if (_statsRt == null) _statsRt = go.AddComponent<RectTransform>();
            _statsRt.pivot = new Vector2(0f, 1f);
            _statsRt.anchorMin = new Vector2(0f, 1f);
            _statsRt.anchorMax = new Vector2(0f, 1f);

            _statsTmp = go.GetComponent<TextMeshProUGUI>();
            if (_statsTmp == null) _statsTmp = go.AddComponent<TextMeshProUGUI>();
            _statsTmp.fontSize = Mathf.Max(18, labelFontSize - 4);
            _statsTmp.color = labelColor;
            _statsTmp.textWrappingMode = TextWrappingModes.NoWrap;
            _statsTmp.raycastTarget = false;
            _statsTmp.alignment = TextAlignmentOptions.TopLeft;
        }

        void ProcessResultOnMainThread(Result res, CameraParams? camOpt)
        {
            UpdateStats(res);

            if (camOpt == null)
            {
                Debug.LogError("CameraParams is null.");
                return;
            }
            var cam = camOpt.Value;

            EnsureCanvas();
            EnsureStatsLabel();

            var W_from_main = Matrix4x4.TRS(MainCamera.transform.position, MainCamera.transform.rotation, Vector3.one);
            var main_from_camera = TRS(new Pose { Position = cam.Pose.Position, Rotation = cam.Pose.Rotation });
            var W_from_camera = W_from_main * main_from_camera;
            Decompose(W_from_camera, out var camPosW, out var camRotW);

            float z = Mathf.Max(0.001f, planeDepthMeters);
            float width_m = z * (float)cam.Rect.width / cam.K.fx;
            float height_m = z * (float)cam.Rect.height / cam.K.fy;

            float u_c = cam.Rect.left + cam.Rect.width * 0.5f;
            float v_c = cam.Rect.top + cam.Rect.height * 0.5f;
            float x_c = (u_c - cam.K.cx) / cam.K.fx * z;
            float y_c = (v_c - cam.K.cy) / cam.K.fy * z;

            var centerLocal = new Vector3(x_c, y_c, z);
            var centerWorld = W_from_camera.MultiplyPoint3x4(centerLocal);

            float imgWpx = (float)cam.Rect.width;
            float imgHpx = (float)cam.Rect.height;
            float metersPerPixelX = width_m / imgWpx;
            float metersPerPixelY = height_m / imgHpx;
            float metersPerPixel = (metersPerPixelX + metersPerPixelY) * 0.5f;

            _canvasRt.position = centerWorld;
            _canvasRt.rotation = camRotW;
            _canvasRt.sizeDelta = new Vector2(imgWpx, imgHpx);
            _canvasRt.localScale = new Vector3(metersPerPixel, metersPerPixel, 1f);

            _imageRt.anchoredPosition = Vector2.zero;
            _imageRt.sizeDelta = new Vector2(imgWpx, imgHpx);

            int borderPx = Mathf.Max(1, Mathf.RoundToInt(rectLineThickness / metersPerPixel));
            BorderUtil.ApplyBorder(_imageRt, borderPx, rectColor);

            double currLat = _latHist.Count > 0 ? _latHist[_latHist.Count - 1] : 0.0;
            double avgLat = 0.0;
            for (int i = 0; i < _latHist.Count; i++) avgLat += _latHist[i];
            if (_latHist.Count > 0) avgLat /= _latHist.Count;
            double hz = ComputeHz(_tsHist);

            float padding = 30f;
            _statsRt.sizeDelta = new Vector2(imgWpx * 0.6f, 0f);
            _statsRt.anchoredPosition = new Vector2(padding, -padding);
            _statsTmp.text =
                $"Latency: {currLat * 1000.0:0.0} ms  (avg {avgLat * 1000.0:0.0} ms, n={_latHist.Count})\n" +
                $"Rate: {hz:0.0} Hz";

            ClearActive();

            if (res.Detections == null) return;

            foreach (var det in res.Detections)
            {
                var boxGo = GetPooled("det", _imageRt);
                var rt = boxGo.GetComponent<RectTransform>();
                if (rt == null) rt = boxGo.AddComponent<RectTransform>();
                rt.pivot = new Vector2(0.5f, 0.5f);
                rt.anchorMin = rt.anchorMax = new Vector2(0.5f, 0.5f);

                var img = boxGo.GetComponent<Image>();
                if (img == null) img = boxGo.AddComponent<Image>();
                if (img.sprite == null) img.sprite = Sprite.Create(Texture2D.whiteTexture, new Rect(0,0,1,1), new Vector2(0.5f,0.5f));
                img.color = new Color(1,1,1,0);
                img.raycastTarget = false;

                float bx_px = (det.Box.x - 0.5f) * imgWpx;
                float by_px = (0.5f - det.Box.y) * imgHpx;
                float bw_px = det.Box.w * imgWpx;
                float bh_px = det.Box.h * imgHpx;

                rt.anchoredPosition = new Vector2(bx_px, by_px);
                rt.sizeDelta = new Vector2(bw_px, bh_px);

                int linePx = Mathf.Max(1, Mathf.RoundToInt(rectLineThickness / metersPerPixel));
                BorderUtil.ApplyBorder(rt, linePx, boxColor);

                var labelGo = GetPooled("label", rt);
                var lrt = labelGo.GetComponent<RectTransform>();
                if (lrt == null) lrt = labelGo.AddComponent<RectTransform>();
                lrt.pivot = new Vector2(0, 1);
                lrt.anchorMin = lrt.anchorMax = new Vector2(0, 1);
                lrt.anchoredPosition = new Vector2(-bw_px * 0.5f, -bh_px * 0.5f - linePx * 2f);

                var label = labels.ElementAtOrDefault(det.ClassId);

                var tmp = labelGo.GetComponent<TextMeshProUGUI>();
                if (tmp == null) tmp = labelGo.AddComponent<TextMeshProUGUI>();
                tmp.fontSize = labelFontSize;
                tmp.color = labelColor;
                tmp.text = $"{label} ({det.Score:0.00})";

                _active.Add(rt);
                _active.Add(lrt);
            }
        }
    }
}
