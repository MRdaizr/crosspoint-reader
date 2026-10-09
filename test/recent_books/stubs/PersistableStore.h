#pragma once
template <typename T>
class PersistableStore {
 public:
  unsigned saves = 0;
  static T& getInstance() {
    static T store;
    return store;
  }
  bool saveToFile() {
    ++saves;
    return true;
  }
};
