/*
$info$
tags: thunklibs|X11
desc: Android guest-side X11 compatibility surface for Noesis clipboard probing
$end_info$
*/

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
struct _XDisplay;
using Display = _XDisplay*;
using Window = unsigned long;
using Atom = unsigned long;
using Time = unsigned long;

struct XEvent {
  int type {};
  unsigned long serial {};
  int send_event {};
  Display display {};
  Window window {};
  long pad[24] {};
};

Display XOpenDisplay(const char*) {
  return nullptr;
}

Window XDefaultRootWindow(Display) {
  return 0;
}

Window XCreateSimpleWindow(Display, Window, int, int, unsigned int, unsigned int,
                           unsigned int, unsigned long, unsigned long) {
  return 0;
}

Atom XInternAtom(Display, const char*, int) {
  return 0;
}

int XChangeProperty(Display, Window, Atom, Atom, int, int, const unsigned char*, int) {
  return 0;
}

int XDeleteProperty(Display, Window, Atom) {
  return 0;
}

int XGetWindowProperty(Display, Window, Atom, long, long, int, Atom,
                       Atom* actual_type_return, int* actual_format_return,
                       unsigned long* nitems_return, unsigned long* bytes_after_return,
                       unsigned char** prop_return) {
  if (actual_type_return) {
    *actual_type_return = 0;
  }
  if (actual_format_return) {
    *actual_format_return = 0;
  }
  if (nitems_return) {
    *nitems_return = 0;
  }
  if (bytes_after_return) {
    *bytes_after_return = 0;
  }
  if (prop_return) {
    *prop_return = nullptr;
  }

  // Mirrors the "no X server / no clipboard owner" path expected by Noesis.
  return 1;
}

int XConvertSelection(Display, Atom, Atom, Atom, Window, Time) {
  return 0;
}

int XSelectInput(Display, Window, long) {
  return 0;
}

int XNextEvent(Display, XEvent* event_return) {
  if (event_return) {
    std::memset(event_return, 0, sizeof(*event_return));
  }
  return 0;
}

int XSync(Display, int) {
  return 0;
}

int XFree(void*) {
  return 0;
}
}
