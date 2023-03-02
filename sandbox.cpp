#if defined(__GNUC__)
#  include <features.h>
#  if !__GNUC_PREREQ(8, 0) // https://stackoverflow.com/a/259279/868014
#    define STD_FILESYSTEM_EXPERIMENTAL
#  endif
#endif

#include <memory>
#include <algorithm>
#include <vector>
#include <list>
#include <string>
#include <sstream>
#if defined(STD_FILESYSTEM_EXPERIMENTAL)
#  include <experimental/filesystem>
using std::experimental::filesystem::path;
#else
#  include <filesystem>
using namespace std::filesystem::path;
#endif
#include <fstream>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

// NOTE: This is not really needed for replay, just a reference to the header which creates appsrc replay files
#include "record.h"

#if defined(WIN32) && !defined(NDEBUG)
#  include <windows.h>
#endif

#define GETTEXT_PACKAGE "gstreamer_appsrcsandbox"

// Commandline option parser https://developer-old.gnome.org/glib/unstable/glib-Commandline-option-parser.html

static gchar* g_path = nullptr;
static gint g_video_mode = 0;
static guint g_stream_count = 1;
static guint g_video_index = 0;
static gboolean g_no_sync = false;
static guint g_only_push_index = std::numeric_limits<guint>::max ();
static gboolean g_playsink_mode = false;

static GOptionEntry g_option_context_entries[] {
  { "path", 'p', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_STRING, &g_path, "Path to input file to play back", nullptr },
  { "video-mode", 'v', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_video_mode, "Playbin video-sink mode (0 - default sink, 1 - I420 appsink, 2 - I420 capsfilter & appsink)", nullptr },
  { "stream-count", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_stream_count, "Number of bins in the pipeline and presumably in the supplied replay input", nullptr },
  { "video-index", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_video_index, "Index of video stream in the input file (and respectively in multi-stream or joint stream configuration)", nullptr },
  { "no-sync", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &g_no_sync, "Remove sync mode from appsink instances", nullptr },
  { "only-push-index", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &g_only_push_index, "Replay buffers only on specified stream index", nullptr },
  { "playsink-mode", 's', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &g_playsink_mode, "Use single playsink instead of playbins for video/audio input", nullptr },
  { nullptr }
};

GST_DEBUG_CATEGORY_STATIC (application_category); // https://gstreamer.freedesktop.org/documentation/tutorials/basic/debugging-tools.html#adding-your-own-debug-information
#define GST_CAT_DEFAULT application_category

void set_pipeline_state (GstPipeline* pipeline, GstState state)
{
  g_assert_nonnull (pipeline);
  const auto result = gst_element_set_state (GST_ELEMENT_CAST (pipeline), state);
  g_assert_true (result >= GST_STATE_CHANGE_SUCCESS);
  if (result == GST_STATE_CHANGE_ASYNC) {
    const auto async_result = gst_element_get_state (GST_ELEMENT_CAST (pipeline), nullptr, nullptr, GST_CLOCK_TIME_NONE);
    g_assert_true (async_result != GST_STATE_CHANGE_FAILURE);
  }
}

struct Application {
  struct SourceData {
    GstAppSrc* source = nullptr;
    std::atomic_uint caps_version = 0;
    std::atomic_bool hold = false;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_bool need = false;
    bool end_of_stream = false;
  };

  struct SingleStream {
    void create ()
    {
      playbin = gst_element_factory_make ("playbin", nullptr); // https://gstreamer.freedesktop.org/documentation/playback/playbin.html#properties
      g_assert_nonnull (playbin);
      g_object_set (playbin,
          "uri", "appsrc://",
          "flags", 3, // static_cast<GstPlayFlags>(GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO), // https://gstreamer.freedesktop.org/documentation/playback/playsink.html#GstPlayFlags
          nullptr);
      g_signal_connect (playbin, "source-setup", G_CALLBACK (+[] (GstElement* pipeline, GstElement* element, SingleStream* stream) { stream->handle_source_setup (element); }), this);
      g_signal_connect (playbin, "element-setup", G_CALLBACK (+[] (GstElement* pipeline, GstElement* element, SingleStream* stream) { stream->handle_element_setup (element); }), this);

      const auto connect_sink_signals = [&] {
        g_object_set (G_OBJECT (sink), "emit-signals", TRUE, nullptr);
        g_signal_connect (sink, "new-preroll", G_CALLBACK (+[] (GstAppSink* sink, SingleStream* stream) -> GstFlowReturn { return stream->handle_sink_preroll_sample (sink); }), this);
        g_signal_connect (sink, "new-sample", G_CALLBACK (+[] (GstAppSink* sink, SingleStream* stream) -> GstFlowReturn { return stream->handle_sink_sample (sink); }), this);
        g_signal_connect (sink, "eos", G_CALLBACK (+[] (GstAppSink* sink, SingleStream* stream) { stream->handle_sink_eos (sink); }), this);
      };
      const auto set_sink_callbacks = [&] {
        g_object_set (G_OBJECT (sink), "emit-signals", FALSE, nullptr);
        GstAppSinkCallbacks callbacks;
        callbacks.eos = ([] (GstAppSink* sink, gpointer stream) { reinterpret_cast<SingleStream*> (stream)->handle_sink_eos (sink); });
        callbacks.new_preroll = ([] (GstAppSink* sink, gpointer stream) -> GstFlowReturn { return reinterpret_cast<SingleStream*> (stream)->handle_sink_preroll_sample (sink); });
        callbacks.new_sample = ([] (GstAppSink* sink, gpointer stream) -> GstFlowReturn { return reinterpret_cast<SingleStream*> (stream)->handle_sink_sample (sink); });
        gst_app_sink_set_callbacks (sink, &callbacks, this, nullptr);
      };

      // NOTE: 0 - default sink, visual rendering
      //       1 - appsink, restricted to I420
      //       2 - bin with capsfilter and appsink, restricted to I420
      switch (g_video_mode) {
        case 0:
          break;
        case 1: {
          if (index == g_video_index) {
            sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", "video_sink"));
            GstCaps* caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
            g_object_set (G_OBJECT (sink),
                "sync", !g_no_sync,
                "caps", caps,
                "max-buffers", static_cast<guint> (12),
                nullptr);
            set_sink_callbacks (); // connect_sink_signals ();
            g_object_set (G_OBJECT (playbin), "video-sink", GST_ELEMENT_CAST (sink), nullptr);
          } else {
            sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", "audio_sink"));
            g_object_set (G_OBJECT (sink),
                "sync", !g_no_sync,
                "max-buffers", static_cast<guint> (12),
                nullptr);
            set_sink_callbacks (); // connect_sink_signals ();
            g_object_set (G_OBJECT (playbin), "audio-sink", GST_ELEMENT_CAST (sink), nullptr);
          }
        } break;
        case 2: {
          g_assert_true (g_stream_count == 1 && g_video_index == 0);
          auto capsfilter = gst_element_factory_make ("capsfilter", "video_caps"); // https://gstreamer.freedesktop.org/documentation/coreelements/capsfilter.html#capsfilter-page
          GstCaps* caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
          g_object_set (G_OBJECT (capsfilter),
              "caps", caps,
              nullptr);
          sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", "video_sink"));
          g_object_set (G_OBJECT (sink),
              "sync", !g_no_sync,
              "max-buffers", static_cast<guint> (12),
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

      gst_bin_add (GST_BIN (application->pipeline), playbin);
      gst_element_sync_state_with_parent (playbin);
    }

    void handle_source_setup (GstElement* element)
    {
      GST_INFO_OBJECT (element, "%u: handle_source_setup, %s", index, GST_ELEMENT_NAME (element));
      source_data.source = GST_APP_SRC (element);
      gst_object_ref (GST_OBJECT_CAST (source_data.source));
      g_object_set (source_data.source, // https://gstreamer.freedesktop.org/documentation/app/appsrc.html?gi-language=c
          "max-bytes", static_cast<guint64> (2 << 20),
          "min-percent", static_cast<guint> (50),
          nullptr);
      g_object_set (source_data.source, "format", GST_FORMAT_TIME, nullptr);
      g_signal_connect (source_data.source, "enough-data", G_CALLBACK (+[] (GstElement* element, SingleStream* stream) { stream->handle_enough_data (); }), this);
      g_signal_connect (source_data.source, "need-data", G_CALLBACK (+[] (GstElement* element, guint data_size, SingleStream* stream) { stream->handle_need_data (); }), this);
      source_data.need.store (true);
    }
    void handle_element_setup (GstElement* element)
    {
      GST_INFO_OBJECT (element, "%u: handle_element_setup, %s", index, GST_ELEMENT_NAME (element));
    }

    void handle_enough_data ()
    {
      GST_WARNING ("%u: handle_enough_data", index);
      // GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox-handle_enough_data");
      std::unique_lock source_data_lock (source_data.mutex);
      source_data.need.store (false);
      source_data.condition.notify_all ();
    }
    void handle_need_data ()
    {
      GST_WARNING ("%u: handle_need_data", index);
      // GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox-handle_need_data");
      std::unique_lock source_data_lock (source_data.mutex);
      source_data.need.store (true);
      source_data.condition.notify_all ();
    }

    GstFlowReturn handle_sink_preroll_sample (GstAppSink* sink)
    {
      GST_DEBUG_OBJECT (sink, "%u: handle_sink_preroll_sample", index);
      g_assert_nonnull (sink);
      for (;;) {
        const auto sample = gst_app_sink_try_pull_preroll (sink, 0);
        if (!sample)
          break;
        GST_INFO_OBJECT (sample, "%u: handle_sink_preroll_sample: %s", index, sample_text (sample).c_str ());
        gst_sample_unref (sample);
      }
      return GstFlowReturn::GST_FLOW_OK;
    }
    GstFlowReturn handle_sink_sample (GstAppSink* sink)
    {
      GST_DEBUG_OBJECT (sink, "%u: handle_sink_sample", index);
      g_assert_nonnull (sink);
      for (;;) {
        const auto sample = gst_app_sink_try_pull_sample (sink, 0);
        if (!sample)
          break;
        GST_INFO_OBJECT (sample, "%u: handle_sink_sample: %s", index, sample_text (sample).c_str ());
        gst_sample_unref (sample);
      }
      return GstFlowReturn::GST_FLOW_OK;
    }
    void handle_sink_eos (GstAppSink* sink)
    {
      GST_INFO_OBJECT (sink, "%u: handle_sink_eos", index);
    }

    Application* application;
    guint index;
    GstElement* playbin = nullptr;
    SourceData source_data;
    GstAppSink* sink = nullptr;
  };

  struct JointStream {
    struct Stream {
      void create_source ()
      {
        std::ostringstream name;
        name << this->name << "_source";
        source_data.source = GST_APP_SRC (gst_element_factory_make ("appsrc", name.str ().c_str ()));
        g_object_set (source_data.source, // https://gstreamer.freedesktop.org/documentation/app/appsrc.html?gi-language=c
            "max-bytes", static_cast<guint64> (2 << 20),
            "min-percent", static_cast<guint> (50),
            nullptr);
        g_object_set (source_data.source, "format", GST_FORMAT_TIME, nullptr);
        g_signal_connect (source_data.source, "enough-data", G_CALLBACK (+[] (GstElement* element, Stream* stream) { stream->handle_enough_data (); }), this);
        g_signal_connect (source_data.source, "need-data", G_CALLBACK (+[] (GstElement* element, guint data_size, Stream* stream) { stream->handle_need_data (); }), this);
      }
      void create_sink ()
      {
        std::ostringstream name;
        name << this->name << "_sink";
        sink = GST_APP_SINK_CAST (gst_element_factory_make ("appsink", name.str ().c_str ()));
        g_object_set (G_OBJECT (sink),
            "sync", !g_no_sync,
            "max-buffers", static_cast<guint> (12),
            nullptr);
        const auto connect_sink_signals = [&] {
          g_object_set (G_OBJECT (sink), "emit-signals", TRUE, nullptr);
          g_signal_connect (sink, "new-preroll", G_CALLBACK (+[] (GstAppSink* sink, Stream* stream) -> GstFlowReturn { return stream->handle_sink_preroll_sample (sink); }), this);
          g_signal_connect (sink, "new-sample", G_CALLBACK (+[] (GstAppSink* sink, Stream* stream) -> GstFlowReturn { return stream->handle_sink_sample (sink); }), this);
          g_signal_connect (sink, "eos", G_CALLBACK (+[] (GstAppSink* sink, Stream* stream) { stream->handle_sink_eos (sink); }), this);
        };
        const auto set_sink_callbacks = [&] {
          g_object_set (G_OBJECT (sink), "emit-signals", FALSE, nullptr);
          GstAppSinkCallbacks callbacks;
          callbacks.eos = ([] (GstAppSink* sink, gpointer stream) { reinterpret_cast<Stream*> (stream)->handle_sink_eos (sink); });
          callbacks.new_preroll = ([] (GstAppSink* sink, gpointer stream) -> GstFlowReturn { return reinterpret_cast<Stream*> (stream)->handle_sink_preroll_sample (sink); });
          callbacks.new_sample = ([] (GstAppSink* sink, gpointer stream) -> GstFlowReturn { return reinterpret_cast<Stream*> (stream)->handle_sink_sample (sink); });
          gst_app_sink_set_callbacks (sink, &callbacks, this, nullptr);
        };
        set_sink_callbacks (); // connect_sink_signals ();
      }

      void create_decodebin (const char* playsink_pad_name)
      {
        g_assert_nonnull (playsink_pad_name);
        GST_DEBUG ("name %s, playsink_pad_name %s", name.c_str (), playsink_pad_name);
        g_assert_null (decodebin);
        g_assert_false (decodebin_linked);
        GstPad* source_pad = gst_element_get_static_pad (GST_ELEMENT_CAST (source_data.source), "src");
        decodebin = gst_element_factory_make ("decodebin3", nullptr);
        g_assert_nonnull (decodebin);
        g_object_set (GST_OBJECT (decodebin),
            "flags", 3, // static_cast<GstPlayFlags>(GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO), // https://gstreamer.freedesktop.org/documentation/playback/playsink.html#GstPlayFlags
            nullptr);
        g_signal_connect (decodebin, "pad-added", G_CALLBACK (+[] (GstElement* element, GstPad* pad, gpointer parameter) { reinterpret_cast<Stream*> (parameter)->handle_decodebin_pad_added (pad); }), this);
        this->playsink_pad_name = playsink_pad_name;
        gst_bin_add (GST_BIN (foo->application->pipeline), decodebin);
        gst_element_sync_state_with_parent (decodebin);
        auto sink_pad = gst_element_get_static_pad (decodebin, "sink");
        const auto pad_link_result = gst_pad_link (source_pad, sink_pad);
        g_assert_true (pad_link_result == GST_PAD_LINK_OK);
        gst_object_unref (GST_OBJECT (sink_pad));
        gst_object_unref (GST_OBJECT (std::exchange (source_pad, nullptr)));
      }

      void handle_decodebin_pad_added (GstPad* pad)
      {
        g_assert_nonnull (pad);
        GST_DEBUG ("name %s, playsink_pad_name %s", name.c_str (), playsink_pad_name.c_str ());
        g_assert_nonnull (decodebin);
        g_assert_false (gst_pad_is_linked (pad));
        GstPadTemplate* pad_template = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (foo->playsink), playsink_pad_name.c_str ());
        GstPad* sink_pad = gst_element_request_pad (foo->playsink, pad_template, nullptr, nullptr);
        g_assert_false (gst_pad_is_linked (sink_pad));
        const auto pad_link_result = gst_pad_link (pad, sink_pad);
        g_assert_true (pad_link_result == GST_PAD_LINK_OK);
        gst_object_unref (sink_pad);
#if !defined(NDEBUG)
        char name[64];
        sprintf (name, "gstreamer_appsrcsandbox-handle_decodebin_pad_added-%s", this->name.c_str ());
        GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (foo->application->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, name);
#endif
        decodebin_linked.store (true);
      }

      void handle_enough_data ()
      {
        GST_WARNING ("%s: handle_enough_data", name.c_str ());
#if !defined(NDEBUG)
        static std::atomic_uint g_counter;
        char name[64];
        sprintf (name, "gstreamer_appsrcsandbox-handle_enough_data-%03u", g_counter++);
        GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (foo->application->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, name);
#endif
        std::unique_lock source_data_lock (source_data.mutex);
        source_data.need.store (false);
        source_data.condition.notify_all ();
      }
      void handle_need_data ()
      {
        GST_WARNING ("%s: handle_need_data", name.c_str ());
#if !defined(NDEBUG)
        static std::atomic_uint g_counter;
        char name[64];
        sprintf (name, "gstreamer_appsrcsandbox-handle_need_data-%03u", g_counter++);
        GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (foo->application->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, name);
#endif
        std::unique_lock source_data_lock (source_data.mutex);
        source_data.need.store (true);
        source_data.condition.notify_all ();
      }

      GstFlowReturn handle_sink_preroll_sample (GstAppSink* sink)
      {
        GST_DEBUG_OBJECT (sink, "%s: handle_sink_preroll_sample", name.c_str ());
        g_assert_nonnull (sink);
        for (;;) {
          const auto sample = gst_app_sink_try_pull_preroll (sink, 0);
          if (!sample)
            break;
          GST_INFO_OBJECT (sample, "%s: handle_sink_preroll_sample: %s", name.c_str (), sample_text (sample).c_str ());
          gst_sample_unref (sample);
        }
        return GstFlowReturn::GST_FLOW_OK;
      }
      GstFlowReturn handle_sink_sample (GstAppSink* sink)
      {
        GST_DEBUG_OBJECT (sink, "%s: handle_sink_sample", name.c_str ());
        g_assert_nonnull (sink);
        for (;;) {
          const auto sample = gst_app_sink_try_pull_sample (sink, 0);
          if (!sample)
            break;
          GST_INFO_OBJECT (sample, "%s: handle_sink_sample: %s", name.c_str (), sample_text (sample).c_str ());
          gst_sample_unref (sample);
        }
        return GstFlowReturn::GST_FLOW_OK;
      }
      void handle_sink_eos (GstAppSink* sink)
      {
        GST_INFO_OBJECT (sink, "%s: handle_sink_eos", name.c_str ());
      }

      JointStream* foo;
      std::string name;
      SourceData source_data;
      GstElement* decodebin = nullptr;
      std::atomic_bool decodebin_linked = false;
      std::string playsink_pad_name;
      GstAppSink* sink = nullptr;
    };

    void create ()
    {
      g_assert_null (playsink);
      playsink = gst_element_factory_make ("playsink", nullptr);
      g_assert_nonnull (playsink);
      gst_bin_add (GST_BIN (application->pipeline), playsink);

      g_assert (g_video_index >= 0 && g_video_index < 2);

      video.foo = this;
      video.name = "video";
      audio.foo = this;
      audio.name = "audio";

      // NOTE: 0 - default sink, visual rendering
      //       1 - appsinks, video restricted to I420
      switch (g_video_mode) {
        case 0:
          break;
        case 1: {
          video.create_sink ();
          GstCaps* caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
          g_object_set (G_OBJECT (video.sink), "caps", caps, nullptr);
          g_object_set (G_OBJECT (playsink), "video-sink", GST_ELEMENT_CAST (video.sink), nullptr);
          audio.create_sink ();
          g_object_set (G_OBJECT (playsink), "audio-sink", GST_ELEMENT_CAST (audio.sink), nullptr);
        } break;
      }

      video.create_source ();
      audio.create_source ();

      g_assert_true (gst_bin_add (GST_BIN (application->pipeline), GST_ELEMENT_CAST (video.source_data.source)));
      g_assert_true (gst_bin_add (GST_BIN (application->pipeline), GST_ELEMENT_CAST (audio.source_data.source)));

      g_assert_true (gst_element_sync_state_with_parent (playsink));
      g_assert_true (gst_element_sync_state_with_parent (GST_ELEMENT_CAST (video.source_data.source)));
      g_assert_true (gst_element_sync_state_with_parent (GST_ELEMENT_CAST (audio.source_data.source)));
    }
    void link_source_pads ()
    {
      g_assert_true (playsink && video.source_data.source && audio.source_data.source);
      // SUGG: Use single decodebin for both video and audio?
      video.create_decodebin ("video_raw_sink");
      audio.create_decodebin ("audio_raw_sink");
    }

    Application* application;
    GstElement* playsink = nullptr;
    Stream video;
    Stream audio;
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
    if (!stream_list.empty ()) {
      for (; !termination.load ();) {
        if (std::all_of (stream_list.cbegin (), stream_list.cend (), [&] (auto&& bin) { return bin.source_data.source != nullptr; }))
          break;
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
      }
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

      SourceData* source_data;
      std::string name;
      if (!stream_list.empty ()) {
        if (index >= stream_list.size ()) {
          std::call_once (stream_warnning, [&] {
            GST_ERROR ("Trying to play packet for stream %u in %zu-bin configuration, use --bin-count", index, stream_list.size ());
          });
          continue;
        }
        auto iterator = stream_list.begin ();
        std::advance (iterator, index);
        auto& stream = *iterator;
        source_data = &stream.source_data;
        std::ostringstream name_stream;
        name_stream << stream.index;
        name = name_stream.str ();
      } else {
        auto* stream = (index == g_video_index) ? &joint_stream.video : &joint_stream.audio;
        source_data = &stream->source_data;
        name = stream->name;
      }
      g_assert_nonnull (source_data);
      g_assert_nonnull (source_data->source);

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
          GST_INFO ("%s: gst_app_src_set_caps: %s", name.c_str (), caps_string.c_str ());
#if 0
          {
            gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING, "byte-stream", nullptr);
            gchar* caps_string = gst_caps_to_string (caps);
            GST_INFO ("%u: gst_app_src_set_caps: %s", index, caps_string);
            g_free (caps_string);
          }
#endif
          gst_app_src_set_caps (source_data->source, caps);
          gst_caps_unref (caps);
          source_data->caps_version++;
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
          if (g_only_push_index == std::numeric_limits<guint>::max () || g_only_push_index == index) {
            GstBuffer* buffer = gst_buffer_new_allocate (nullptr, size, nullptr);
            GST_BUFFER_FLAGS (buffer) = static_cast<guint> (flags);
            GST_BUFFER_DTS (buffer) = static_cast<GstClockTime> (dts);
            GST_BUFFER_PTS (buffer) = static_cast<GstClockTime> (pts);
            GST_BUFFER_DURATION (buffer) = static_cast<GstClockTime> (duration);
            gst_buffer_fill (buffer, 0, data.data (), data.size ());
            {
              std::unique_lock source_data_lock (source_data->mutex);
              source_data->condition.wait (source_data_lock, [&] { return (!source_data->hold.load () && source_data->need.load ()) || termination.load (); });
            }
            if (termination.load ())
              return;
            GST_INFO ("%s: gst_app_src_push_buffer: %s", name.c_str (), buffer_to_string (buffer).c_str ());
            const auto result = gst_app_src_push_buffer (source_data->source, buffer);
            g_assert_true (result == GstFlowReturn::GST_FLOW_OK);
          }
        } break;
        case 3: {
          GST_INFO ("%s: gst_app_src_end_of_stream", name.c_str ());
          gst_app_src_end_of_stream (source_data->source);
          source_data->end_of_stream = true;
        } break;
        default:
          g_assert_not_reached ();
      }
    }
    GST_INFO ("After pushing data");
    if (g_video_mode != 0) {
      if (!stream_list.empty ()) {
        for (auto&& stream : stream_list) {
          if (!stream.source_data.end_of_stream) {
            GST_INFO ("%u: gst_app_src_end_of_stream", stream.index);
            gst_app_src_end_of_stream (stream.source_data.source);
          }
        }
      } else {
        for (auto* stream : { &joint_stream.video, &joint_stream.audio })
          if (!stream->source_data.end_of_stream) {
            GST_INFO ("%s: gst_app_src_end_of_stream", stream->name.c_str ());
            gst_app_src_end_of_stream (stream->source_data.source);
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
  std::list<SingleStream> stream_list;
  JointStream joint_stream;
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

  if (g_playsink_mode) {
    application.joint_stream.application = &application;
    application.joint_stream.create ();
    application.joint_stream.video.source_data.hold.store (true);
    application.joint_stream.audio.source_data.hold.store (true);
  } else {
    g_assert_true (g_stream_count > 0);
    for (guint index = 0; index < g_stream_count; index++) {
      auto& stream = application.stream_list.emplace_back ();
      stream.application = &application;
      stream.index = index;
      stream.create ();
    }
  }

  std::string path = g_path ? g_path : "../data/appsrc";
  std::replace (path.begin (), path.end (), '/', static_cast<char> (path::preferred_separator));
  GST_DEBUG ("path %s", path.c_str ());
  std::atomic_bool push_thread_termination = false;
  std::thread push_thread ([&] { application.push (push_thread_termination, path); });

  if (g_playsink_mode) {
    // TODO: If source file does not have caps as first items this might lock dead
    // TODO: Also check for EOS for accuracy
    GST_INFO ("Waiting for appsrc caps...");
    for (unsigned int iteration = 0;; iteration++) {
      if (application.joint_stream.video.source_data.caps_version.load () >= 1 || application.joint_stream.audio.source_data.caps_version.load () >= 1)
        break;
      std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    application.joint_stream.link_source_pads ();
    GST_INFO ("Waiting for playsink source links...");
    application.joint_stream.video.source_data.hold.store (false);
    application.joint_stream.video.source_data.need.store (true);
    application.joint_stream.video.source_data.condition.notify_all ();
    application.joint_stream.audio.source_data.hold.store (false);
    application.joint_stream.audio.source_data.need.store (true);
    application.joint_stream.audio.source_data.condition.notify_all ();
    set_pipeline_state (application.pipeline, GST_STATE_PAUSED);
    for (unsigned int iteration = 0;; iteration++) {
      if (application.joint_stream.video.decodebin_linked.load () && application.joint_stream.audio.decodebin_linked.load ())
        break;
      std::this_thread::sleep_for (std::chrono::milliseconds (100));
      if (iteration == 25)
        GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (application.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox");
    }
  }

  GST_INFO ("Playing...");
  set_pipeline_state (application.pipeline, GST_STATE_PLAYING);
  auto message = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, static_cast<GstMessageType> (GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  // NOTE: https://gstreamer.freedesktop.org/documentation/tutorials/basic/debugging-tools.html#getting-pipeline-graphs
  //       https://dreampuf.github.io/GraphvizOnline
  GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN_CAST (application.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "gstreamer_appsrcsandbox");
  g_assert_true (message != nullptr);
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
    GError* error = nullptr;
    gchar* debug_message = nullptr;
    gst_message_parse_error (message, &error, &debug_message);
    GST_ERROR ("%s, %s", GST_MESSAGE_SRC_NAME (message), error->message);
    if (debug_message)
      GST_ERROR ("%s", debug_message);
    g_clear_error (&error);
    g_free (debug_message);
  }
  g_assert_true (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS);
  gst_message_unref (std::exchange (message, nullptr));

  push_thread_termination.store (true);
  for (auto&& stream : application.stream_list)
    stream.source_data.condition.notify_all ();
  for (auto* stream : { &application.joint_stream.video, &application.joint_stream.audio })
    stream->source_data.condition.notify_all ();
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
GST_DEBUG=*:4 build/sandbox -p ~/rpi/build_reference/VideoPlayerTester/AppSrc-Video -v 2
GST_DEBUG=*:2,application:4 build/sandbox -p ~/rpi/build_reference/VideoPlayerTester/AppSrc -v 1 --bin-count 2 --video-bin-index 1

- fakesink?

*/
