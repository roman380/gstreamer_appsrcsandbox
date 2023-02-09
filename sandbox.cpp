#include <memory>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
#include <chrono>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#if defined(WIN32) && !defined(NDEBUG)
#include <windows.h>
#endif

void set_pipeline_state (GstPipeline* pipeline, GstState state)
{
  g_assert_nonnull (pipeline);
  auto const result = gst_element_set_state (GST_ELEMENT_CAST (pipeline), state);
  g_assert_true (result >= GST_STATE_CHANGE_SUCCESS);
  if (result == GST_STATE_CHANGE_ASYNC)
    g_assert_true (gst_element_get_state (GST_ELEMENT_CAST (pipeline), nullptr, nullptr, GST_CLOCK_TIME_NONE) != GST_STATE_CHANGE_FAILURE);
}

struct Application {

  Application () = default;
  ~Application () = default;

  void push (std::atomic_bool& termination, std::string path)
  {
    std::ifstream stream;
    stream.open (path, std::ios_base::in | std::ios_base::binary);
    g_assert_true (stream);
    for (; !termination.load ();) {
      if (source != nullptr)
        break;
      std::this_thread::sleep_for (std::chrono::milliseconds (200));
    }
    for (; !termination.load () && !stream.eof ();) {
      uint8_t type;
      stream.read (reinterpret_cast<char*> (&type), sizeof type);
      g_assert_nonnull (source);
      switch (type) {
        case 1: {
          uint16_t size;
          stream.read (reinterpret_cast<char*> (&size), sizeof size);
          std::string caps_string;
          if (size) {
            caps_string.resize (size);
            stream.read (reinterpret_cast<char*> (caps_string.data ()), caps_string.size ());
          }
          GstCaps* caps = gst_caps_from_string (caps_string.c_str ());
          gst_app_src_set_caps (source, caps);
          gst_caps_unref (caps);
        } break;
        case 2: {
          uint32_t size;
          stream.read (reinterpret_cast<char*> (&size), sizeof size);
          std::vector<uint8_t> data;
          data.resize (size);
          stream.read (reinterpret_cast<char*> (data.data ()), data.size ());
          GstBuffer* buffer = gst_buffer_new_allocate (nullptr, size, nullptr);
          gst_buffer_fill (buffer, 0, data.data (), data.size ());
          gst_app_src_push_buffer (source, buffer);
        } break;
        case 3: {
          gst_app_src_end_of_stream (source);
        } break;
        default:
          g_assert_not_reached ();
      }
    }
  }

  void handle_source_setup (GstElement* element)
  {
    GST_INFO_OBJECT (element, "playbin source setup, %hs", GST_ELEMENT_NAME (element));
    source = GST_APP_SRC (element);
    gst_object_ref (GST_OBJECT_CAST (source));
  }
  void handle_element_setup (GstElement* element)
  {
    GST_INFO_OBJECT (element, "playbin element setup, %hs", GST_ELEMENT_NAME (element));
  }

  void handle_bus_error_message (GstBus* bus, GstMessage* message)
  {
    g_assert_nonnull (bus);
    g_assert_nonnull (message);
    GError* error = nullptr;
    gchar* debug_message = nullptr;
    gst_message_parse_error (message, &error, &debug_message);
    g_printerr ("handle_bus_error_message: %s, %s\n", GST_MESSAGE_SRC_NAME (message), error->message);
    if (debug_message)
      g_printerr ("%s\n", debug_message);
    g_clear_error (&error);
    g_free (debug_message);
  }
  void handle_bus_eos_message (GstBus* bus, GstMessage* message)
  {
    g_assert_nonnull (bus);
    g_assert_nonnull (message);
    g_print ("handle_bus_eos_message\n");
  }
  void handle_bus_state_changed_message (GstBus* bus, GstMessage* message)
  {
    g_assert_nonnull (bus);
    g_assert_nonnull (message);
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed (message, &old_state, &new_state, &pending_state);
    g_print ("handle_bus_state_changed_message: %s, %s to %s, pending %s\n", GST_MESSAGE_SRC_NAME (message), gst_element_state_get_name (new_state), gst_element_state_get_name (old_state), gst_element_state_get_name (pending_state));
  }

  GstPipeline* pipeline = nullptr;
  GstElement* playbin = nullptr;
  GstAppSrc* source = nullptr;
};

inline void add_debug_output_log_function ()
{
#if defined(WIN32) && !defined(NDEBUG)
  auto const log_function = [] (GstDebugCategory* category, GstDebugLevel level, gchar const* file, gchar const* function, gint line, GObject* object, GstDebugMessage* message, gpointer) {
    g_assert_true (category && message);
    if (level > category->threshold)
      return;
    std::ostringstream stream;
    char prefix[512];
    sprintf_s (prefix, "%hs(%d): %hs: ", file, line, function);
    stream << prefix << gst_debug_log_get_line (category, level, file, function, line, object, message); // << std::endl;
    OutputDebugStringA (stream.str ().c_str ());
  };
  gst_debug_add_log_function (log_function, nullptr, nullptr);
#endif
}

int main (int argc, char* argv[])
{
  gst_init (&argc, &argv);

#if defined(WIN32) && !defined(NDEBUG)
  add_debug_output_log_function ();
  gst_debug_set_active (true);
  gst_debug_set_default_threshold (GST_LEVEL_INFO);
  // gst_debug_set_threshold_for_name ("...", GST_LEVEL_DEBUG);
#if defined(WIN32) && !defined(NDEBUG)

  Application application;
  application.pipeline = GST_PIPELINE_CAST (gst_pipeline_new ("pipeline"));
  g_assert_nonnull (application.pipeline);
  GstBus* bus = gst_element_get_bus (GST_ELEMENT_CAST (application.pipeline));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", G_CALLBACK (+[] (GstBus* bus, GstMessage* message, Application* application) -> void { application->handle_bus_error_message (bus, message); }), &application);
  g_signal_connect (G_OBJECT (bus), "message::eos", G_CALLBACK (+[] (GstBus* bus, GstMessage* message, Application* application) -> void { application->handle_bus_eos_message (bus, message); }), &application);
  g_signal_connect (G_OBJECT (bus), "message::state-changed", G_CALLBACK (+[] (GstBus* bus, GstMessage* message, Application* application) -> void { application->handle_bus_state_changed_message (bus, message); }), &application);

  application.playbin = gst_element_factory_make ("playbin", nullptr);
  g_assert_nonnull (application.playbin);
  g_object_set (application.playbin,
      "uri", "appsrc://",
      "flags", 3, // static_cast<GstPlayFlags>(GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO),
      nullptr);
  g_signal_connect (application.playbin, "source-setup", G_CALLBACK (+[] (GstElement* pipeline, GstElement* element, Application* application) { application->handle_source_setup (element); }), &application);
  g_signal_connect (application.playbin, "element-setup", G_CALLBACK (+[] (GstElement* pipeline, GstElement* element, Application* application) { application->handle_element_setup (element); }), &application);
  gst_bin_add (GST_BIN (application.pipeline), application.playbin);
  gst_element_sync_state_with_parent (application.playbin);

  std::string path = "../data/AppSrc-Video";
  std::replace (path.begin (), path.end (), '/', static_cast<char> (std::filesystem::path::preferred_separator));
  std::atomic_bool push_thread_termination = false;
  std::thread push_thread ([&] { application.push (push_thread_termination, path); });

  set_pipeline_state (application.pipeline, GST_STATE_PLAYING);
  auto message = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, static_cast<GstMessageType> (GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (application.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox"); // https://gstreamer.freedesktop.org/documentation/tutorials/basic/debugging-tools.html?gi-language=c
  g_assert_true (message != nullptr);
  g_assert_true (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS);
  gst_message_unref (std::exchange (message, nullptr));

  push_thread_termination.store (true);
  push_thread.join ();

  gst_bus_remove_signal_watch (bus);
  gst_object_unref (std::exchange (bus, nullptr));

  gst_element_set_state (GST_ELEMENT_CAST (application.pipeline), GST_STATE_NULL);
  gst_object_unref (std::exchange (application.playbin, nullptr));
  gst_object_unref (std::exchange (application.pipeline, nullptr));

  return 0;
}
