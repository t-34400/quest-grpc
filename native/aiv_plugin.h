#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIV_VENDOR_TAG_CAMERA_SOURCE 0x80004d00
#define AIV_VENDOR_TAG_POSITION      0x80004d01
#define AIV_POSITION_LEFT            0
#define AIV_POSITION_RIGHT           1

typedef struct {
  float fx, fy, cx, cy, skew;
} AIV_Intrinsics;

typedef struct {
  float tx, ty, tz, qx, qy, qz, qw;
} AIV_Extrinsics;

typedef struct {
  int32_t width;
  int32_t height;
  int32_t fps;
} AIV_CaptureConfig;

typedef struct {
  float x;
  float y;
  float w;
  float h;
} AIV_Box;

typedef struct {
  AIV_Box box;
  int32_t class_id;
  float score;
} AIV_Detection;

typedef struct {
  const char* image_id;
  int64_t frame_index;
  double timestamp_sec;
  const AIV_Detection* detections;
  int32_t detection_count;
} AIV_Result;

typedef enum {
  AIV_OK = 0,
  AIV_ERR_INVALID_ARG = -1,
  AIV_ERR_NOT_INITIALIZED = -2,
  AIV_ERR_ALREADY_RUNNING = -3,
  AIV_ERR_NOT_RUNNING = -4,
  AIV_ERR_CAMERA_OPEN = -5,
  AIV_ERR_CAMERA_PARAM = -6,
  AIV_ERR_GRPC = -7,
  AIV_ERR_INTERNAL = -9
} AIV_Status;

typedef void (*AIV_OnResult)(const AIV_Result* result);
typedef void (*AIV_OnError)(int32_t code, const char* message);
typedef void (*AIV_OnFrameSent)(const char* image_id, int64_t frame_index, double timestamp_sec);

AIV_Status AIV_Init(const char* grpc_target);
void       AIV_Shutdown(void);

void       AIV_SetCallbacks(AIV_OnResult on_result,
                            AIV_OnError on_error,
                            AIV_OnFrameSent on_frame_sent);

AIV_Status AIV_EnumerateCameras(char* out_json, int32_t capacity);

AIV_Status AIV_OpenCameraByPosition(int32_t position_value,
                                    const AIV_CaptureConfig* config,
                                    char* out_cam_id,
                                    int32_t cam_id_capacity);

AIV_Status AIV_OpenCameraById(const char* cam_id,
                              const AIV_CaptureConfig* config);

AIV_Status AIV_GetCameraParams(const char* cam_id,
                               AIV_Intrinsics* K,
                               AIV_Extrinsics* X);

AIV_Status AIV_SetScoreThreshold(float score_threshold);
AIV_Status AIV_SetImageIdPrefix(const char* prefix);

AIV_Status AIV_StartStreaming(const char* cam_id,
                              const char* stream_id);

AIV_Status AIV_StopStreaming(void);

int32_t    AIV_IsStreaming(void);

#ifdef __cplusplus
}
#endif
