// The Bluetooth settings page exists in the activity graph, but the Plus port
// has no BLE host stack yet (FREEINK_CAP_BLE_HID_HOST=0). Its implementation is
// compiled out in that configuration, so these no-op overrides keep the page
// constructible and the UI honest: it opens, lists nothing and cannot act.

#include "activities/settings/BluetoothSettingsActivity.h"

#if !FREEINK_CAP_BLE_HID_HOST

void BluetoothSettingsActivity::onEnter() { Activity::onEnter(); }
void BluetoothSettingsActivity::onExit() { Activity::onExit(); }
void BluetoothSettingsActivity::buildScreen(UiScreen& /*screen*/) {}
void BluetoothSettingsActivity::activateIndex(int /*index*/) {}
void BluetoothSettingsActivity::onRowLongPress(int /*index*/) {}
bool BluetoothSettingsActivity::handleCustomInput() { return false; }
bool BluetoothSettingsActivity::handleButtons() { return false; }
void BluetoothSettingsActivity::onBackButton() {}
void BluetoothSettingsActivity::drawChrome() {}

#endif
