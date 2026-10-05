// Executable spec for "send feedback with logs" whether or not a tunnel runs
// (support inbox 2090): which device in the service carries the upload
// (Common/LogUpload.h), when the service builds no standalone device for it,
// how long that one may wait for its upload, what the app does with the
// service's answer, and the upload_logs request and reply on the control pipe
// (Common/Protocol.h), the feedback id check among them. Run against the SAME
// headers the service and the app compile; it needs nlohmann/json, like them.
//
//   c++ -std=c++20 -I ../src/Common -I <dir with nlohmann/json.hpp> log-upload-tests.cpp -o /tmp/log-upload-tests && /tmp/log-upload-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <iostream>
#include <string>

#include "LogUpload.h"
#include "Protocol.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

// synthetic values only, shaped like the real ones
proto::UploadLogs SampleRequest() {
  proto::UploadLogs r;
  r.feedback_id = "f00dfeed-0000-4000-8000-000000000001";
  r.by_jwt = "client.jwt.value";
  r.network_space_json = R"({"key":{"host_name":"network.example","env_name":"test"}})";
  r.instance_id = "0000feed-0000-4000-8000-0000000000aa";
  r.device_description = "DESKTOP-1";
  r.device_spec = "windows amd64";
  r.app_version = "2026.10.5-1";
  return r;
}

// (a) the device that runs carries it, the session's first, and a standalone
// device only when neither runs
void TestCarrier() {
  Check(logupload::CarrierFor(true, false) == logupload::Carrier::Tunnel,
        "carrier: the session's device");
  Check(logupload::CarrierFor(false, true) == logupload::Carrier::Provider,
        "carrier: the provider-only device while there is no session");
  Check(logupload::CarrierFor(false, false) == logupload::Carrier::Standalone,
        "carrier: a standalone device only when neither runs");
  Check(logupload::CarrierFor(true, true) == logupload::Carrier::Tunnel,
        "carrier: the session's device wins should both ever run");
  Check(std::string(logupload::ToString(logupload::Carrier::Tunnel)) == "tunnel" &&
            std::string(logupload::ToString(logupload::Carrier::Provider)) == "provider" &&
            std::string(logupload::ToString(logupload::Carrier::Standalone)) == "standalone",
        "carrier: the reply's words");
}

// (b) the standalone device's refusals: a held device or a pending restart,
// and nothing about the kill switch, whose armed floor permits this service
void TestStandaloneRefusals() {
  Check(logupload::StandaloneRefusalFor(false, false) == logupload::StandaloneRefusal::None,
        "refusal: none by default");
  Check(logupload::StandaloneRefusalFor(true, false) ==
            logupload::StandaloneRefusal::DeviceStillHeld,
        "refusal: a teardown still holds this identity");
  Check(logupload::StandaloneRefusalFor(false, true) ==
            logupload::StandaloneRefusal::RestartPending,
        "refusal: the service is restarting itself");
  Check(logupload::StandaloneRefusalFor(true, true) ==
            logupload::StandaloneRefusal::DeviceStillHeld,
        "refusal: the held device is named first");
  Check(std::string(logupload::RefusalReason(logupload::StandaloneRefusal::None)).empty(),
        "refusal: no reason for none");
  for (const logupload::StandaloneRefusal refusal :
       {logupload::StandaloneRefusal::DeviceStillHeld,
        logupload::StandaloneRefusal::RestartPending}) {
    Check(!std::string(logupload::RefusalReason(refusal)).empty(),
          "refusal: every refusal has a reason for the reply");
  }
}

// (c) how long a standalone device may wait for its upload
void TestStandaloneLifetime() {
  Check(logupload::kStandaloneDeviceMaxLifetime == std::chrono::minutes(30),
        "lifetime: 30 minutes without a report");
}

// (d) the app falls back only when the service did not take it
void TestAppStep() {
  Check(logupload::AppStepAfterService(true, false) == logupload::AppStep::Done,
        "app: the service took it");
  Check(logupload::AppStepAfterService(true, true) == logupload::AppStep::Done,
        "app: no second upload through the DeviceRemote beside the service's");
  Check(logupload::AppStepAfterService(false, true) == logupload::AppStep::DeviceRemote,
        "app: the old path while a session is bound");
  Check(logupload::AppStepAfterService(false, false) == logupload::AppStep::Skip,
        "app: nothing can carry it");
}

// (e) the request survives the wire, and an absent field is empty, not a throw
void TestUploadLogsJson() {
  Check(std::string(proto::msg::kUploadLogs) == "upload_logs", "wire: the verb");
  const proto::UploadLogs sent = SampleRequest();
  const nlohmann::json request = proto::Request(proto::msg::kUploadLogs, sent);
  Check(proto::TypeOf(request) == "upload_logs", "wire: the request carries its type");
  const nlohmann::json wire = nlohmann::json::parse(proto::DumpForWire(request));
  const proto::UploadLogs got = wire.get<proto::UploadLogs>();
  Check(got.feedback_id == sent.feedback_id, "wire: feedback_id");
  Check(got.by_jwt == sent.by_jwt, "wire: by_jwt");
  Check(got.network_space_json == sent.network_space_json, "wire: network_space_json");
  Check(got.instance_id == sent.instance_id, "wire: instance_id");
  Check(got.device_description == sent.device_description, "wire: device_description");
  Check(got.device_spec == sent.device_spec, "wire: device_spec");
  Check(got.app_version == sent.app_version, "wire: app_version");
  const proto::UploadLogs sparse =
      nlohmann::json::parse(R"({"feedback_id":null})").get<proto::UploadLogs>();
  Check(sparse.feedback_id.empty() && sparse.by_jwt.empty(),
        "wire: absent and null fields read as empty");
}

// (f) the reply names the device, only when it answers an upload; a service
// that predates the verb answers "unknown request type" and names none
void TestReply() {
  proto::Reply reply;
  reply.ok = true;
  reply.in_reply_to = proto::msg::kUploadLogs;
  reply.log_upload_carrier = "standalone";
  const proto::Reply back =
      nlohmann::json::parse(proto::DumpForWire(nlohmann::json(reply))).get<proto::Reply>();
  Check(back.ok && back.log_upload_carrier == "standalone", "reply: the carrier comes back");
  proto::Reply other;
  other.ok = true;
  Check(!nlohmann::json(other).contains("log_upload_carrier"),
        "reply: no carrier field in any other reply");
  const proto::Reply older =
      nlohmann::json::parse(
          R"({"type":"reply","ok":false,"error":"unknown request type: upload_logs"})")
          .get<proto::Reply>();
  Check(!older.ok && older.log_upload_carrier.empty(),
        "reply: an older service's answer is a refusal with no carrier");
}

// (g) the feedback id becomes a path segment of the API url, so only the
// server's own ids pass
void TestFeedbackId() {
  Check(proto::LooksLikeFeedbackId("f00dfeed-0000-4000-8000-000000000001"),
        "feedback id: a lower case uuid");
  Check(proto::LooksLikeFeedbackId("F00DFEED-0000-4000-8000-000000000001"),
        "feedback id: an upper case uuid");
  for (const char* bad : {
           "",
           "not-a-feedback-id",
           "../../network/provider-status",
           "f00dfeed-0000-4000-8000-00000000000",    // one short
           "f00dfeed-0000-4000-8000-0000000000012",  // one long
           "f00dfeed/0000-4000-8000-000000000001",   // a path separator for a dash
           "f00dfeed-0000-4000-8000-00000000000?",   // a query
           "f00dfeed-0000-4000-8000-00000000000 ",   // whitespace
           "{00dfeed-0000-4000-8000-00000000000}",   // braces
           "f00dfeed0000040000800000000000000001",   // no dashes
           "g00dfeed-0000-4000-8000-000000000001",   // not hex
       }) {
    Check(!proto::LooksLikeFeedbackId(bad), std::string("feedback id: refuses \"") + bad + "\"");
  }
}

}  // namespace

int main() {
  TestCarrier();
  TestStandaloneRefusals();
  TestStandaloneLifetime();
  TestAppStep();
  TestUploadLogsJson();
  TestReply();
  TestFeedbackId();
  std::cout << (gCases - gFailures) << "/" << gCases << " log upload checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
