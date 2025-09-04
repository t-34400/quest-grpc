using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
using AOT;
using UnityEngine.Scripting;

namespace Grpc
{
  public enum AivStatus : int
  {
    OK = 0,
    ERR_INVALID_ARG = -1,
    ERR_NOT_INITIALIZED = -2,
    ERR_ALREADY_RUNNING = -3,
    ERR_NOT_RUNNING = -4,
    ERR_CAMERA_OPEN = -5,
    ERR_CAMERA_PARAM = -6,
    ERR_GRPC = -7,
    ERR_INTERNAL = -9
  }

  [StructLayout(LayoutKind.Sequential, Pack = 8)]
  public struct Intrinsics { public float fx, fy, cx, cy, skew; }

  [StructLayout(LayoutKind.Sequential, Pack = 8)]
  public struct Extrinsics { public float tx, ty, tz, qx, qy, qz, qw; }

  [StructLayout(LayoutKind.Sequential)]
  public struct CaptureConfig { public int width; public int height; public int fps; }

  [StructLayout(LayoutKind.Sequential, Pack = 8)]
  public struct Box { public float x, y, w, h; }

  [StructLayout(LayoutKind.Sequential)]
  struct NativeDetection { public Box box; public int class_id; public float score; }

  public struct Detection { public Box Box; public int ClassId; public float Score; }

  [StructLayout(LayoutKind.Sequential)]
  struct NativeResult
  {
    public IntPtr image_id;
    public long frame_index;
    public double timestamp_sec;
    public IntPtr detections;
    public int detection_count;
  }

  public struct Result
  {
    public string ImageId;
    public long FrameIndex;
    public double TimestampSec;
    public Detection[] Detections;
  }

  public static class Native
  {
    const string LIB = "aiv_plugin";

    static OnResultCb s_onResultCb;
    static OnErrorCb s_onErrorCb;
    static OnFrameSentCb s_onFrameSentCb;

    static Action<Result> s_onResultManaged;
    static Action<int, string> s_onErrorManaged;
    static Action<string, long, double> s_onFrameSentManaged;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate void OnResultCb(IntPtr resultPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate void OnErrorCb(int code, IntPtr message);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate void OnFrameSentCb(IntPtr imageId, long frameIndex, double timestampSec);

    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    static extern AivStatus AIV_Init(string grpc_target);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern void AIV_Shutdown();
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern void AIV_SetCallbacks(OnResultCb on_result, OnErrorCb on_error, OnFrameSentCb on_frame_sent);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    static extern AivStatus AIV_EnumerateCameras(StringBuilder out_json, int capacity);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern AivStatus AIV_OpenCameraByPosition(int position_value, ref CaptureConfig config, StringBuilder out_cam_id, int cam_id_capacity);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    static extern AivStatus AIV_OpenCameraById(string cam_id, ref CaptureConfig config);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern AivStatus AIV_GetCameraParams([MarshalAs(UnmanagedType.LPStr)] string cam_id, out Intrinsics K, out Extrinsics X);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern AivStatus AIV_SetScoreThreshold(float score_threshold);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    static extern AivStatus AIV_SetImageIdPrefix(string prefix);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    static extern AivStatus AIV_StartStreaming(string cam_id, string stream_id);
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern AivStatus AIV_StopStreaming();
    [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
    static extern int AIV_IsStreaming();

    public static AivStatus Init(string grpcTarget) => AIV_Init(grpcTarget);
    public static void Shutdown() => AIV_Shutdown();

    public static void SetCallbacks(Action<Result> onResult, Action<int, string> onError, Action<string, long, double> onFrameSent)
    {
      s_onResultManaged = onResult;
      s_onErrorManaged = onError;
      s_onFrameSentManaged = onFrameSent;

      s_onResultCb = OnResultNative;
      s_onErrorCb = OnErrorNative;
      s_onFrameSentCb = OnFrameSentNative;

      AIV_SetCallbacks(s_onResultCb, s_onErrorCb, s_onFrameSentCb);
    }

    public static AivStatus EnumerateCameras(out string json)
    {
      var sb = new StringBuilder(4096);
      var st = AIV_EnumerateCameras(sb, sb.Capacity);
      json = st == AivStatus.OK ? sb.ToString() : "";
      return st;
    }

    public static AivStatus OpenCameraByPosition(int positionValue, CaptureConfig cfg, out string camId)
    {
      var sb = new StringBuilder(128);
      var st = AIV_OpenCameraByPosition(positionValue, ref cfg, sb, sb.Capacity);
      camId = st == AivStatus.OK ? sb.ToString() : "";
      return st;
    }

    public static AivStatus OpenCameraById(string camId, CaptureConfig cfg) => AIV_OpenCameraById(camId, ref cfg);
    public static AivStatus GetCameraParams(string camId, out Intrinsics K, out Extrinsics X) => AIV_GetCameraParams(camId, out K, out X);
    public static AivStatus SetScoreThreshold(float v) => AIV_SetScoreThreshold(v);
    public static AivStatus SetImageIdPrefix(string prefix) => AIV_SetImageIdPrefix(prefix);
    public static AivStatus StartStreaming(string camId, string streamId) => AIV_StartStreaming(camId, streamId);
    public static AivStatus StopStreaming() => AIV_StopStreaming();
    public static bool IsStreaming() => AIV_IsStreaming() != 0;

    [Preserve]
    [MonoPInvokeCallback(typeof(OnResultCb))]
    static void OnResultNative(IntPtr resultPtr)
    {
      if (s_onResultManaged == null || resultPtr == IntPtr.Zero) return;
      var nr = Marshal.PtrToStructure<NativeResult>(resultPtr);
      string imageId = PtrToStringUTF8(nr.image_id);

      var dets = new Detection[Math.Max(0, nr.detection_count)];
      if (nr.detections != IntPtr.Zero && nr.detection_count > 0)
      {
        int sz = Marshal.SizeOf<NativeDetection>();
        for (int i = 0; i < nr.detection_count; ++i)
        {
          var ptr = nr.detections + i * sz;
          var nd = Marshal.PtrToStructure<NativeDetection>(ptr);
          dets[i] = new Detection { Box = nd.box, ClassId = nd.class_id, Score = nd.score };
        }
      }

      var r = new Result { ImageId = imageId, FrameIndex = nr.frame_index, TimestampSec = nr.timestamp_sec, Detections = dets };
      s_onResultManaged?.Invoke(r);
    }

    [Preserve]
    [MonoPInvokeCallback(typeof(OnErrorCb))]
    static void OnErrorNative(int code, IntPtr msgPtr)
    {
      if (s_onErrorManaged == null) return;
      string msg = PtrToStringUTF8(msgPtr) ?? "";
      s_onErrorManaged(code, msg);
    }

    [Preserve]
    [MonoPInvokeCallback(typeof(OnFrameSentCb))]
    static void OnFrameSentNative(IntPtr imageIdPtr, long frameIndex, double ts)
    {
      if (s_onFrameSentManaged == null) return;
      string id = PtrToStringUTF8(imageIdPtr) ?? "";
      s_onFrameSentManaged(id, frameIndex, ts);
    }

    static string PtrToStringUTF8(IntPtr ptr)
    {
      if (ptr == IntPtr.Zero) return null;
      var bytes = new List<byte>(64);
      int offset = 0;
      byte b;
      while ((b = Marshal.ReadByte(ptr, offset)) != 0) { bytes.Add(b); offset++; }
      return Encoding.UTF8.GetString(bytes.ToArray());
    }
  }
}
