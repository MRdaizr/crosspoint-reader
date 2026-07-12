#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TodoItem {
  uint32_t id = 0;
  std::string title;
  std::string scheduledAt;
  bool completed = false;
};

class TodoStore {
 public:
  static TodoStore& getInstance() { return instance; }

  bool getItems(std::vector<TodoItem>& items) const;
  bool add(const std::string& title, const std::string& scheduledAt, TodoItem& item);
  bool toggle(uint32_t id, TodoItem& item);
  bool remove(uint32_t id);

  static bool isValidScheduledAt(const std::string& value);

  static constexpr size_t MAX_ITEMS = 50;
  static constexpr size_t MAX_TITLE_BYTES = 120;

 private:
  static TodoStore instance;

  bool load(std::vector<TodoItem>& items, uint32_t& nextId) const;
  bool save(const std::vector<TodoItem>& items, uint32_t nextId) const;
  static void sortItems(std::vector<TodoItem>& items);
};

#define TODO_STORE TodoStore::getInstance()
