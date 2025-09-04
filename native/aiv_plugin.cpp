#include "aiv_plugin.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <grpcpp/grpcpp.h>
#include <vision.grpc.pb.h>

#include <turbojpeg.h>
#include <libyuv.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImageReader.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AIV_PLUGIN", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AIV_PLUGIN", __VA_ARGS__)
#else
#define LOGI(...) do{}while(0)
#define LOGE(...) do{}while(0)
#endif

static AIV_OnResult     g_on_result     = nullptr;
static AIV_OnError      g_on_error      = nullptr;
static AIV_OnFrameSent  g_on_frame_sent = nullptr;

static std::string   g_target;
static std::string   g_image_prefix = "img";
static std::string   g_stream_base  = "default";
static std::atomic<int> g_running{0};
static std::atomic<int> g_connected{0};
static float         g_score_thresh = 0.0f;
static AIV_JpegConfig g_jpeg_cfg{0, 0, 70};

static inline void clamp_jpeg_cfg(AIV_JpegConfig& c) {
  if (c.jpeg_quality < 1)   c.jpeg_quality = 70;
  if (c.jpeg_quality > 100) c.jpeg_quality = 100;
}

static std::shared_ptr<grpc::Channel> g_channel;
static std::unique_ptr<vision::Vision::Stub> g_stub;
static std::unique_ptr<grpc::ClientContext> g_ctx;
static std::unique_ptr<grpc::ClientReaderWriter<vision::Frame, vision::Result>> g_stream;
static std::mutex g_stream_mu;

#if defined(__ANDROID__)
static ACameraManager* g_mgr = nullptr;
#endif

static bool I420ToJpeg(const uint8_t* i420, int w, int h, int quality, std::vector<uint8_t>& jpeg) {
  tjhandle hnd = tjInitCompress();
  if (!hnd) return false;
  const unsigned char* planes[3] = {
    (const unsigned char*)i420,
    (const unsigned char*)(i420 + w * h),
    (const unsigned char*)(i420 + w * h + ((w + 1) / 2) * ((h + 1) / 2))
  };
  const int strides[3] = { w, (w + 1) / 2, (w + 1) / 2 };
  unsigned char* out = nullptr;
  unsigned long out_size = 0;
  const int rc = tjCompressFromYUVPlanes(
    hnd, planes, w, strides, h, TJSAMP_420, &out, &out_size, quality, TJFLAG_FASTDCT
  );
  if (rc != 0) { tjDestroy(hnd); return false; }
  jpeg.assign(out, out + out_size);
  tjFree(out);
  tjDestroy(hnd);
  return true;
}

template <typename T>
class SpscQueue {
public:
  explicit SpscQueue(size_t capacity) : cap_(capacity), buf_(capacity) {
    head_.store(0); tail_.store(0);
  }
  bool push(T&& v) {
    size_t h = head_.load(std::memory_order_relaxed);
    size_t t = tail_.load(std::memory_order_acquire);
    size_t n = cap_;
    if (((h + 1) % n) == t) { // full -> drop oldest
      tail_.store((t + 1) % n, std::memory_order_release);
    }
    buf_[h] = std::move(v);
    head_.store((h + 1) % n, std::memory_order_release);
    return true;
  }
  bool pop(T& out) {
    size_t t = tail_.load(std::memory_order_relaxed);
    size_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    out = std::move(buf_[t]);
    tail_.store((t + 1) % cap_, std::memory_order_release);
    return true;
  }
  void clear() {
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
  }
private:
  size_t cap_;
  std::vector<T> buf_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

struct I420Frame {
  AIV_CamRole role;
  int w{0}, h{0};
  int64_t frame_index{0};
  uint64_t ts_ns{0};
  std::vector<uint8_t> data; // size = w*h + (w/2*h/2)*2
};

struct EncodedPacket {
  AIV_CamRole role;
  int w{0}, h{0};
  int64_t frame_index{0};
  uint64_t ts_ns{0};
  std::vector<uint8_t> jpeg;
  std::string camera_id;
  std::string stream_id;
};

struct CamContext {
  AIV_CamRole role{AIV_CAM_LEFT};
  std::string cam_id;
  AIV_CaptureConfig cfg{0,0,0};
  std::atomic<int64_t> idx{0};

  std::unique_ptr<SpscQueue<I420Frame>> raw_q;   // capture -> encode
  std::unique_ptr<SpscQueue<EncodedPacket>> enc_q; // encode -> send

  std::thread encode_th;
  std::atomic<int> encode_running{0};

#if defined(__ANDROID__)
  ACameraDevice* device{nullptr};
  AImageReader* reader{nullptr};
  ACaptureRequest* request{nullptr};
  ACaptureSessionOutputContainer* outputs{nullptr};
  ACaptureSessionOutput* output{nullptr};
  ACameraOutputTarget* target{nullptr};
  ACameraCaptureSession* session{nullptr};
#endif
};

static CamContext g_left;
static CamContext g_right;

static inline const char* role_suffix(AIV_CamRole r) { return (r==AIV_CAM_LEFT) ? "left" : "right"; }

static void encode_loop(CamContext* cc);
static void send_loop();
static void recv_loop();

#if defined(__ANDROID__)
static void close_camera(CamContext* cc);

static void on_image_available(void* ctx, AImageReader* reader) {
  CamContext* cc = reinterpret_cast<CamContext*>(ctx);
  if (!cc || !g_running.load()) return;

  AImage* img = nullptr;
  media_status_t mr = AImageReader_acquireNextImage(reader, &img);
  if (mr != AMEDIA_OK || !img) return;

  int32_t fmt=0, w=0, h=0;
  AImage_getFormat(img, &fmt);
  AImage_getWidth(img, &w);
  AImage_getHeight(img, &h);
  if (fmt != AIMAGE_FORMAT_YUV_420_888 || w<=0 || h<=0) {
    AImage_delete(img);
    return;
  }

  uint8_t* yptr=nullptr; int ylen=0, ys=0;
  uint8_t* uptr=nullptr; int ulen=0, us=0;
  uint8_t* vptr=nullptr; int vlen=0, vs=0;
  int uv_ps=0;
  AImage_getPlaneData(img, 0, &yptr, &ylen);
  AImage_getPlaneRowStride(img, 0, &ys);
  AImage_getPlaneData(img, 1, &uptr, &ulen);
  AImage_getPlaneRowStride(img, 1, &us);
  AImage_getPlanePixelStride(img, 1, &uv_ps);
  AImage_getPlaneData(img, 2, &vptr, &vlen);
  AImage_getPlaneRowStride(img, 2, &vs);

  I420Frame f;
  f.role = cc->role;
  f.w = w; f.h = h;
  int64_t idx = cc->idx.fetch_add(1, std::memory_order_relaxed);
  f.frame_index = idx;

  int64_t ts; AImage_getTimestamp(img, &ts);
  f.ts_ns = (uint64_t)(ts < 0 ? 0 : ts);

  const int y_size = w*h;
  const int uv_w = (w + 1) >> 1;
  const int uv_h = (h + 1) >> 1;
  const int uv_size = uv_w * uv_h;
  f.data.resize(y_size + uv_size + uv_size);

  int r = libyuv::Android420ToI420(
    yptr, ys,
    uptr, us,
    vptr, vs,
    uv_ps,
    f.data.data(), w,
    f.data.data() + y_size, uv_w,
    f.data.data() + y_size + uv_size, uv_w,
    w, h
  );

  AImage_delete(img);
  if (r != 0) return;

  if (cc->raw_q) cc->raw_q->push(std::move(f));
}

static void on_cam_disconnected(void* ctx, ACameraDevice* dev) {
    LOGE("Camera disconnected");
    if (g_on_error) {
        g_on_error(AIV_ERR_CAMERA_OPEN, "Camera disconnected.");
    }
}

static void on_cam_error(void* ctx, ACameraDevice* dev, int err) {
    LOGE("Camera device error err=%d", err);
    if (g_on_error) {
        g_on_error(AIV_ERR_CAMERA_OPEN, "Camera device error.");
    }
}

static bool open_camera(CamContext* cc) {
  if (!cc) { LOGE("open_camera: invalid context"); return false; }
  if (!g_mgr) { LOGE("open_camera: camera manager is null"); return false; }

  const int w   = (cc->cfg.width  > 0) ? cc->cfg.width  : 640;
  const int h   = (cc->cfg.height > 0) ? cc->cfg.height : 480;
  const int fps = (cc->cfg.fps    > 0) ? cc->cfg.fps    : 30;

  LOGI("open_camera: begin cam_id=%s, w=%d, h=%d, fps=%d",
       cc->cam_id.c_str(), w, h, fps);

  ACameraDevice_stateCallbacks dev_cbs{};
  dev_cbs.context        = cc;
  dev_cbs.onDisconnected = on_cam_disconnected;
  dev_cbs.onError        = on_cam_error;

  camera_status_t st = ACameraManager_openCamera(
      g_mgr, cc->cam_id.c_str(), &dev_cbs, &cc->device);
  LOGI("open_camera: ACameraManager_openCamera ret=%d device=%p", st, (void*)cc->device);
  if (st != ACAMERA_OK || !cc->device) {
      LOGE("open_camera: Camera open failed. ret=%d", st);
      if (g_on_error) g_on_error(AIV_ERR_CAMERA_OPEN, "Camera open failed.");
      return false;
  }

  media_status_t mr = AImageReader_new(
      w, h, AIMAGE_FORMAT_YUV_420_888, /*maxImages*/4, &cc->reader);
  LOGI("open_camera: AImageReader_new ret=%d reader=%p", mr, (void*)cc->reader);
  if (mr != AMEDIA_OK || !cc->reader) {
    LOGE("open_camera: AImageReader creation failed. ret=%d", mr);
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "AImageReader creation failed.");
    close_camera(cc);
    return false;
  }

  AImageReader_ImageListener listener{};
  listener.context = cc;
  listener.onImageAvailable = on_image_available;
  media_status_t ml = AImageReader_setImageListener(cc->reader, &listener);
  LOGI("open_camera: AImageReader_setImageListener ret=%d", ml);

  ANativeWindow* wnd = nullptr;
  media_status_t mw = AImageReader_getWindow(cc->reader, &wnd);
  LOGI("open_camera: AImageReader_getWindow ret=%d wnd=%p", mw, (void*)wnd);
  if (!wnd) {
    LOGE("open_camera: AImageReader window unavailable.");
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "AImageReader window unavailable.");
    close_camera(cc);
    return false;
  }

  camera_status_t rc1 = ACaptureSessionOutputContainer_create(&cc->outputs);
  camera_status_t rc2 = ACaptureSessionOutput_create(wnd, &cc->output);
  camera_status_t rc3 = ACameraOutputTarget_create(wnd, &cc->target);
  LOGI("open_camera: outputs-create ret={%d,%d,%d} outputs=%p output=%p target=%p",
       rc1, rc2, rc3, (void*)cc->outputs, (void*)cc->output, (void*)cc->target);
  if (rc1 != ACAMERA_OK || rc2 != ACAMERA_OK || rc3 != ACAMERA_OK) {
    LOGE("open_camera: Capture output setup failed. ret={%d,%d,%d}", rc1, rc2, rc3);
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "Capture output setup failed.");
    close_camera(cc);
    return false;
  }

  camera_status_t ra = ACaptureSessionOutputContainer_add(cc->outputs, cc->output);
  LOGI("open_camera: ACaptureSessionOutputContainer_add ret=%d", ra);
  if (ra != ACAMERA_OK) {
    LOGE("open_camera: Capture output add failed. ret=%d", ra);
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "Capture output add failed.");
    close_camera(cc);
    return false;
  }

  camera_status_t rrq = ACameraDevice_createCaptureRequest(
      cc->device, TEMPLATE_RECORD, &cc->request);
  LOGI("open_camera: ACameraDevice_createCaptureRequest ret=%d request=%p", rrq, (void*)cc->request);
  if (rrq != ACAMERA_OK || !cc->request) {
    LOGE("open_camera: Create capture request failed. ret=%d", rrq);
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "Create capture request failed.");
    close_camera(cc);
    return false;
  }

  camera_status_t rtt = ACaptureRequest_addTarget(cc->request, cc->target);
  LOGI("open_camera: ACaptureRequest_addTarget ret=%d", rtt);
  if (rtt != ACAMERA_OK) {
    LOGE("open_camera: Add target to request failed. ret=%d", rtt);
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "Add target to request failed.");
    close_camera(cc);
    return false;
  }

  ACameraCaptureSession_stateCallbacks cbs{};
  cbs.context = cc;

  camera_status_t cs = ACameraDevice_createCaptureSession(
      cc->device, cc->outputs, &cbs, &cc->session);
  LOGI("open_camera: ACameraDevice_createCaptureSession ret=%d session=%p", cs, (void*)cc->session);
  if (cs != ACAMERA_OK || !cc->session) {
    LOGE("open_camera: Create capture session failed. ret=%d", cs);
    if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "Create capture session failed.");
    close_camera(cc);
    return false;
  }

  ACameraMetadata* chars = nullptr;
  camera_status_t rcmeta = ACameraManager_getCameraCharacteristics(
      g_mgr, cc->cam_id.c_str(), &chars);
  LOGI("open_camera: getCameraCharacteristics ret=%d meta=%p", rcmeta, (void*)chars);
  if (chars) ACameraMetadata_free(chars);

  camera_status_t rrep = ACameraCaptureSession_setRepeatingRequest(
      cc->session, nullptr, 1, &cc->request, nullptr);
  LOGI("open_camera: setRepeatingRequest ret=%d", rrep);

  const bool ok = (rrep == ACAMERA_OK);
  if (!ok) {
    LOGE("open_camera: Start repeating request failed. ret=%d", rrep);
    close_camera(cc);
    return false;
  }

  LOGI("open_camera: success");
  return true;
}


static void close_camera(CamContext* cc) {
  if (!cc) return;
  if (cc->session) {
    ACameraCaptureSession_stopRepeating(cc->session);
    ACameraCaptureSession_close(cc->session);
    cc->session = nullptr;
  }
  if (cc->request) {
    ACaptureRequest_free(cc->request);
    cc->request = nullptr;
  }
  if (cc->target) {
    ACameraOutputTarget_free(cc->target);
    cc->target = nullptr;
  }
  if (cc->output) {
    ACaptureSessionOutput_free(cc->output);
    cc->output = nullptr;
  }
  if (cc->outputs) {
    ACaptureSessionOutputContainer_free(cc->outputs);
    cc->outputs = nullptr;
  }
  if (cc->reader) {
    AImageReader_delete(cc->reader);
    cc->reader = nullptr;
  }
  if (cc->device) {
    ACameraDevice_close(cc->device);
    cc->device = nullptr;
  }
}
#endif // __ANDROID__

static void encode_loop(CamContext* cc) {
  if (!cc) return;
  cc->encode_running.store(1);
  while (g_running.load()) {
    I420Frame in;
    if (!cc->raw_q || !cc->raw_q->pop(in)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    EncodedPacket pkt;
    pkt.role = in.role; pkt.w = in.w; pkt.h = in.h;
    pkt.frame_index = in.frame_index; pkt.ts_ns = in.ts_ns;
    pkt.camera_id = cc->cam_id;
    pkt.stream_id = g_stream_base + "_" + role_suffix(cc->role);

    std::vector<uint8_t> jpeg;
    AIV_JpegConfig jc; { jc = g_jpeg_cfg; }
    if (!I420ToJpeg(in.data.data(), in.w, in.h, jc.jpeg_quality, jpeg)) {
      if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "JPEG encode failed.");
      continue;
    }
    pkt.jpeg = std::move(jpeg);
    if (cc->enc_q) cc->enc_q->push(std::move(pkt));
  }
  cc->encode_running.store(0);
}

static std::thread g_send_thread;
static void send_loop() {
  int turn = 0;
  while (g_running.load()) {
    bool sent = false;

    auto try_send_from = [&](CamContext* cc) {
      if (!cc || !cc->enc_q) return false;
      EncodedPacket pkt;
      if (!cc->enc_q->pop(pkt)) return false;

      vision::Frame f;
      f.set_stream_id(pkt.stream_id);
      f.set_camera_id(pkt.camera_id);
      f.set_frame_index((uint64_t)pkt.frame_index);
      f.set_timestamp_ns(pkt.ts_ns);
      f.set_width((uint32_t)pkt.w);
      f.set_height((uint32_t)pkt.h);
      f.set_format(vision::IMAGE_FORMAT_JPEG);
      f.set_data(std::string(reinterpret_cast<const char*>(pkt.jpeg.data()), pkt.jpeg.size()));

      grpc::ClientReaderWriter<vision::Frame, vision::Result>* stream = nullptr;
      {
        std::lock_guard<std::mutex> lk(g_stream_mu);
        if (!g_stream) return false;
        stream = g_stream.get();
      }

      if (!stream->Write(f)) {
        if (g_on_error) g_on_error(AIV_ERR_GRPC, "Write failed on streaming RPC.");
        g_running.store(0);
        return true;
      }

      char idbuf[128];
      std::snprintf(idbuf, sizeof(idbuf), "%s_%lld", g_image_prefix.c_str(), (long long)pkt.frame_index);
      if (g_on_frame_sent) g_on_frame_sent(idbuf, pkt.frame_index, (double)pkt.ts_ns * 1e-9);
      return true;
    };

    if (turn % 2 == 0) {
      sent = try_send_from(&g_left) || try_send_from(&g_right);
    } else {
      sent = try_send_from(&g_right) || try_send_from(&g_left);
    }
    turn++;

    if (!sent) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::lock_guard<std::mutex> lk(g_stream_mu);
  if (g_stream) g_stream->WritesDone();
}

static std::thread g_recv_thread;
static void recv_loop() {
  while (g_running.load()) {
    vision::Result res;

    grpc::ClientReaderWriter<vision::Frame, vision::Result>* stream = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_stream_mu);
      if (!g_stream) break;
      stream = g_stream.get();
    }

    if (!stream->Read(&res)) break;

    static thread_local std::vector<AIV_Detection> detbuf;
    detbuf.clear(); detbuf.reserve(res.detections_size());
    for (int i = 0; i < res.detections_size(); ++i) {
      const auto& d = res.detections(i);
      if (d.score() < g_score_thresh) continue;
      AIV_Detection ad{};
      ad.box.x = d.box().x();
      ad.box.y = d.box().y();
      ad.box.w = d.box().w();
      ad.box.h = d.box().h();
      ad.class_id = d.class_id();
      ad.score = d.score();
      detbuf.push_back(ad);
    }

    char idbuf[128];
    std::snprintf(idbuf, sizeof(idbuf), "%s_%llu", g_image_prefix.c_str(), (unsigned long long)res.frame_index());
    AIV_Result r{};
    r.image_id = idbuf;
    r.frame_index = (int64_t)res.frame_index();
    r.timestamp_sec = (double)res.timestamp_ns() * 1e-9;
    r.detections = detbuf.empty() ? nullptr : detbuf.data();
    r.detection_count = (int32_t)detbuf.size();
    if (g_on_result) g_on_result(&r);
  }
}

AIV_Status AIV_Init(const char* grpc_target) {
  if (!grpc_target) return AIV_ERR_INVALID_ARG;
  if (g_running.load()) return AIV_ERR_ALREADY_RUNNING;

  g_target = grpc_target;
  try {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, 32 * 1024 * 1024);
    args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, 32 * 1024 * 1024);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 15000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetCompressionAlgorithm(GRPC_COMPRESS_NONE);
    g_channel = grpc::CreateCustomChannel(g_target, grpc::InsecureChannelCredentials(), args);
    g_stub = vision::Vision::NewStub(g_channel);
  } catch (...) {
    if (g_on_error) g_on_error(AIV_ERR_GRPC, "Failed to initialize gRPC channel/stub.");
    return AIV_ERR_GRPC;
  }

#if defined(__ANDROID__)
  if (!g_mgr) g_mgr = ACameraManager_create();
  if (!g_mgr) return AIV_ERR_INTERNAL;
#endif

  g_left.role  = AIV_CAM_LEFT;
  g_right.role = AIV_CAM_RIGHT;
  return AIV_OK;
}

void AIV_Shutdown(void) {
  AIV_StopStreaming();
  g_target.clear();
  g_stub.reset();
  g_channel.reset();
#if defined(__ANDROID__)
  if (g_mgr) { ACameraManager_delete(g_mgr); g_mgr = nullptr; }
#endif
}

void AIV_SetCallbacks(AIV_OnResult on_result, AIV_OnError on_error, AIV_OnFrameSent on_frame_sent) {
  g_on_result = on_result;
  g_on_error  = on_error;
  g_on_frame_sent = on_frame_sent;
}

AIV_Status AIV_SetJpegConfig(const AIV_JpegConfig* cfg) {
  if (!cfg) return AIV_ERR_INVALID_ARG;
  g_jpeg_cfg = *cfg;
  clamp_jpeg_cfg(g_jpeg_cfg);
  return AIV_OK;
}
void AIV_GetJpegConfig(AIV_JpegConfig* out) { if (out) *out = g_jpeg_cfg; }

AIV_Status AIV_SetScoreThreshold(float score_threshold) { g_score_thresh = score_threshold; return AIV_OK; }
AIV_Status AIV_SetImageIdPrefix(const char* prefix) { if (!prefix) return AIV_ERR_INVALID_ARG; g_image_prefix = prefix; return AIV_OK; }
AIV_Status AIV_SetStereoStreamBaseId(const char* base_id) { if (!base_id) return AIV_ERR_INVALID_ARG; g_stream_base = base_id; return AIV_OK; }

#if defined(__ANDROID__)
static int read_int32(const ACameraMetadata* m, uint32_t tag, int32_t* out, int n) {
  if (!m || !out || n<=0) return 0;
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK) return 0;
  int c = e.count < (size_t)n ? (int)e.count : n;
  for (int i=0;i<c;++i) out[i] = e.data.i32[i];
  return c;
}
static bool get_vendor_pos_for_id(const char* camId, int32_t* pos_out) {
  if (!g_mgr || !pos_out) return false;
  ACameraMetadata* chars = nullptr;
  if (ACameraManager_getCameraCharacteristics(g_mgr, camId, &chars) != ACAMERA_OK || !chars) return false;
  int32_t v=-1;
  int r = read_int32(chars, AIV_VENDOR_TAG_POSITION, &v, 1);
  ACameraMetadata_free(chars);
  if (r<=0) return false;
  *pos_out = v; return true;
}
#endif

AIV_Status AIV_EnumerateCameras(char* out_json, int32_t capacity) {
  if (!out_json || capacity <= 2) return AIV_ERR_INVALID_ARG;
#if defined(__ANDROID__)
  if (!g_mgr) return AIV_ERR_NOT_INITIALIZED;
  ACameraIdList* list = nullptr;
  if (ACameraManager_getCameraIdList(g_mgr, &list) != ACAMERA_OK || !list) return AIV_ERR_INTERNAL;
  std::string j = "[";
  for (int i=0;i<list->numCameras;++i) {
    const char* id = list->cameraIds[i];
    int32_t pos = -1; get_vendor_pos_for_id(id, &pos);
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

AIV_Status AIV_SetCameraForRole(int role, const char* cam_id, const AIV_CaptureConfig* cfg) {
  if (!cam_id || !cfg) return AIV_ERR_INVALID_ARG;

  LOGI("SetCameraForRole(native): role=%d (LEFT=%d RIGHT=%d) cam_id=%s",
       role, AIV_CAM_LEFT, AIV_CAM_RIGHT, cam_id);

  CamContext* cc = (role == AIV_CAM_RIGHT) ? &g_right : &g_left;
  cc->role = (role == AIV_CAM_RIGHT) ? AIV_CAM_RIGHT : AIV_CAM_LEFT;
  cc->cam_id = cam_id;
  cc->cfg = *cfg;

  LOGI("SetCameraForRole(native): assigned_to=%s  g_left=%s  g_right=%s",
       (cc == &g_right) ? "RIGHT" : "LEFT",
       g_left.cam_id.c_str(), g_right.cam_id.c_str());
  return AIV_OK;
}

AIV_Status AIV_StartStreamingStereo(void) {
  if (g_running.exchange(1)) return AIV_ERR_ALREADY_RUNNING;

  if ((!g_left.cam_id.size()) && (!g_right.cam_id.size())) {
    g_running.store(0);
    return AIV_ERR_INVALID_ARG;
  }

  g_left.raw_q  = std::make_unique<SpscQueue<I420Frame>>(4);
  g_left.enc_q  = std::make_unique<SpscQueue<EncodedPacket>>(3);
  g_right.raw_q = std::make_unique<SpscQueue<I420Frame>>(4);
  g_right.enc_q = std::make_unique<SpscQueue<EncodedPacket>>(3);

  try {
    g_ctx = std::make_unique<grpc::ClientContext>();
    {
      std::lock_guard<std::mutex> lk(g_stream_mu);
      g_stream = g_stub->StreamDetect(g_ctx.get());
      if (!g_stream) {
        if (g_on_error) g_on_error(AIV_ERR_GRPC, "Failed to open streaming RPC.");
        g_ctx.reset();
        g_running.store(0);
        return AIV_ERR_GRPC;
      }
    }
    g_connected.store(1);
  } catch (...) {
    g_running.store(0);
    return AIV_ERR_INTERNAL;
  }

#if defined(__ANDROID__)
  if (g_left.cam_id.size()) {
    g_left.idx.store(0);
    LOGI("StartStreamingStereo: opening LEFT id=%s", g_left.cam_id.c_str());
    if (!open_camera(&g_left)) {
      LOGE("StartStreamingStereo: failed to open LEFT id=%s", g_left.cam_id.c_str());
      g_running.store(0);
      return AIV_ERR_CAMERA_OPEN;
    }
    LOGI("StartStreamingStereo: opened LEFT id=%s", g_left.cam_id.c_str());
  }

  if (g_right.cam_id.size()) {
    g_right.idx.store(0);
    LOGI("StartStreamingStereo: opening RIGHT id=%s", g_right.cam_id.c_str());
    if (!open_camera(&g_right)) {
      LOGE("StartStreamingStereo: failed to open RIGHT id=%s", g_right.cam_id.c_str());
      if (g_left.cam_id.size()) close_camera(&g_left);
      g_running.store(0);
      return AIV_ERR_CAMERA_OPEN;
    }
    LOGI("StartStreamingStereo: opened RIGHT id=%s", g_right.cam_id.c_str());
  }
#else
  if (g_on_error) g_on_error(AIV_ERR_INTERNAL, "Android-only capture path is not available on this platform.");
  g_running.store(0);
  return AIV_ERR_INTERNAL;
#endif

  g_left.encode_th  = std::thread(encode_loop, &g_left);
  g_right.encode_th = std::thread(encode_loop, &g_right);
  g_recv_thread     = std::thread(recv_loop);
  g_send_thread     = std::thread(send_loop);

  return AIV_OK;
}

AIV_Status AIV_StopStreaming(void) {
  if (!g_running.exchange(0)) return AIV_ERR_NOT_RUNNING;

#if defined(__ANDROID__)
  close_camera(&g_left);
  close_camera(&g_right);
#endif

  if (g_send_thread.joinable()) g_send_thread.join();
  if (g_recv_thread.joinable()) g_recv_thread.join();

  if (g_left.encode_th.joinable())  g_left.encode_th.join();
  if (g_right.encode_th.joinable()) g_right.encode_th.join();

  grpc::Status status;
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    if (g_stream) {
      status = g_stream->Finish();
      g_stream.reset();
    }
    g_ctx.reset();
  }
  g_connected.store(0);

  g_left.raw_q.reset();  g_left.enc_q.reset();
  g_right.raw_q.reset(); g_right.enc_q.reset();

  if (!status.ok() && g_on_error) g_on_error(AIV_ERR_GRPC, status.error_message().c_str());
  return AIV_OK;
}

int32_t AIV_IsStreaming(void) { return g_running.load() && g_connected.load(); }

AIV_Status AIV_GetCameraIdByPosition(int32_t position_value, char* out_cam_id, int32_t cap) {
  if (!out_cam_id || cap <= 0) return AIV_ERR_INVALID_ARG;

#if defined(__ANDROID__)
  if (!g_mgr) return AIV_ERR_NOT_INITIALIZED;

  ACameraIdList* list = nullptr;
  camera_status_t st = ACameraManager_getCameraIdList(g_mgr, &list);
  if (st != ACAMERA_OK || !list) return AIV_ERR_INTERNAL;

  const char* found = nullptr;
  for (int i = 0; i < list->numCameras; ++i) {
    const char* id = list->cameraIds[i];
    int32_t pos = -1;
    if (get_vendor_pos_for_id(id, &pos) && pos == position_value) { found = id; break; }
  }
  ACameraManager_deleteCameraIdList(list);

  if (!found) return AIV_ERR_CAMERA_OPEN;

  int n = std::snprintf(out_cam_id, cap, "%s", found);
  if (n < 0 || n >= cap) return AIV_ERR_INVALID_ARG; // output buffer too small
  return AIV_OK;
#else
  return AIV_ERR_INTERNAL;
#endif
}

AIV_Status AIV_GetCameraParams(const char* cam_id, AIV_Intrinsics* K, AIV_Extrinsics* X) {
  if (!cam_id || !K || !X) return AIV_ERR_INVALID_ARG;
#if defined(__ANDROID__)
  if (!g_mgr) return AIV_ERR_NOT_INITIALIZED;
  ACameraMetadata* chars = nullptr;
  if (ACameraManager_getCameraCharacteristics(g_mgr, cam_id, &chars) != ACAMERA_OK || !chars) return AIV_ERR_CAMERA_PARAM;
  float intr[5] = {}; float t[3] = {}; float q[4] = {};
  auto read_floats = [](const ACameraMetadata* m, uint32_t tag, float* out, int n)->int{
    if (!m || !out || n<=0) return 0;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK) return 0;
    int c = e.count < (size_t)n ? (int)e.count : n;
    for (int i=0;i<c;++i) out[i] = e.data.f[i];
    return c;
  };
  int s1 = read_floats(chars, ACAMERA_LENS_INTRINSIC_CALIBRATION, intr, 5);
  int s2 = read_floats(chars, ACAMERA_LENS_POSE_TRANSLATION, t, 3);
  int s3 = read_floats(chars, ACAMERA_LENS_POSE_ROTATION, q, 4);
  ACameraMetadata_free(chars);
  if (s1<=0 || s2<=0 || s3<=0) return AIV_ERR_CAMERA_PARAM;
  K->fx = intr[0]; K->fy = intr[1]; K->cx = intr[2]; K->cy = intr[3]; K->skew = intr[4];
  X->tx = t[0]; X->ty = t[1]; X->tz = t[2];
  X->qx = q[0]; X->qy = q[1]; X->qz = q[2]; X->qw = q[3];
  return AIV_OK;
#else
  return AIV_ERR_INTERNAL;
#endif
}
