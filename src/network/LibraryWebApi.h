#pragma once

#include <LibraryBuilder.h>
#include <LibraryQuery.h>
class WebServer;

// One bounded query session per running web server. Release the file before a
// rebuild and drop the whole session when file transfer stops.
struct LibraryWebQuerySession {
  library::LibraryIndexFile index;
  library::LibraryQuery query;
  uint32_t generation = 0;
  void reset() {
    index.close();
    query.invalidate();
  }
};

// Call after server creation, before begin(). GET is read-only; POST is an
// explicit synchronous rebuild. Callbacks may update transfer UI/poll cancel
// but must not recursively call handleClient or destroy the WebServer.
void registerLibraryWebApi(WebServer& server, const library::BuildCallbacks& callbacks = {},
                           LibraryWebQuerySession* session = nullptr);
void handleLibraryGet(WebServer& server, LibraryWebQuerySession* session = nullptr);
void handleLibraryRebuild(WebServer& server, const library::BuildCallbacks& callbacks = {},
                          LibraryWebQuerySession* session = nullptr);
