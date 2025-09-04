#nullable enable

using System;
using System.Linq;
using UnityEngine;

namespace Grpc
{
    public struct Pose
    {
        public Vector3 Position { get; set; }
        public Quaternion Rotation { get; set; }
    }

    public struct CameraParams
    {
        public CameraRect Rect { get; set; }
        public Intrinsics K { get; set; }
        public Pose Pose { get; set; }
    }

    public class GrpcStereoRunner : MonoBehaviour
    {
        [Header("gRPC")]
        [SerializeField] private string host = "127.0.0.1";
        [SerializeField] private int port = 8032;
        [SerializeField] private string baseStreamId = "unity_stream";
        [SerializeField] private bool autoStart = true;

        [Header("Left Camera")]
        [SerializeField] private bool enableLeftCamStreaming = true;
        [SerializeField] private int leftWidth = 640;
        [SerializeField] private int leftHeight = 480;
        [SerializeField] private int leftFps = 30;

        [Header("Right Camera")]
        [SerializeField] private bool enableRightCamStreaming = true;
        [SerializeField] private int rightWidth = 640;
        [SerializeField] private int rightHeight = 480;
        [SerializeField] private int rightFps = 30;

        [Header("Runtime Tuning")]
        [SerializeField] private float scoreThreshold = 0.0f;
        [SerializeField] private int jpegWidth = 0;      // 0 = capture size
        [SerializeField] private int jpegHeight = 0;     // 0 = capture size
        [SerializeField] private int jpegQuality = 70;   // 1..100

        public CameraParams? LeftCameraParams { get; set; } = null;
        public CameraParams? RightCameraParams { get; set; } = null;

        public event Action<Result, CameraParams?>? ResultReceived;

        private void Start()
        {
            var st = Native.Init($"{host}:{port}");
            Debug.Log($"Init: {st}");
            if (st != AivStatus.OK) return;

            Native.SetCallbacks(
                onResult: r =>
                {
                    Debug.Log($"Result: id={r.ImageId} det={r.Detections.Length}, ts={r.TimestampSec}, recv-ts={r.ReceivedTimeSec}");
                    for (int i = 0; i < r.Detections.Length; ++i)
                    {
                        var d = r.Detections[i];
                        Debug.Log($"  [{i}] box=({d.Box.x},{d.Box.y},{d.Box.w},{d.Box.h}) cls={d.ClassId} score={d.Score}");
                    }

                    ResultReceived?.Invoke(r, GetCameraParamsByStreamId(r.ImageId, baseStreamId));
                },
                onError: (code, msg) =>
                {
                    Debug.LogError($"Error {code}: {msg}");
                },
                onFrameSent: (id, idx, ts) =>
                {
                    Debug.Log($"FrameSent: {id} idx={idx} ts={ts:F6}");
                }
            );

            Native.SetScoreThreshold(scoreThreshold);

            var jc = new JpegConfig
            {
                jpeg_width = jpegWidth,
                jpeg_height = jpegHeight,
                jpeg_quality = Mathf.Clamp(jpegQuality, 1, 100)
            };
            Native.SetJpegConfig(jc);

            var est = Native.EnumerateCameras(out var camJson);
            Debug.Log($"Enumerate: {est} json={camJson}");

            if (autoStart) StartSending();
        }

        public void StartSending()
        {
            if (Native.IsStreaming()) return;

            Native.SetStereoStreamBaseId(baseStreamId);

            if (enableLeftCamStreaming && ResolveCameraId(CamRole.LEFT, out var resolvedLeftId))
            {
                var cfgL = new CaptureConfig { width = leftWidth, height = leftHeight, fps = leftFps };
                var stL = Native.SetCameraForRole(CamRole.LEFT, resolvedLeftId, cfgL);
                Debug.Log($"SetCameraForRole LEFT: {stL} id={resolvedLeftId}");
                if (stL != AivStatus.OK) Debug.LogError("Failed to set LEFT camera.");

                LeftCameraParams = GetCameraParams(resolvedLeftId);
            }

            if (enableRightCamStreaming && ResolveCameraId(CamRole.RIGHT, out var resolvedRightId))
            {
                var cfgR = new CaptureConfig { width = rightWidth, height = rightHeight, fps = rightFps };
                var stR = Native.SetCameraForRole(CamRole.RIGHT, resolvedRightId, cfgR);
                Debug.Log($"SetCameraForRole RIGHT: {stR} id={resolvedRightId}");
                if (stR != AivStatus.OK) Debug.LogError("Failed to set RIGHT camera.");

                RightCameraParams = GetCameraParams(resolvedRightId);
            }

            var st = Native.StartStreamingStereo();
            Debug.Log($"StartStreamingStereo: {st}");
            if (st != AivStatus.OK) Debug.LogError("StartStreamingStereo failed.");
        }

        public void StopSending()
        {
            if (!Native.IsStreaming()) return;

            var st = Native.StopStreaming();
            Debug.Log($"StopStreaming: {st}");
        }

        private void OnDestroy()
        {
            if (Native.IsStreaming()) StopSending();
            Native.Shutdown();
        }

        private bool ResolveCameraId(CamRole role, out string resolvedId)
        {
            var result = Native.AIV_GetCameraIdByPosition(role, out resolvedId);
            return result == AivStatus.OK;
        }

        private CameraParams? GetCameraParamsByStreamId(string streamId, string baseStreamId)
        {
            if (!streamId.StartsWith(baseStreamId)) return null;

            var roleString = streamId.Skip(baseStreamId.Length + 1).Take(4).ToString().ToUpper();
            if (roleString.Equals("LEFT"))
                return LeftCameraParams;

            return RightCameraParams;            
        }

        private static CameraParams? GetCameraParams(string cameraId)
        {
            var stCamPrmL = Native.GetCameraParams(cameraId, out var K, out var X, out var A);
            if (stCamPrmL == AivStatus.OK)
            {
                return new CameraParams()
                {
                    Rect = A,
                    K = K,
                    Pose = ConvertExtrinsicToPose(X),
                };
            }
            else
            {
                Debug.LogError($"Failed to get camera params for ID={cameraId}.");
                return null;
            }
        }

        private static Pose ConvertExtrinsicToPose(Extrinsics X)
        {
            var pos_c2h = new Vector3(X.tx, X.ty, -X.tz);

            var rot_h2c = new Quaternion(-X.qx, -X.qy, X.qz, X.qw);
            var rot_c2h = Quaternion.Inverse(rot_h2c);
            rot_c2h *= Quaternion.Euler(180f, 0f, 0f);

            return new Pose()
            {
                Position = pos_c2h,
                Rotation = rot_c2h,
            };
        }
    }
}
