#ifndef VEXA_THUNKS_STUB_XLIB_H
#define VEXA_THUNKS_STUB_XLIB_H
/* VEXA_FIXES: Minimal Xlib types so vulkan.h's Xlib/Xrandr sections parse
   without real X11 headers. Layout matches LP64 Xlib: XID is unsigned long,
   Display is an opaque _XDisplay. */
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;
typedef XID VisualID;
typedef int Bool;
typedef int Status;
#endif
