#pragma once

#include <vector>
#include <sstream>
#include <fstream>

struct AppsrcFile {
  void open ()
  {
    stream.open ("appsrc", std::ios_base::binary | std::ios_base::out);
  }
  void close ()
  {
    stream.close ();
  }
  void write (const void* data, size_t data_size)
  {
    stream.write (reinterpret_cast<const char*> (data), data_size);
  }
  template<typename ValueType>
  void write (const ValueType& value)
  {
    write (&value, sizeof value);
  }
  template<typename ValueType>
  void write_as (ValueType value)
  {
    write (&value, sizeof value);
  }

  void handle_caps (GstCaps* caps, uint8_t element_identifier = 0)
  {
    g_assert_nonnull (caps);
    static uint8_t constexpr const g_identifier = 1;
    write (g_identifier);
    write (element_identifier);
    std::string caps_string;
    if (caps) {
      gchar* string = gst_caps_to_string (caps);
      if (string) {
        caps_string = string;
        g_free (string);
      }
    }
    const auto size = static_cast<uint16_t> (caps_string.size ());
    write (size);
    write (caps_string.data (), caps_string.size ());
  }
  void handle_buffer (GstBuffer* buffer, uint8_t element_identifier = 0)
  {
    g_assert_nonnull (buffer);
    static uint8_t constexpr const g_identifier = 2;
    write (g_identifier);
    write (element_identifier);
    {
      write_as (static_cast<uint64_t> GST_BUFFER_FLAGS (buffer));
      write_as (static_cast<int64_t> GST_BUFFER_DTS (buffer));
      write_as (static_cast<int64_t> GST_BUFFER_PTS (buffer));
      write_as (static_cast<int64_t> GST_BUFFER_DURATION (buffer));
    }
    std::vector<uint8_t> data;
    data.resize (gst_buffer_get_size (buffer));
    gst_buffer_extract (buffer, 0, data.data (), static_cast<gsize> (data.size ()));
    const auto data_size = static_cast<uint32_t> (data.size ());
    write (data_size);
    write (data.data (), data.size ());
  }
  void handle_end_of_stream (uint8_t element_identifier = 0)
  {
    static uint8_t constexpr const g_identifier = 3;
    write (g_identifier);
    write (element_identifier);
  }

  std::ofstream stream;
};
