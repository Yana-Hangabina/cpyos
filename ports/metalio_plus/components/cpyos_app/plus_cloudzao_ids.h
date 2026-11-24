#pragma once

// Public identifiers of the CPYOS product on CloudZao Burner's partner API.
// These are not credentials: the partner update query embeds them in the image
// and the documentation says integrationId may be public.  They replace the
// retired X-Resource-Key firmware-OTA/resource endpoints (HTTP 410
// partner_api_retired, verified 2026-11-24).
namespace plus_cloudzao {

constexpr const char* kBaseUrl = "https://claw.cloudzao.cn/xiaozhi/api/partner/v1";
constexpr const char* kIntegrationId = "c38660f49f6d8d6a3c2efc32fe4acb4c";
constexpr const char* kFirmwareId = "e72b32b47a121318e53bf155b666d6b9";

}  // namespace plus_cloudzao
