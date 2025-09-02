#include <grpcpp/grpcpp.h>
#include <grpc/support/time.h>
#include <fstream>
#include <iostream>
#include <string>
#include "vision.pb.h"
#include "vision.grpc.pb.h"

static void usage(const char* a0) {
  std::cerr << "Usage: " << a0
            << " <host:port> <image_path> <width> <height> [--score=0.5] [--id=xxx] [--deadline_ms=5000]\n";
  std::exit(2);
}

static std::string read_file(const std::string& p) {
  std::ifstream ifs(p, std::ios::binary);
  if (!ifs) { std::cerr << "Error: failed to open file: " << p << "\n"; std::exit(1); }
  return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  if (argc < 5) usage(argv[0]);
  const std::string target = argv[1];
  const std::string img_path = argv[2];
  const uint32_t width = static_cast<uint32_t>(std::stoul(argv[3]));
  const uint32_t height = static_cast<uint32_t>(std::stoul(argv[4]));
  float score = 0.5f;
  std::string image_id;
  int deadline_ms = 5000;
  for (int i = 5; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--score=", 0) == 0) score = std::stof(a.substr(8));
    else if (a.rfind("--id=", 0) == 0) image_id = a.substr(5);
    else if (a.rfind("--deadline_ms=", 0) == 0) deadline_ms = std::stoi(a.substr(14));
    else usage(argv[0]);
  }

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  const gpr_timespec dl = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                       gpr_time_from_millis(deadline_ms, GPR_TIMESPAN));
  if (!channel->WaitForConnected(dl)) {
    std::cerr << "Error: connection timeout (" << deadline_ms << " ms)\n";
    return 1;
  }

  std::unique_ptr<vision::Vision::Stub> stub = vision::Vision::NewStub(channel);

  vision::DetectRequest req;
  req.set_image(read_file(img_path));
  req.set_width(width);
  req.set_height(height);
  req.set_score_threshold(score);
  if (!image_id.empty()) req.set_image_id(image_id);

  vision::DetectResponse resp;
  grpc::ClientContext ctx;
  const grpc::Status s = stub->Detect(&ctx, req, &resp);
  if (!s.ok()) {
    std::cerr << "RPC failed: code=" << s.error_code() << " msg=" << s.error_message() << "\n";
    return 1;
  }

  for (int i = 0; i < resp.detections_size(); ++i) {
    const auto& d = resp.detections(i);
    const auto& b = d.box();
    std::cout << i << "\t" << d.class_id() << "\t" << d.score()
              << "\t" << b.x() << "\t" << b.y() << "\t" << b.w() << "\t" << b.h() << "\n";
  }
  return 0;
}
