#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>

#include <mutex>
#include <string>

/**
 * Shared JSON file I/O for persistable stores.
 *
 * Keeping ArduinoJson serialization in one translation unit avoids emitting a
 * copy of the parser/serializer in every store implementation.
 */
class PersistableStoreBase {
 protected:
  PersistableStoreBase() = default;
  ~PersistableStoreBase() = default;

  // Held across a save so concurrent tasks cannot reorder complete snapshots.
  // Read-only accessors must not take this lock: they should snapshot under
  // their own short state lock and never block on SD I/O.
  mutable std::mutex storeMutex;

  // Legacy readers request a write-back after parsing; the CRTP loader saves
  // after releasing storeMutex to avoid recursive locking.
  void requestResave() { resaveRequested = true; }
  bool resaveRequested = false;

 public:
  static bool writeDocToFile(const char* path, const JsonDocument& doc);
  static bool readDocFromFile(const char* path, JsonDocument& doc);

 protected:
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave);
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave, size_t maxLength, bool& valid);
};

/**
 * CRTP helper for stores with a single JSON file.
 *
 * Derived classes provide getFilePath(), toJson() and fromJson(). Existing
 * stores may keep a custom load path when they need binary migration or lazy
 * loading, while still using PersistableStoreBase for common JSON I/O.
 */
template <typename T>
class PersistableStore : public PersistableStoreBase {
 protected:
  PersistableStore() = default;
  ~PersistableStore() = default;

 public:
  PersistableStore(const PersistableStore&) = delete;
  PersistableStore& operator=(const PersistableStore&) = delete;

  static T& getInstance() {
    static T instance;
    return instance;
  }

  bool saveToFile() const {
    std::lock_guard<std::mutex> lock(storeMutex);
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }

  bool loadFromFile() {
    bool ok;
    bool doResave;
    {
      std::lock_guard<std::mutex> lock(storeMutex);
      resaveRequested = false;
      JsonDocument doc;
      if (!readDocFromFile(T::getFilePath(), doc)) return false;
      ok = static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
      doResave = resaveRequested;
      resaveRequested = false;
    }
    if (ok && doResave && !saveToFile()) {
      LOG_ERR("PERSIST", "Failed to resave %s after format update", T::getFilePath());
    }
    return ok;
  }
};
