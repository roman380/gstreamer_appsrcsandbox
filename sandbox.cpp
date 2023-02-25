#include <memory>
#include <algorithm>
#include <vector>
#include <list>
#include <string>
#include <sstream>
#include <filesystem>
//#include <experimental/filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#if defined(WIN32) && !defined(NDEBUG)
#  include <windows.h>
#endif

#define GETTEXT_PACKAGE "gstreamer_appsrcsandbox"

// Commandline option parser https://developer-old.gnome.org/glib/unstable/glib-Commandline-option-parser.html

static gchar* g_path = nullptr;
static gint g_video_mode = 0;
static gboolean g_internal_appsink = false;
static guint g_bin_count = 1;
static guint g_video_bin_index = 0;

static GOptionEntry g_option_context_entries[] {
  { "path", 'p', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_STRING, &g_path, "Path to input file to play back", nullptr },
  { "sink-mode", 's', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_video_mode, "Playbin video-sink mode mode (0 - default sink, 1 - I420 appsink, 2 - I420 capsfilter & appsink)", nullptr },
  { "internal-appsink", 'i', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &g_internal_appsink, "Use internal appsink", nullptr },
  { "bin-count", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_bin_count, "Number of bins in the pipeline and presumably in the supplied replay input", nullptr },
  { "video-bin-index", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_video_bin_index, "Index of video bin/stream in the multi-bin configuration", nullptr },
  { nullptr }
};

GST_DEBUG_CATEGORY_STATIC (application_category); // https://gstreamer.freedesktop.org/documentation/tutorials/basic/debugging-tools.html#adding-your-own-debug-information
#define GST_CAT_DEFAULT application_category

void set_pipeline_state (GstPipeline* pipeline, GstState state)
{
  g_assert_nonnull (pipeline);
  const auto result = gst_element_set_state (GST_ELEMENT_CAST (pipeline), state);
  g_assert_true (result >= GST_STATE_CHANGE_SUCCESS);
  if (result == GST_STATE_CHANGE_ASYNC)
    g_assert_true (gst_element_get_state (GST_ELEMENT_CAST (pipeline), nullptr, nullptr, GST_CLOCK_TIME_NONE) != GST_STATE_CHANGE_FAILURE);
}

struct Application {
  struct Bin {
    void create_playbin ()
    {
      playbin = gst_element_factory_make ("playbin", nullptr); // https://gstreamer.freedesktop.org/documentation/playback/playbin.html#properties
      g_assert_nonnull (playbin);
      g_object_set (playbin,
          "uri", "appsrc://",
          "flags", 3, // static_cast<GstPlayFlags>(GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO), // https://gstreamer.freedesktop.org/documentation/playback/playsink.html#GstPlayFlags
          nullptr);
      g_signal_connect (playbin, "source-setup", G_CALLBACK (+[] (GstElement* pipeline, GstElement* element, Bin* bin) { bin->handle_source_setup (element); }), this);
      g_signal_connect (playbin, "element-setup", G_CALLBACK (+[] (GstElement* pipeline, GstElement* element, Bin* bin) { bin->handle_element_setup (element); }), this);
    }
    void create_sink ()
    {
      const auto connect_sink_signals = [&] {
        g_object_set (G_OBJECT (sink), "emit-signals", TRUE, nullptr);
        g_signal_connect (sink, "new-preroll", G_CALLBACK (+[] (GstAppSink* sink, Bin* bin) -> GstFlowReturn { return bin->handle_sink_preroll_sample (sink); }), this);
        g_signal_connect (sink, "new-sample", G_CALLBACK (+[] (GstAppSink* sink, Bin* bin) -> GstFlowReturn { return bin->handle_sink_sample (sink); }), this);
        g_signal_connect (sink, "eos", G_CALLBACK (+[] (GstAppSink* sink, Bin* bin) { bin->handle_sink_eos (sink); }), this);
      };
      const auto set_sink_callbacks = [&] {
        g_object_set (G_OBJECT (sink), "emit-signals", FALSE, nullptr);
        GstAppSinkCallbacks callbacks;
        callbacks.eos = ([] (GstAppSink* sink, gpointer bin) { reinterpret_cast<Bin*> (bin)->handle_sink_eos (sink); });
        callbacks.new_preroll = ([] (GstAppSink* sink, gpointer bin) -> GstFlowReturn { return reinterpret_cast<Bin*> (bin)->handle_sink_preroll_sample (sink); });
        callbacks.new_sample = ([] (GstAppSink* sink, gpointer bin) -> GstFlowReturn { return reinterpret_cast<Bin*> (bin)->handle_sink_sample (sink); });
        gst_app_sink_set_callbacks (sink, &callbacks, this, nullptr);
      };

      // NOTE: 0 - default sink, visual rendering
      //       1 - appsink, restricted to I420
      //       2 - bin with capsfilter and appsink, restricted to I420
      switch (g_video_mode) {
        case 0:
          break;
        case 1: {
          if (index == g_video_bin_index) {
            sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", "video_sink"));
            GstCaps* caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
            g_object_set (G_OBJECT (sink),
                "sync", FALSE,
                "caps", caps,
                "max-buffers", static_cast<guint> (32),
                nullptr);
            set_sink_callbacks (); // connect_sink_signals ();
            g_object_set (G_OBJECT (playbin), "video-sink", GST_ELEMENT_CAST (sink), nullptr);
          }
        } break;
        case 2: {
          g_assert_true (g_bin_count == 1 && g_video_bin_index == 0);
          auto capsfilter = gst_element_factory_make ("capsfilter", "video_caps"); // https://gstreamer.freedesktop.org/documentation/coreelements/capsfilter.html#capsfilter-page
          GstCaps* caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
          g_object_set (G_OBJECT (capsfilter),
              "caps", caps,
              nullptr);
          sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", "video_sink"));
          g_object_set (G_OBJECT (sink),
              "sync", FALSE,
              "max-buffers", static_cast<guint> (32),
              "emit-signals", TRUE,
              nullptr);
          set_sink_callbacks (); // connect_sink_signals ();
          auto sink_bin = GST_BIN_CAST (gst_bin_new ("sink_bin"));
          gst_bin_add_many (sink_bin, capsfilter, GST_ELEMENT_CAST (sink), nullptr);
          gst_element_link_many (capsfilter, GST_ELEMENT_CAST (sink), nullptr);
          auto pad = gst_element_get_static_pad (capsfilter, "sink");
          auto ghost_pad = gst_ghost_pad_new ("sink", pad);
          gst_pad_set_active (ghost_pad, TRUE);
          gst_element_add_pad (GST_ELEMENT_CAST (sink_bin), std::exchange (ghost_pad, nullptr));
          gst_object_unref (std::exchange (pad, nullptr));
          g_object_set (G_OBJECT (playbin), "video-sink", GST_ELEMENT_CAST (sink_bin), nullptr);
        } break;
      }
    }

    void handle_source_setup (GstElement* element)
    {
      GST_INFO_OBJECT (element, "%u: handle_source_setup, %s", index, GST_ELEMENT_NAME (element));
      source = GST_APP_SRC (element);
      gst_object_ref (GST_OBJECT_CAST (source));
      g_object_set (source, // https://gstreamer.freedesktop.org/documentation/app/appsrc.html?gi-language=c
          "max-bytes", static_cast<guint64> (2 << 20),
          "min-percent", static_cast<guint> (50),
          nullptr);
      g_object_set (source, "format", GST_FORMAT_TIME, nullptr);
      g_signal_connect (source, "enough-data", G_CALLBACK (+[] (GstElement* Element, Bin* bin) { bin->handle_enough_data (); }), this);
      g_signal_connect (source, "need-data", G_CALLBACK (+[] (GstElement* Element, guint DataSize, Bin* bin) { bin->handle_need_data (); }), this);
      source_data_need.store (true);
    }
    void handle_element_setup (GstElement* element)
    {
      GST_INFO_OBJECT (element, "%u: handle_element_setup, %s", index, GST_ELEMENT_NAME (element));
    }

    void handle_enough_data ()
    {
      GST_WARNING ("%u: handle_enough_data", index);
      // GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox-handle_enough_data");
      std::unique_lock source_data_lock (source_data_mutex);
      source_data_need.store (false);
      source_data_condition.notify_all ();
    }
    void handle_need_data ()
    {
      GST_WARNING ("%u: handle_need_data", index);
      // GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox-handle_need_data");
      std::unique_lock source_data_lock (source_data_mutex);
      source_data_need.store (true);
      source_data_condition.notify_all ();
    }

    GstFlowReturn handle_sink_preroll_sample (GstAppSink* sink)
    {
      GST_DEBUG_OBJECT (sink, "handle_sink_preroll_sample");
      g_assert_nonnull (sink);
      for (;;) {
        const auto sample = gst_app_sink_try_pull_preroll (sink, 0);
        if (!sample)
          break;
        GST_INFO_OBJECT (sample, "handle_sink_preroll_sample: %s", sample_text (sample).c_str ());
        gst_sample_unref (sample);
      }
      return GstFlowReturn::GST_FLOW_OK;
    }
    GstFlowReturn handle_sink_sample (GstAppSink* sink)
    {
      GST_DEBUG_OBJECT (sink, "handle_sink_sample");
      g_assert_nonnull (sink);
      for (;;) {
        const auto sample = gst_app_sink_try_pull_sample (sink, 0);
        if (!sample)
          break;
        GST_INFO_OBJECT (sample, "handle_sink_sample: %s", sample_text (sample).c_str ());
        gst_sample_unref (sample);
      }
      return GstFlowReturn::GST_FLOW_OK;
    }
    void handle_sink_eos (GstAppSink* sink)
    {
      GST_INFO_OBJECT (sink, "handle_sink_eos");
    }

    Application* application;
    guint index;
    GstElement* playbin = nullptr;
    GstAppSrc* source = nullptr;
    std::mutex source_data_mutex;
    std::condition_variable source_data_condition;
    std::atomic_bool source_data_need;
    bool end_of_stream = false;
    GstAppSink* sink = nullptr;
  };

  Application () = default;
  ~Application ()
  {
    if (pipeline)
      gst_object_unref (GST_OBJECT (std::exchange (pipeline, nullptr)));
  }

  static std::string buffer_flags_to_string (guint flags)
  {
    static const std::initializer_list<std::pair<guint, const char*>> g_list { // clang-format off
      #define IDENTIFIER(Name) std::make_pair<guint, const char*>(Name, #Name),
      IDENTIFIER (GST_BUFFER_FLAG_LIVE)
      IDENTIFIER (GST_BUFFER_FLAG_DECODE_ONLY)
      IDENTIFIER (GST_BUFFER_FLAG_DISCONT)
      IDENTIFIER (GST_BUFFER_FLAG_RESYNC)
      IDENTIFIER (GST_BUFFER_FLAG_CORRUPTED)
      IDENTIFIER (GST_BUFFER_FLAG_MARKER)
      IDENTIFIER (GST_BUFFER_FLAG_HEADER)
      IDENTIFIER (GST_BUFFER_FLAG_GAP)
      IDENTIFIER (GST_BUFFER_FLAG_DROPPABLE)
      IDENTIFIER (GST_BUFFER_FLAG_DELTA_UNIT)
      IDENTIFIER (GST_BUFFER_FLAG_TAG_MEMORY)
      IDENTIFIER (GST_BUFFER_FLAG_SYNC_AFTER)
      IDENTIFIER (GST_BUFFER_FLAG_NON_DROPPABLE)
      #undef IDENTIFIER
    }; // clang-format on
    std::ostringstream stream;
    for (auto&& element : g_list)
      if (flags & element.first)
        stream << " | " << element.second;
    auto string = stream.str ();
    if (string.empty ())
      return "0";
    string.erase (0, 3);
    return string;
  }
  static std::string buffer_to_string (GstBuffer* buffer)
  {
    std::ostringstream stream;
    char text[256];
    // sprintf (text, "flags 0x%04X", GST_BUFFER_FLAGS (buffer));
    stream << buffer_flags_to_string (GST_BUFFER_FLAGS (buffer)); // text;
    if (GST_BUFFER_DTS_IS_VALID (buffer)) {
      sprintf (text, ", dts %.3f", GST_BUFFER_DTS (buffer) / 1E9);
      stream << text;
    }
    if (GST_BUFFER_PTS_IS_VALID (buffer)) {
      sprintf (text, ", pts %.3f", GST_BUFFER_PTS (buffer) / 1E9);
      stream << text;
    }
    if (GST_BUFFER_DURATION_IS_VALID (buffer)) {
      sprintf (text, ", duration %.3f", GST_BUFFER_DURATION (buffer) / 1E9);
      stream << text;
    }
    return stream.str ();
  }
  void push (std::atomic_bool& termination, std::string path)
  {
    std::ifstream stream;
    stream.open (path, std::ios_base::in | std::ios_base::binary);
    g_assert_true (stream);
    for (; !termination.load ();) {
      if (std::all_of (bin_list.cbegin (), bin_list.cend (), [&] (auto&& bin) { return bin.source != nullptr; }))
        break;
      std::this_thread::sleep_for (std::chrono::milliseconds (200));
    }
    GST_INFO ("Before pushing data");
    std::once_flag stream_warnning;
    for (; !termination.load () && !stream.eof ();) {
      uint8_t type;
      stream.read (reinterpret_cast<char*> (&type), sizeof type);
      if (stream.fail ())
        break;
      uint8_t index;
      stream.read (reinterpret_cast<char*> (&index), sizeof index);
      if (stream.fail ())
        break;
      if (index >= bin_list.size ()) {
        std::call_once (stream_warnning, [&] {
          GST_ERROR ("Trying to play packet for stream %u in %zu-bin configuration, use --bin-count", index, bin_list.size ());
        });
        continue;
      }
      auto iterator = bin_list.begin ();
      std::advance (iterator, index);
      auto& bin = *iterator;
      g_assert_nonnull (bin.source);
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
          GST_INFO ("%u: gst_app_src_set_caps: %s", bin.index, caps_string.c_str ());
#if 0
          {
            gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING, "byte-stream", nullptr);
            gchar* caps_string = gst_caps_to_string (caps);
            GST_INFO ("gst_app_src_set_caps: %s", caps_string);
            g_free (caps_string);
          }
#endif
          gst_app_src_set_caps (bin.source, caps);
          gst_caps_unref (caps);
        } break;
        case 2: {
          uint64_t flags;
          int64_t dts;
          int64_t pts;
          int64_t duration;
          stream.read (reinterpret_cast<char*> (&flags), sizeof flags);
          stream.read (reinterpret_cast<char*> (&dts), sizeof dts);
          stream.read (reinterpret_cast<char*> (&pts), sizeof pts);
          stream.read (reinterpret_cast<char*> (&duration), sizeof duration);
          uint32_t size;
          stream.read (reinterpret_cast<char*> (&size), sizeof size);
          std::vector<uint8_t> data;
          data.resize (size);
          stream.read (reinterpret_cast<char*> (data.data ()), data.size ());
          GstBuffer* buffer = gst_buffer_new_allocate (nullptr, size, nullptr);
          GST_BUFFER_FLAGS (buffer) = static_cast<guint> (flags);
          GST_BUFFER_DTS (buffer) = static_cast<GstClockTime> (dts);
          GST_BUFFER_PTS (buffer) = static_cast<GstClockTime> (pts);
          GST_BUFFER_DURATION (buffer) = static_cast<GstClockTime> (duration);
          gst_buffer_fill (buffer, 0, data.data (), data.size ());
          {
            std::unique_lock source_data_lock (bin.source_data_mutex);
            bin.source_data_condition.wait (source_data_lock, [&] { return bin.source_data_need.load () || termination.load (); });
          }
          GST_INFO ("%u: gst_app_src_push_buffer: %s", bin.index, buffer_to_string (buffer).c_str ());
          const auto result = gst_app_src_push_buffer (bin.source, buffer);
          g_assert_true (result == GstFlowReturn::GST_FLOW_OK);
        } break;
        case 3: {
          GST_INFO ("%u: gst_app_src_end_of_stream", bin.index);
          gst_app_src_end_of_stream (bin.source);
          bin.end_of_stream = true;
        } break;
        default:
          g_assert_not_reached ();
      }
    }
    GST_INFO ("After pushing data");
    if (g_video_mode != 0) {
      for (auto&& bin : bin_list) {
        if (!bin.end_of_stream) {
          GST_INFO ("%u: gst_app_src_end_of_stream", bin.index);
          gst_app_src_end_of_stream (bin.source);
        }
      }
    }
  }

  static std::string time (GstClockTime value)
  {
    char text[32];
    sprintf (text, "%.3f", value / 1E9);
    return text;
  }
  static std::string sample_text (GstSample* sample)
  {
    std::ostringstream stream;
    if (sample) {
      // const auto caps = gst_sample_get_caps (sample);
      // if (caps) {
      //   const auto caps_string = gst_caps_to_string (caps);
      //   if (caps_string) {
      //     stream << "caps: " << caps_string;
      //     g_free (caps_string);
      //   }
      // }
      const auto buffer = gst_sample_get_buffer (sample);
      if (buffer) {
        stream << "pts " << time (GST_BUFFER_PTS (buffer));
      }
    } else
      stream << "(null)";
    return stream.str ();
  }

  void handle_bus_error_message (GstBus* bus, GstMessage* message)
  {
    g_assert_nonnull (bus);
    g_assert_nonnull (message);
    GError* error = nullptr;
    gchar* debug_message = nullptr;
    gst_message_parse_error (message, &error, &debug_message);
    GST_ERROR ("handle_bus_error_message: %s, %s", GST_MESSAGE_SRC_NAME (message), error->message);
    if (debug_message)
      GST_ERROR ("%s", debug_message);
    g_clear_error (&error);
    g_free (debug_message);
  }
  void handle_bus_eos_message (GstBus* bus, GstMessage* message)
  {
    g_assert_nonnull (bus);
    g_assert_nonnull (message);
    GST_INFO ("handle_bus_eos_message\n");
  }
  void handle_bus_state_changed_message (GstBus* bus, GstMessage* message)
  {
    g_assert_nonnull (bus);
    g_assert_nonnull (message);
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed (message, &old_state, &new_state, &pending_state);
    GST_DEBUG ("handle_bus_state_changed_message: %s, %s to %s, pending %s\n", GST_MESSAGE_SRC_NAME (message), gst_element_state_get_name (new_state), gst_element_state_get_name (old_state), gst_element_state_get_name (pending_state));
  }

  GstPipeline* pipeline = nullptr;
  std::list<Bin> bin_list;
};

inline void add_debug_output_log_function ()
{
#if defined(WIN32) && !defined(NDEBUG)
  const auto log_function = [] (GstDebugCategory* category, GstDebugLevel level, const gchar* file, const gchar* function, gint line, GObject* object, GstDebugMessage* message, gpointer) {
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

// Table of Concepts https://gstreamer.freedesktop.org/documentation/tutorials/table-of-concepts.html#table-of-concepts

#include "app/gstappsink.h"

GST_PLUGIN_STATIC_DECLARE (appsink);

int main (int argc, char* argv[])
{
  GError* error = nullptr;
  GOptionContext* context = g_option_context_new ("- GStreamer appsrc testbed");
  g_option_context_add_main_entries (context, g_option_context_entries, GETTEXT_PACKAGE);
  // g_option_context_add_group (context, gtk_get_option_group (TRUE));
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_print ("Command line parse failed: %s\n", error->message);
    exit (1);
  }

  gst_init (&argc, &argv);

  GST_DEBUG_CATEGORY_INIT (application_category, "application", 0, "Application specific distinct debug category");

#if defined(WIN32) && !defined(NDEBUG)
  add_debug_output_log_function ();
  gst_debug_set_active (true);
  gst_debug_set_default_threshold (GST_LEVEL_INFO);
  // gst_debug_set_threshold_for_name ("...", GST_LEVEL_DEBUG);
#endif

#if defined(WITH_APPSINK)
  const auto registry = gst_registry_get ();
  {
    auto feature = gst_registry_find_feature (registry, "appsink", GST_TYPE_ELEMENT_FACTORY);
    GST_INFO_OBJECT (feature, "feature");
    g_assert_nonnull (feature);
    gst_registry_remove_feature (registry, feature);
    gst_object_unref (std::exchange (feature, nullptr));
    g_assert_null (gst_registry_find_feature (registry, "appsink", GST_TYPE_ELEMENT_FACTORY));
  }
  gst_element_register (nullptr, "appsink", GST_RANK_NONE, GST_TYPE_APP_SINK);
// {
//   const auto sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", nullptr));
//   g_assert_nonnull (sink);
//   GST_INFO_OBJECT (sink, "sink");
//   gst_object_unref (GST_OBJECT (sink));
// }
#endif

  Application application;
  application.pipeline = GST_PIPELINE_CAST (gst_pipeline_new ("pipeline"));
  g_assert_nonnull (application.pipeline);
  GstBus* bus = gst_element_get_bus (GST_ELEMENT_CAST (application.pipeline));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", G_CALLBACK (+[] (GstBus* bus, GstMessage* message, Application* application) -> void { application->handle_bus_error_message (bus, message); }), &application);
  g_signal_connect (G_OBJECT (bus), "message::eos", G_CALLBACK (+[] (GstBus* bus, GstMessage* message, Application* application) -> void { application->handle_bus_eos_message (bus, message); }), &application);
  g_signal_connect (G_OBJECT (bus), "message::state-changed", G_CALLBACK (+[] (GstBus* bus, GstMessage* message, Application* application) -> void { application->handle_bus_state_changed_message (bus, message); }), &application);

  g_assert_true (g_bin_count > 0);
  for (guint index = 0; index < g_bin_count; index++) {
    auto& bin = application.bin_list.emplace_back ();
    bin.application = &application;
    bin.index = index;
    bin.create_playbin ();
    bin.create_sink ();
    gst_bin_add (GST_BIN (application.pipeline), bin.playbin);
    gst_element_sync_state_with_parent (bin.playbin);
  }

  std::string path = g_path ? g_path : "../data/appsrc";
  std::replace (path.begin (), path.end (), '/', static_cast<char> (std:://experimental::
    filesystem::path::preferred_separator));
  GST_DEBUG ("path %s", path.c_str ());
  std::atomic_bool push_thread_termination = false;
  std::thread push_thread ([&] { application.push (push_thread_termination, path); });

  set_pipeline_state (application.pipeline, GST_STATE_PLAYING);
  auto message = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, static_cast<GstMessageType> (GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  // NOTE: https://gstreamer.freedesktop.org/documentation/tutorials/basic/debugging-tools.html#getting-pipeline-graphs
  //       https://dreampuf.github.io/GraphvizOnline
  GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (application.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox");
  g_assert_true (message != nullptr);
  g_assert_true (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS);
  gst_message_unref (std::exchange (message, nullptr));

  push_thread_termination.store (true);
  for (auto&& bin : application.bin_list)
    bin.source_data_condition.notify_all ();
  push_thread.join ();

  gst_bus_remove_signal_watch (bus);
  gst_object_unref (std::exchange (bus, nullptr));

  gst_element_set_state (GST_ELEMENT_CAST (application.pipeline), GST_STATE_NULL);

  return 0;
}

/*

GST_DEBUG_DUMP_DOT_DIR=~ GST_DEBUG=*:2,application:4 ./sandbox
GST_DEBUG_DUMP_DOT_DIR=~/gstreamer_appsrcsandbox/build GST_DEBUG=*:2,application:4 ./sandbox
GST_DEBUG=*:2,application:4 ./sandbox 2>sandbox-1.log
GST_DEBUG=*:6 ./sandbox 2>sandbox-2.log
GST_DEBUG=*:4 build/sandbox -p ~/rpi/build_reference/VideoPlayerTester/AppSrc-Video -s 2
GST_DEBUG=*:2,application:4 build/sandbox -p ~/rpi/build_reference/VideoPlayerTester/AppSrc -s 1 --bin-count 2 --video-bin-index 0

roman@raspberrypi:~/gstreamer_appsrcsandbox/build $ mv ~/cross/VideoPlayerTester/AppSrc-Video ../data
roman@raspberrypi:~/gstreamer_appsrcsandbox/build $ ls -l ../data

- fakesink?

*/
