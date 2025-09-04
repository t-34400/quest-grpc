using UnityEngine;

namespace Grpc
{
  public class GrpcStereoSender : MonoBehaviour
  {
    [Header("gRPC")]
    [SerializeField] string host = "127.0.0.1";
    [SerializeField] int port = 8032;
    [SerializeField] string baseStreamId = "unity_stream";
    [SerializeField] bool autoStart = true;

    [Header("Left Camera")]
    [SerializeField] bool enableLeftCamStreaming = true;
    [SerializeField] int leftPositionValue = 0;
    [SerializeField] int leftWidth = 640;
    [SerializeField] int leftHeight = 480;
    [SerializeField] int leftFps = 30;

    [Header("Right Camera")]
    [SerializeField] bool enableRightCamStreaming = true;
    [SerializeField] int rightPositionValue = 1;
    [SerializeField] int rightWidth = 640;
    [SerializeField] int rightHeight = 480;
    [SerializeField] int rightFps = 30;

    [Header("Runtime Tuning")]
    [SerializeField] float scoreThreshold = 0.0f;
    [SerializeField] string imageIdPrefix = "img";
    [SerializeField] int jpegWidth = 0;     // 0 = capture size
    [SerializeField] int jpegHeight = 0;    // 0 = capture size
    [SerializeField] int jpegQuality = 70;  // 1..100

    void Start()
    {
      var st = Native.Init($"{host}:{port}");
      Debug.Log($"Init: {st}");
      if (st != AivStatus.OK) return;

      Native.SetCallbacks(
        onResult: r =>
        {
          Debug.Log($"Result: id={r.ImageId} det={r.Detections.Length}");
          for (int i = 0; i < r.Detections.Length; ++i)
          {
            var d = r.Detections[i];
            Debug.Log($"  [{i}] box=({d.Box.x},{d.Box.y},{d.Box.w},{d.Box.h}) cls={d.ClassId} score={d.Score}");
          }
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
      Native.SetImageIdPrefix(imageIdPrefix);

      var jc = new JpegConfig { jpeg_width = jpegWidth, jpeg_height = jpegHeight, jpeg_quality = Mathf.Clamp(jpegQuality, 1, 100) };
      Native.SetJpegConfig(jc);

      string camJson;
      var est = Native.EnumerateCameras(out camJson);
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
      }

      if (enableRightCamStreaming && ResolveCameraId(CamRole.RIGHT, out var resolvedRightId))
      {
        var cfgR = new CaptureConfig { width = rightWidth, height = rightHeight, fps = rightFps };
        var stR = Native.SetCameraForRole(CamRole.RIGHT, resolvedRightId, cfgR);
        Debug.Log($"SetCameraForRole RIGHT: {stR} id={resolvedRightId}");
        if (stR != AivStatus.OK) Debug.LogError("Failed to set RIGHT camera.");
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

    void OnDestroy()
    {
      if (Native.IsStreaming()) StopSending();
      Native.Shutdown();
    }

    bool ResolveCameraId(CamRole role, out string resolvedId)
    {
      var result = Native.AIV_GetCameraIdByPosition(role, out resolvedId);
      return result == AivStatus.OK;
    }
  }
}
