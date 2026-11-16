#pragma once

// Pages where the physical AI/BOOT key must not open or replace the page.
// Kept out of shell_demo so a host test can pin the list.
namespace plus_ai_key {

bool routingBlocked(const char* activityName);

}  // namespace plus_ai_key
