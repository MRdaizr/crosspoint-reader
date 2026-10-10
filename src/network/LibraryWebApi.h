#pragma once

#include <LibraryBuilder.h>
class WebServer;

// Call after server creation, before begin(). GET is read-only; POST is an
// explicit synchronous rebuild. Callbacks may update transfer UI/poll cancel
// but must not recursively call handleClient or destroy the WebServer.
void registerLibraryWebApi(WebServer& server, const library::BuildCallbacks& callbacks = {});
void handleLibraryGet(WebServer& server);
void handleLibraryRebuild(WebServer& server, const library::BuildCallbacks& callbacks = {});
