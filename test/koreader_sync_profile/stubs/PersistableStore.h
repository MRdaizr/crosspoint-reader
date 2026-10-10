#pragma once

#include <ArduinoJson.h>
#include <HalStorage.h>

#include <string>

template <typename T>
class PersistableStore {
 protected:
  void requestResave() { resaveRequested = true; }
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave) {
    if (doc["password_obf"].is<const char*>()) return doc["password_obf"].as<std::string>();
    needsResave = !doc["password"].isNull();
    return doc["password"] | "";
  }

 public:
  bool resaveRequested = false;
  inline static bool failWrite = false;
  static T& getInstance() {
    static T store;
    return store;
  }
  bool saveToFile() const {
    if (failWrite) return false;
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    serializeJson(doc, Storage.files[T::getFilePath()]);
    return true;
  }
  bool loadFromFile() {
    JsonDocument doc;
    if (deserializeJson(doc, Storage.files[T::getFilePath()])) return false;
    return static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
  }
};
