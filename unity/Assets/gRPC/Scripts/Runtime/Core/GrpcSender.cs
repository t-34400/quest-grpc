using UnityEngine;

namespace Grpc
{
    public class SampleAiv : MonoBehaviour
    {
        [SerializeField] private string host = "127.0.0.1";
        [SerializeField] private int port = 8032;

        string camId = "";

        void Start()
        {
            var st = Native.Init($"{host}:{port}");
            Debug.Log("Init: " + st);

            Native.SetCallbacks(
                onResult: r =>
                {
                    Debug.Log($"Result: id={r.ImageId} det={r.Detections.Length}");
                    foreach (var d in r.Detections)
                    {
                        Debug.Log($"  box=({d.Box.x},{d.Box.y},{d.Box.w},{d.Box.h}) cls={d.ClassId} score={d.Score}");
                    }
                },
                onError: (code, msg) =>
                {
                    Debug.LogError($"Error {code}: {msg}");
                },
                onFrameSent: (id, idx, ts) =>
                {
                    Debug.Log($"FrameSent: {id} idx={idx} ts={ts}");
                }
            );

            string json;
            st = Native.EnumerateCameras(out json);
            Debug.Log("Enumerate: " + st + " json=" + json);

            var cfg = new CaptureConfig { width = 640, height = 480, fps = 30 };
            st = Native.OpenCameraByPosition(0, cfg, out camId);
            Debug.Log("OpenCam: " + st + " camId=" + camId);

            if (st == AivStatus.OK)
            {
                st = Native.StartStreaming(camId, "unity_stream");
                Debug.Log("StartStreaming: " + st);
            }
        }

        void OnDestroy()
        {
            if (Native.IsStreaming())
            {
                var st = Native.StopStreaming();
                Debug.Log("StopStreaming: " + st);
            }
            Native.Shutdown();
        }
    }
}