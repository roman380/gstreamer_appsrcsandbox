# GStreamer `appsrc` Sandbox

Record data pushed via appsrc elements in your application into file, and replay retraactively. 

Use helper `AppsrcFile` class from [record.h](record.h) to produce the replay files.

See also:

- GStreamer [`appsrc` element](https://gstreamer.freedesktop.org/documentation/app/appsrc.html)

## `app` directory

A copy/fork of `appsink` from [gst-plugins-base/gst-libs/gst/app/](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/tree/main/subprojects/gst-plugins-base/gst/app), as of `6a4425e46a8b69c5b3d616bdbaa84c6f908907d3` (GStreamer 1.14.5 + edits) is included in the repository and can be used (except Windows) in the replay.
