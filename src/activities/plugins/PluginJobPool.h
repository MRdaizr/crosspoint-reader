#pragma once
#include <cstdint>
#include <cstring>
#include <limits>

#include "util/PluginLocations.h"

// Lives only in CrossPointWebServer. Browser JavaScript executes every action;
// firmware stores six bounded records and never evaluates JS.
class PluginJobPool {
 public:
  static constexpr size_t CAPACITY = 6;
  static constexpr size_t JSON_CAPACITY = 192;
  static constexpr uint32_t LEASE_MS = 10UL * 60 * 1000;
  enum State : uint8_t { Empty, Pending, Running, Done, Error };
  struct Job {
    uint32_t id = 0, claim = 0, updatedAt = 0;
    State state = Empty;
    char plugin[24] = {}, action[24] = {}, args[JSON_CAPACITY] = {}, result[JSON_CAPACITY] = {};
  };
  static void clear(Job& job) {
    job.id = job.claim = job.updatedAt = 0;
    job.state = Empty;
    memset(job.plugin, 0, sizeof(job.plugin));
    memset(job.action, 0, sizeof(job.action));
    memset(job.args, 0, sizeof(job.args));
    memset(job.result, 0, sizeof(job.result));
  }
  static bool validAction(const char* action) {
    if (!action || !*action || strlen(action) >= 24) return false;
    for (const char* c = action; *c; ++c)
      if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' ||
            *c == '_' || *c == '.'))
        return false;
    return true;
  }
  void reset(uint32_t seed = 1) {
    for (auto& job : jobs) clear(job);
    nextId = seed ? seed : 1;
    nextClaim = (seed ^ 0x5a719bc3u) ? (seed ^ 0x5a719bc3u) : 1;
  }
  Job* submit(const char* plugin, const char* action, const char* args, uint32_t now, bool authorized) {
    if (!authorized || !PluginLocations::validName(plugin) || !validAction(action) || !args ||
        strlen(args) >= JSON_CAPACITY)
      return nullptr;
    Job* best = nullptr;
    uint32_t oldest = 0;
    for (auto& job : jobs) {
      if (job.state == Empty) {
        best = &job;
        break;
      }
      if ((job.state == Done || job.state == Error) && (!best || now - job.updatedAt > oldest)) {
        best = &job;
        oldest = now - job.updatedAt;
      }
    }
    if (!best) return nullptr;
    clear(*best);
    do {
      best->id = nextId++;
    } while (best->id == 0);
    best->state = Pending;
    best->updatedAt = now;
    strcpy(best->plugin, plugin);
    strcpy(best->action, action);
    strcpy(best->args, args);
    return best;
  }
  void expire(uint32_t now) {
    for (auto& job : jobs)
      if (job.state == Running && now - job.updatedAt >= LEASE_MS) {
        job.state = Pending;
        job.claim = 0;  // preserve submit order through updatedAt
      }
  }
  Job* claim(const char* plugin, uint32_t now, bool authorized) {
    if (!authorized || !PluginLocations::validName(plugin)) return nullptr;
    expire(now);
    Job* best = nullptr;
    for (auto& job : jobs) {
      if (job.state == Pending && strcmp(job.plugin, plugin) == 0 &&
          (!best || now - job.updatedAt > now - best->updatedAt))
        best = &job;
    }
    if (!best) return nullptr;
    best->state = Running;
    do {
      best->claim = nextClaim++;
    } while (best->claim == 0);
    best->updatedAt = now;
    return best;
  }
  Job* find(uint32_t id) {
    if (!id) return nullptr;
    for (auto& job : jobs)
      if (job.id == id && job.state != Empty) return &job;
    return nullptr;
  }
  bool complete(uint32_t id, uint32_t lease, bool ok, const char* result, uint32_t now, bool authorized) {
    Job* job = find(id);
    if (!authorized || !job || !lease || job->claim != lease || !result || strlen(result) >= JSON_CAPACITY)
      return false;
    if (job->state == Done || job->state == Error) return true;  // identical lease's retry
    if (job->state != Running || now - job->updatedAt >= LEASE_MS) return false;
    strcpy(job->result, result);
    job->state = ok ? Done : Error;
    job->updatedAt = now;
    return true;
  }

 private:
  Job jobs[CAPACITY];
  uint32_t nextId = 1, nextClaim = 1;
};
