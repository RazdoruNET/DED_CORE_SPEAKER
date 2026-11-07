#include "net/DeviceConfig.h"

DeviceConfig deviceConfig;

bool DeviceConfig::begin() {
  if (!_prefs.begin("spk_cfg", false)) {
    return false;
  }

  wifiSsid = _prefs.isKey("wifi_ssid") ? _prefs.getString("wifi_ssid") : String("");
  wifiPass = _prefs.isKey("wifi_pass") ? _prefs.getString("wifi_pass") : String("");
  apiBase = _prefs.isKey("api_base") ? _prefs.getString("api_base") : String("");
  deviceKey = _prefs.isKey("device_key") ? _prefs.getString("device_key") : String("");
  pairToken = _prefs.isKey("pair_token") ? _prefs.getString("pair_token") : String("");
  name = _prefs.isKey("name") ? _prefs.getString("name") : String("Колонка");
  room = _prefs.isKey("room") ? _prefs.getString("room") : String("");
  volume = _prefs.getUChar("volume", 60);
  muted = _prefs.getBool("muted", false);
  rulesVersion = _prefs.getULong("rules_ver", 0);

  return true;
}

void DeviceConfig::saveNetwork() {
  _prefs.putString("wifi_ssid", wifiSsid);
  _prefs.putString("wifi_pass", wifiPass);
  _prefs.putString("api_base", apiBase);
}

void DeviceConfig::saveIdentity() {
  _prefs.putString("device_key", deviceKey);
  _prefs.putString("pair_token", pairToken);
  _prefs.putString("name", name);
  _prefs.putString("room", room);
}

void DeviceConfig::saveVolume() {
  _prefs.putUChar("volume", volume);
  _prefs.putBool("muted", muted);
  _prefs.putULong("rules_ver", rulesVersion);
  _volumeDirty = false;
}

void DeviceConfig::tickVolume() {
  if (!_volumeDirty) {
    return;
  }
  if (millis() - _volumeChangedAt < VOLUME_SAVE_DELAY) {
    return;
  }
  saveVolume();
}

void DeviceConfig::clear() {
  _prefs.clear();
  wifiSsid = "";
  wifiPass = "";
  apiBase = "";
  deviceKey = "";
  pairToken = "";
  name = "Колонка";
  room = "";
  volume = 60;
  muted = false;
  rulesVersion = 0;
}
