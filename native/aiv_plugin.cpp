#include "aiv_plugin.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <grpcpp/grpcpp.h>
#include <vision.grpc.pb.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <camera/NdkCameraManager.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AIV_PLUGIN", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AIV_PLUGIN", __VA_ARGS__)
#else
#define LOGI(...) do{}while(0)
#define LOGE(...) do{}while(0)
#endif

static AIV_OnResult    g_on_result     = nullptr;
static AIV_OnError     g_on_error      = nullptr;
static AIV_OnFrameSent g_on_frame_sent = nullptr;

static std::string      g_target;
static std::string      g_cam_id;
static std::string      g_image_prefix = "img";
static std::string      g_stream_id    = "default";
static AIV_CaptureConfig g_cfg{0,0,0};
static std::atomic<int>  g_running{0};
static std::thread       g_thread;
static std::mutex        g_mu;
static float             g_score_thresh = 0.0f;

// gRPC objects
static std::shared_ptr<grpc::Channel> g_channel;
static std::mutex g_rpc_mu;

static std::unique_ptr<vision::Vision::Stub> g_stub;

#if defined(__ANDROID__)
static ACameraManager* g_mgr = nullptr;

static int read_floats(const ACameraMetadata* m, uint32_t tag, float* out, int n) {
  if (!m || !out || n <= 0) return 0;
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK) return 0;
  int c = e.count < (size_t)n ? (int)e.count : n;
  for (int i = 0; i < c; ++i) out[i] = e.data.f[i];
  return c;
}

static int read_int32(const ACameraMetadata* m, uint32_t tag, int32_t* out, int n) {
  if (!m || !out || n <= 0) return 0;
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK) return 0;
  int c = e.count < (size_t)n ? (int)e.count : n;
  for (int i = 0; i < c; ++i) out[i] = e.data.i32[i];
  return c;
}

static bool load_params_for_id_ndk(const char* camId, AIV_Intrinsics* K, AIV_Extrinsics* X) {
  if (!g_mgr) return false;
  ACameraMetadata* chars = nullptr;
  if (ACameraManager_getCameraCharacteristics(g_mgr, camId, &chars) != ACAMERA_OK || !chars) return false;
  float intr[5] = {};
  float t[3] = {};
  float q[4] = {};
  int s1 = read_floats(chars, ACAMERA_LENS_INTRINSIC_CALIBRATION, intr, 5);
  int s2 = read_floats(chars, ACAMERA_LENS_POSE_TRANSLATION, t, 3);
  int s3 = read_floats(chars, ACAMERA_LENS_POSE_ROTATION, q, 4);
  ACameraMetadata_free(chars);
  if (s1 <= 0 || s2 <= 0 || s3 <= 0) return false;
  K->fx = intr[0]; K->fy = intr[1]; K->cx = intr[2]; K->cy = intr[3]; K->skew = intr[4];
  X->tx = t[0]; X->ty = t[1]; X->tz = t[2]; X->qx = q[0]; X->qy = q[1]; X->qz = q[2]; X->qw = q[3];
  return true;
}

static bool get_vendor_pos_for_id(const char* camId, int32_t* pos_out) {
  if (!g_mgr || !pos_out) return false;
  ACameraMetadata* chars = nullptr;
  if (ACameraManager_getCameraCharacteristics(g_mgr, camId, &chars) != ACAMERA_OK || !chars) return false;
  int32_t v = -1;
  int r = read_int32(chars, AIV_VENDOR_TAG_POSITION, &v, 1);
  ACameraMetadata_free(chars);
  if (r <= 0) return false;
  *pos_out = v;
  return true;
}
#endif

AIV_Status AIV_Init(const char* grpc_target) {
  if (!grpc_target) return AIV_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_running.load()) return AIV_ERR_ALREADY_RUNNING;
  g_target = grpc_target;

  try {
    g_channel = grpc::CreateChannel(g_target, grpc::InsecureChannelCredentials());
    g_stub    = vision::Vision::NewStub(g_channel);
  } catch (...) {
    if (g_on_error) g_on_error(AIV_ERR_GRPC, "Failed to initialize gRPC channel/stub.");
    return AIV_ERR_GRPC;
  }

#if defined(__ANDROID__)
  if (!g_mgr) g_mgr = ACameraManager_create();
  if (!g_mgr) return AIV_ERR_INTERNAL;
#endif
  return AIV_OK;
}

void AIV_Shutdown(void) {
  AIV_StopStreaming();
  std::lock_guard<std::mutex> lk(g_mu);
  g_target.clear();
  g_stub.reset();
  g_channel.reset();
#if defined(__ANDROID__)
  if (g_mgr) {
    ACameraManager_delete(g_mgr);
    g_mgr = nullptr;
  }
#endif
}

void AIV_SetCallbacks(AIV_OnResult on_result,
                      AIV_OnError on_error,
                      AIV_OnFrameSent on_frame_sent) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_on_result = on_result;
  g_on_error = on_error;
  g_on_frame_sent = on_frame_sent;
}

AIV_Status AIV_EnumerateCameras(char* out_json, int32_t capacity) {
  if (!out_json || capacity <= 2) return AIV_ERR_INVALID_ARG;
#if defined(__ANDROID__)
  if (!g_mgr) return AIV_ERR_NOT_INITIALIZED;
  ACameraIdList* list = nullptr;
  if (ACameraManager_getCameraIdList(g_mgr, &list) != ACAMERA_OK || !list) return AIV_ERR_INTERNAL;
  std::string j = "[";
  for (int i = 0; i < list->numCameras; ++i) {
    const char* id = list->cameraIds[i];
    int32_t pos = -1;
    get_vendor_pos_for_id(id, &pos);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s{\"id\":\"%s\",\"position\":%d}", (i? ",": ""), id, pos);
    j += buf;
  }
  j += "]";
  ACameraManager_deleteCameraIdList(list);
  if ((int)j.size()+1 > capacity) return AIV_ERR_INTERNAL;
  std::snprintf(out_json, capacity, "%s", j.c_str());
  return AIV_OK;
#else
  std::snprintf(out_json, capacity, "[]");
  return AIV_OK;
#endif
}

AIV_Status AIV_OpenCameraByPosition(int32_t position_value,
                                    const AIV_CaptureConfig* config,
                                    char* out_cam_id,
                                    int32_t cam_id_capacity) {
  if (!config) return AIV_ERR_INVALID_ARG;
#if defined(__ANDROID__)
  if (!g_mgr) return AIV_ERR_NOT_INITIALIZED;
  ACameraIdList* list = nullptr;
  if (ACameraManager_getCameraIdList(g_mgr, &list) != ACAMERA_OK || !list) return AIV_ERR_INTERNAL;
  std::string found;
  for (int i = 0; i < list->numCameras; ++i) {
    const char* id = list->cameraIds[i];
    int32_t pos = -1;
    if (get_vendor_pos_for_id(id, &pos) && pos == position_value) { found = id; break; }
  }
  ACameraManager_deleteCameraIdList(list);
  if (found.empty()) return AIV_ERR_CAMERA_OPEN;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cam_id = found;
    g_cfg = *config;
  }
  if (out_cam_id && cam_id_capacity > 0) std::snprintf(out_cam_id, cam_id_capacity, "%s", found.c_str());
  return AIV_OK;
#else
  return AIV_ERR_INTERNAL;
#endif
}

AIV_Status AIV_OpenCameraById(const char* cam_id,
                              const AIV_CaptureConfig* config) {
  if (!cam_id || !config) return AIV_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lk(g_mu);
  g_cam_id = cam_id;
  g_cfg = *config;
  return AIV_OK;
}

AIV_Status AIV_GetCameraParams(const char* cam_id,
                               AIV_Intrinsics* K,
                               AIV_Extrinsics* X) {
  if (!cam_id || !K || !X) return AIV_ERR_INVALID_ARG;
#if defined(__ANDROID__)
  if (!g_mgr) return AIV_ERR_NOT_INITIALIZED;
  if (!load_params_for_id_ndk(cam_id, K, X)) return AIV_ERR_CAMERA_PARAM;
  return AIV_OK;
#else
  return AIV_ERR_INTERNAL;
#endif
}

AIV_Status AIV_SetScoreThreshold(float score_threshold) {
  g_score_thresh = score_threshold;
  return AIV_OK;
}

AIV_Status AIV_SetImageIdPrefix(const char* prefix) {
  if (!prefix) return AIV_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lk(g_mu);
  g_image_prefix = prefix;
  return AIV_OK;
}

static bool fetch_detections_via_grpc(const std::string& /*stream_id*/,
                                      const std::string& image_id,
                                      int64_t /*frame_index*/,
                                      double /*timestamp_sec*/,
                                      float score_threshold,
                                      std::vector<AIV_Detection>& out) {
  if (!g_stub) return false;

  vision::DetectRequest req;
  vision::DetectResponse resp;

  req.set_image(nullptr, 0);
  req.set_width(static_cast<uint32_t>(g_cfg.width));
  req.set_height(static_cast<uint32_t>(g_cfg.height));
  req.set_score_threshold(score_threshold);
  req.set_image_id(image_id);

  grpc::ClientContext ctx;
  grpc::Status st;
  {
    std::lock_guard<std::mutex> lk(g_rpc_mu);
    st = g_stub->Detect(&ctx, req, &resp);
  }
  if (!st.ok()) {
    if (g_on_error) g_on_error(AIV_ERR_GRPC, st.error_message().c_str());
    return false;
  }

  out.clear();
  const int n = resp.detections_size();
  out.reserve(n);
  for (int i = 0; i < n; ++i) {
    const auto& d = resp.detections(i);
    if (d.score() < score_threshold) continue;

    AIV_Detection ad{};
    ad.box.x   = d.box().x();
    ad.box.y   = d.box().y();
    ad.box.w   = d.box().w();
    ad.box.h   = d.box().h();
    ad.class_id = d.class_id();
    ad.score    = d.score();
    out.push_back(ad);
  }
  return true;
}

static void streaming_loop(std::string cam_id, std::string stream_id, AIV_CaptureConfig cfg) {
  int64_t idx = 0;
  auto t0 = std::chrono::steady_clock::now();
  const double dt = (cfg.fps > 0) ? 1.0 / (double)cfg.fps : 1.0 / 30.0;

  // thread-local buffer so AIV_OnResult can read synchronously
  std::vector<AIV_Detection> detbuf;

  while (g_running.load()) {
    auto now = std::chrono::steady_clock::now();
    double ts = std::chrono::duration<double>(now - t0).count();

    char idbuf[128];
    std::snprintf(idbuf, sizeof(idbuf), "%s_%lld", g_image_prefix.c_str(), (long long)idx);

    LOGI("Frame %lld ts=%.3f id=%s", (long long)idx, ts, idbuf);

    if (g_on_frame_sent) g_on_frame_sent(idbuf, idx, ts);

    bool ok = fetch_detections_via_grpc(stream_id, idbuf, idx, ts, g_score_thresh, detbuf);
    if (!ok && g_on_error) {
      g_on_error(AIV_ERR_GRPC, "Detection RPC failed.");
    }

    AIV_Result r{};
    r.image_id = idbuf;
    r.frame_index = idx;
    r.timestamp_sec = ts;
    r.detections = detbuf.empty() ? nullptr : detbuf.data();
    r.detection_count = (int32_t)detbuf.size();
    if (g_on_result) g_on_result(&r);

    idx++;
    std::this_thread::sleep_for(std::chrono::duration<double>(dt));
  }
}

AIV_Status AIV_StartStreaming(const char* cam_id,
                              const char* stream_id) {
  if (!cam_id || !stream_id) return AIV_ERR_INVALID_ARG;
  if (g_running.exchange(1)) return AIV_ERR_ALREADY_RUNNING;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cam_id = cam_id;
    g_stream_id = stream_id;
  }
  try {
    g_thread = std::thread([=]{
      streaming_loop(std::string(cam_id), std::string(stream_id), g_cfg);
    });
  } catch (...) {
    g_running.store(0);
    return AIV_ERR_INTERNAL;
  }
  return AIV_OK;
}

AIV_Status AIV_StopStreaming(void) {
  if (!g_running.exchange(0)) return AIV_ERR_NOT_RUNNING;
  if (g_thread.joinable()) g_thread.join();
  return AIV_OK;
}

int32_t AIV_IsStreaming(void) {
  return g_running.load();
}
