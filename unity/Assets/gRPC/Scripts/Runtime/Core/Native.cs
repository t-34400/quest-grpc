using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
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

    public enum CamRole : int
    {
        LEFT = 0,
        RIGHT = 1
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct Intrinsics
    {
        public float fx;
        public float fy;
        public float cx;
        public float cy;
        public float skew;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct Extrinsics
    {
        public float tx;
        public float ty;
        public float tz;
        public float qx;
        public float qy;
        public float qz;
        public float qw;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CameraRect
    {
        public int left, top, width, height;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CaptureConfig
    {
        public int width;
        public int height;
        public int fps;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct Box
    {
        public float x;
        public float y;
        public float w;
        public float h;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeDetection
    {
        public Box box;
        public int class_id;
        public float score;
    }

    public struct Detection
    {
        public Box Box;
        public int ClassId;
        public float Score;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeResult
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
        public double ReceivedTimeSec;
        public Detection[] Detections;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JpegConfig
    {
        public int jpeg_width;
        public int jpeg_height;
        public int jpeg_quality;
    }

    public static class Native
    {
        private const string LIB = "aiv_plugin";

        private static OnResultCb s_onResultCb;
        private static OnErrorCb s_onErrorCb;
        private static OnFrameSentCb s_onFrameSentCb;

        private static Action<Result> s_onResultManaged;
        private static Action<int, string> s_onErrorManaged;
        private static Action<string, long, double> s_onFrameSentManaged;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void OnResultCb(IntPtr resultPtr);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void OnErrorCb(int code, IntPtr message);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void OnFrameSentCb(IntPtr imageId, long frameIndex, double timestampSec);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern AivStatus AIV_Init(string grpc_target);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern void AIV_Shutdown();

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern void AIV_SetCallbacks(OnResultCb on_result, OnErrorCb on_error, OnFrameSentCb on_frame_sent);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern AivStatus AIV_SetJpegConfig(ref JpegConfig cfg);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern void AIV_GetJpegConfig(out JpegConfig outCfg);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern AivStatus AIV_EnumerateCameras(StringBuilder out_json, int capacity);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern AivStatus AIV_GetCameraIdByPosition(int role, StringBuilder out_camera_id, int cap);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern AivStatus AIV_GetCameraParams(string cam_id, out Intrinsics K, out Extrinsics X, out CameraRect A);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern AivStatus AIV_SetScoreThreshold(float score_threshold);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern AivStatus AIV_SetStereoStreamBaseId(string base_id);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern AivStatus AIV_SetCameraForRole(int role, string cam_id, ref CaptureConfig config);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern AivStatus AIV_StartStreamingStereo();

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern AivStatus AIV_StopStreaming();

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern int AIV_IsStreaming();
        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        private static extern long AIV_GetElapsedRealtimeNanos();

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

        public static AivStatus SetJpegConfig(JpegConfig cfg) => AIV_SetJpegConfig(ref cfg);

        public static JpegConfig GetJpegConfig()
        {
            AIV_GetJpegConfig(out var c);
            return c;
        }

        public static AivStatus EnumerateCameras(out string json)
        {
            var sb = new StringBuilder(4096);
            var st = AIV_EnumerateCameras(sb, sb.Capacity);
            json = st == AivStatus.OK ? sb.ToString() : string.Empty;
            return st;
        }

        public static AivStatus AIV_GetCameraIdByPosition(CamRole role, out string camId)
        {
            var sb = new StringBuilder(128);
            var st = AIV_GetCameraIdByPosition((int)role, sb, sb.Capacity);
            camId = st == AivStatus.OK ? sb.ToString() : string.Empty;
            return st;
        }

        public static AivStatus GetCameraParams(string camId, out Intrinsics K, out Extrinsics X, out CameraRect rect) =>
            AIV_GetCameraParams(camId, out K, out X, out rect);

        public static AivStatus SetScoreThreshold(float v) => AIV_SetScoreThreshold(v);

        public static AivStatus SetStereoStreamBaseId(string baseId) => AIV_SetStereoStreamBaseId(baseId);

        public static AivStatus SetCameraForRole(CamRole role, string camId, CaptureConfig cfg) =>
            AIV_SetCameraForRole((int)role, camId, ref cfg);

        public static AivStatus StartStreamingStereo() => AIV_StartStreamingStereo();

        public static AivStatus StopStreaming() => AIV_StopStreaming();

        public static bool IsStreaming() => AIV_IsStreaming() != 0;

        public static long GetElapsedRealtimeNanos() => AIV_GetElapsedRealtimeNanos();

        [Preserve]
        [MonoPInvokeCallback(typeof(OnResultCb))]
        private static void OnResultNative(IntPtr resultPtr)
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
                    dets[i] = new Detection
                    {
                        Box = nd.box,
                        ClassId = nd.class_id,
                        Score = nd.score
                    };
                }
            }

            var receivedTimeNs = GetElapsedRealtimeNanos();
            var receivedTimeSec = receivedTimeNs / 1e9;

            var r = new Result
            {
                ImageId = imageId,
                FrameIndex = nr.frame_index,
                TimestampSec = nr.timestamp_sec,
                ReceivedTimeSec = receivedTimeSec,
                Detections = dets
            };
            s_onResultManaged?.Invoke(r);
        }

        [Preserve]
        [MonoPInvokeCallback(typeof(OnErrorCb))]
        private static void OnErrorNative(int code, IntPtr msgPtr)
        {
            if (s_onErrorManaged == null) return;

            string msg = PtrToStringUTF8(msgPtr) ?? string.Empty;
            s_onErrorManaged(code, msg);
        }

        [Preserve]
        [MonoPInvokeCallback(typeof(OnFrameSentCb))]
        private static void OnFrameSentNative(IntPtr imageIdPtr, long frameIndex, double ts)
        {
            if (s_onFrameSentManaged == null) return;

            string id = PtrToStringUTF8(imageIdPtr) ?? string.Empty;
            s_onFrameSentManaged(id, frameIndex, ts);
        }

        private static string PtrToStringUTF8(IntPtr ptr)
        {
            if (ptr == IntPtr.Zero) return null;

            var bytes = new List<byte>(64);
            int offset = 0;
            byte b;

            while ((b = Marshal.ReadByte(ptr, offset)) != 0)
            {
                bytes.Add(b);
                offset++;
            }

            return Encoding.UTF8.GetString(bytes.ToArray());
        }
    }
}
