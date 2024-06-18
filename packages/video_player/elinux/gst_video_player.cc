// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_video_player.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>

GstVideoPlayer::GstVideoPlayer(
    const std::string& uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler)) {
  gst_.pipeline = nullptr;
  gst_.video_src = nullptr;
  gst_.video_convert = nullptr;
  gst_.video_sink = nullptr;
  gst_.output = nullptr;
  gst_.bus = nullptr;
  gst_.buffer = nullptr;
  gst_.fpssink = nullptr;

  if (!regex_match(uri, GstVideoPlayer::camera_path_regex_)) {
    uri_ = ParseUri(uri);
    is_stream_ = IsStreamUri(uri_);

  } else {
    // camera handling
    uri_ = uri;
    is_camera_ = true;
    width_ = 1920;
    height_ = 1080;
  }

  if (!CreatePipeline()) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    DestroyPipeline();
    return;
  }

  // Prerolls before getting information from the pipeline.
  Preroll();

  // Sets internal video size and buffier.
  GetVideoSize(width_, height_);

  stream_handler_->OnNotifyInitialized();

  // Sometimes live streams doesn't contain aspect ratio
  // which leads to issue with playback picture
  // CorrectAspectRatio();
}

GstVideoPlayer::~GstVideoPlayer() {
  is_destroyed_ = true;
  //   Stop();
  DestroyPipeline();
}

bool GstVideoPlayer::IsStreamUri(const std::string& uri) const {
  return regex_match(uri, GstVideoPlayer::stream_type_regex_) ||
         regex_match(uri, GstVideoPlayer::stream_ext_regex_);
}

bool GstVideoPlayer::CheckPluginAvailability(const std::string& element) {
  return gst_element_factory_find(element.c_str()) ? true : false;
}

// Code to increase Gst plugin rank, should be used to force using particular
// plugin
void GstVideoPlayer::IncreasePluginRank(const std::string& element) {
  GstRegistry* registry = NULL;
  GstElementFactory* factory = NULL;

  registry = gst_registry_get();
  if (!registry) return;

  factory = gst_element_factory_find(element.c_str());
  if (!factory) printf("%s", "factory fail");

  gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory),
                              GST_RANK_PRIMARY + 100);

  gst_registry_add_feature(registry, GST_PLUGIN_FEATURE(factory));
}

// static
void GstVideoPlayer::GstLibraryLoad() { gst_init(NULL, NULL); }

void GstVideoPlayer::ToggleFpsTextDisplay() {
  if (!gst_.fpssink) {
    return;
  }

  gboolean text_overlay;
  g_object_get(G_OBJECT(gst_.fpssink), "text-overlay", &text_overlay, NULL);

  g_object_set(G_OBJECT(gst_.fpssink), "text-overlay", !text_overlay, NULL);
}

// static
void GstVideoPlayer::GstLibraryUnload() { gst_deinit(); }

bool GstVideoPlayer::Play() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PLAYING" << std::endl;
    return false;
  }
  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(gst_.pipeline), GST_DEBUG_GRAPH_SHOW_ALL,
                            "pipeline");

  if (gst_.fpssink) {
    std::cout << "Turning off text overlay" << std::endl;
    g_object_set(G_OBJECT(gst_.fpssink), "text-overlay", FALSE, NULL);
  }
  return true;
}

bool GstVideoPlayer::Pause() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }
  return true;
}

bool GstVideoPlayer::Stop() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_READY) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to READY" << std::endl;
    return false;
  }
  return true;
}

bool GstVideoPlayer::SetVolume(double volume) {
  if (!gst_.video_src) {
    return false;
  }

  volume_ = volume;
  g_object_set(gst_.video_src, "volume", volume, NULL);
  return true;
}

void GstVideoPlayer::GetVideoSize(int32_t& width, int32_t& height) {
  if (!gst_.pipeline || !gst_.video_sink) {
    std::cerr
        << "Failed to get video size. The pileline hasn't initialized yet."
        << std::endl;
    return;
  }

  auto* sink_pad = gst_element_get_static_pad(gst_.video_sink, "sink");
  if (!sink_pad) {
    std::cerr << "Failed to get a pad" << std::endl;
    return;
  }

  auto* caps = gst_pad_get_current_caps(sink_pad);
  if (!caps) {
    // Set a callback when the sink_pad caps get set
    g_signal_connect(sink_pad, "notify::caps", G_CALLBACK(OnCapsChanged), this);

    gst_object_unref(sink_pad);
  }
}

int32_t GstVideoPlayer::GetWidth() {
  if (is_destroyed_ || !initialized_) {
    return 0;
  }
  return width_;
}

int32_t GstVideoPlayer::GetHeight() {
  if (is_destroyed_ || !initialized_) {
    return 0;
  }
  return height_;
}

bool GstVideoPlayer::SetPlaybackRate(double rate) {
  if (is_stream_ || is_camera_) return false;

  if (!gst_.video_src) {
    return false;
  }

  if (rate <= 0) {
    std::cerr << "Rate " << rate << " is not supported" << std::endl;
    return false;
  }

  auto position = GetCurrentPosition();
  if (position < 0) {
    return false;
  }

  if (!gst_element_seek(gst_.pipeline, rate, GST_FORMAT_TIME,
                        GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                        position * GST_MSECOND, GST_SEEK_TYPE_SET,
                        GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to set playback rate to " << rate
              << " (gst_element_seek failed)" << std::endl;
    return false;
  }

  playback_rate_ = rate;
  mute_ = (rate < 0.5 || rate > 2);
  g_object_set(gst_.video_src, "mute", mute_, NULL);

  return true;
}

bool GstVideoPlayer::SetSeek(int64_t position) {
  if (is_stream_ || is_camera_) return false;

  auto nanosecond = position * 1000 * 1000;
  if (!gst_element_seek(
          gst_.pipeline, playback_rate_, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET, nanosecond, GST_SEEK_TYPE_SET,
          GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to seek " << nanosecond << std::endl;
    return false;
  }
  return true;
}

int64_t GstVideoPlayer::GetDuration() {
  if (is_stream_ || is_camera_) return 0;

  GstFormat fmt = GST_FORMAT_TIME;
  int64_t duration_msec;
  if (!gst_element_query_duration(gst_.pipeline, fmt, &duration_msec)) {
    std::cerr << "Failed to get duration" << std::endl;
    return -1;
  }
  duration_msec /= GST_MSECOND;
  return duration_msec;
}

int64_t GstVideoPlayer::GetCurrentPosition() {
  gint64 position = 0;

  if (is_stream_ || is_camera_) return position;

  // Sometimes we get an error when playing streaming videos.
  if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &position)) {
    std::cerr << "Failed to get current position" << std::endl;
    return -1;
  }

  // TODO: We need to handle this code in the proper plase.
  // The VideoPlayer plugin doesn't have a main loop, so EOS message
  // received from GStreamer cannot be processed directly in a callback
  // function. This is because the event channel message of playback complettion
  // needs to be thrown in the main thread.
  {
    std::unique_lock<std::mutex> lock(mutex_event_completed_);
    if (is_completed_) {
      is_completed_ = false;
      lock.unlock();

      stream_handler_->OnNotifyCompleted();
      if (auto_repeat_) {
        SetSeek(0);
      }
    }
  }

  return position / GST_MSECOND;
}

bool GstVideoPlayer::SetStreamDataFromUrl(const std::string& uri) {
  std::size_t param_start_pos = uri.find_last_of('?');
  if (param_start_pos == std::string::npos) {
    std::cerr << "Url doesn't contain any param" << std::endl;
    return false;
  }

  std::size_t param_end_pos = uri.find('&', ++param_start_pos);
  std::unordered_map<std::string, std::string> params;
  while (param_end_pos != std::string::npos) {
    param_end_pos = uri.find('&', param_start_pos);
    std::string p =
        uri.substr(param_start_pos, param_end_pos - param_start_pos);
    std::size_t p_pos = p.find('=');
    params.insert(std::make_pair<std::string, std::string>(
        p.substr(0, p_pos), p.substr(p_pos + 1)));
    param_start_pos = param_end_pos + 1;
  }

  if (params.find("w") != params.end())
    width_ = NormalizeResolutionValue(std::stoi(params["w"]));
  else
    std::cerr << "WARNING: width wasn't provided!" << std::endl;

  if (params.find("h") != params.end())
    height_ = NormalizeResolutionValue(std::stoi(params["h"]));
  else
    std::cerr << "WARNING: height wasn't provided!" << std::endl;

  if (params.find("o") != params.end()) {
    if (params["o"] == "l")
      aspect_ratio_ = "16/9";
    else
      aspect_ratio_ = "9/16";
  } else
    std::cerr << "WARNING: orientation wasn't provided!" << std::endl;

  return true;
}

int GstVideoPlayer::NormalizeResolutionValue(const int res_val) {
  return *(std::lower_bound(resolution_values_.begin(),
                            resolution_values_.end(), res_val));
}

void GstVideoPlayer::CorrectAspectRatio() {
  auto* pad = gst_element_get_static_pad(gst_.caps_filter, "src");
  auto* caps = gst_pad_get_current_caps(pad);
  auto* structure = gst_caps_get_structure(caps, 0);

  if (!structure) {
    std::cerr << "Failed to get a structure to correct aspect ratio"
              << std::endl;
    std::cerr << "Setting portrait aspect ratio" << std::endl;

    auto* caps_portrait = gst_caps_from_string(
        "video/x-raw(memory:DMABuf), format=RGBA, pixel-aspect-ratio=9/16");
    g_object_set(G_OBJECT(gst_.caps_filter), "caps", caps_portrait, NULL);

    return;
  }

  gint aspr_n, aspr_d;
  if (!gst_structure_get_fraction(structure, "pixel-aspect-ratio", &aspr_n,
                                  &aspr_d)) {
    std::cerr << "Failed to get aspect-ratio fraction" << std::endl;
    return;
  }

  if (aspr_n != 1 && aspr_d != 1) {
    gst_caps_unref(caps);
    gst_object_unref(pad);
    return;
  }

  if (width_ > height_) {
    aspr_n = 16;
    aspr_d = 9;
  } else {
    aspr_n = 9;
    aspr_d = 16;
  }

  GValue aspr{0};
  memset(&aspr, 0, sizeof(GValue));
  g_value_init(&aspr, GST_TYPE_FRACTION);
  gst_value_set_fraction(&aspr, aspr_n, aspr_d);

  gst_structure_set_value(structure, "pixel-aspect-ratio", &aspr);

  gst_caps_unref(caps);
  gst_object_unref(pad);
}

const uint8_t* GstVideoPlayer::GetFrameBuffer() {
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  if (pixels_.get() == NULL) {
    return nullptr;
  }

  const uint32_t pixel_bytes = width_ * height_ * 4;
  gst_buffer_extract(gst_.buffer, 0, pixels_.get(), pixel_bytes);
  return reinterpret_cast<const uint8_t*>(pixels_.get());
}

// Creats a video pipeline using playbin.
// $ playbin uri=<file> video-sink="videoconvert ! video/x-raw,format=RGBA !
// fakesink"
bool GstVideoPlayer::CreatePipeline() {
  std::string converter{"imxvideoconvert_g2d"};
  std::string capsStr{"video/x-raw,format=RGBA"};
  std::string video_src{"playbin3"};

  gst_.pipeline = gst_pipeline_new("pipeline");
  if (!gst_.pipeline) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    return false;
  }

  gst_.video_src = gst_element_factory_make(video_src.c_str(), "src");
  if (!gst_.video_src) {
    std::cerr << "Failed to create a source" << std::endl;
    return false;
  }

  gst_.video_convert =
      gst_element_factory_make(converter.c_str(), "videoconvert");
  if (!gst_.video_convert) {
    std::cerr << "Failed to create a videoconvert" << std::endl;
    return false;
  }

  gst_.caps_filter = gst_element_factory_make("capsfilter", "filter");
  if (!gst_.caps_filter) {
    std::cerr << "Failed to create a capsfilter" << std::endl;
    return false;
  }

  gst_.video_sink = gst_element_factory_make("fakesink", "videosink");
  if (!gst_.video_sink) {
    std::cerr << "Failed to create a videosink" << std::endl;
    return false;
  }

  gst_.fpssink = gst_element_factory_make("fpsdisplaysink", "fpssink");
  if (!gst_.fpssink) {
    std::cerr << "Failed to create a fpssink" << std::endl;
    return false;
  }

  if (video_src == "playbin3") {
    gst_.output = gst_bin_new("output");
    if (!gst_.output) {
      std::cerr << "Failed to create an output" << std::endl;
      return false;
    }
  }

  gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
  if (!gst_.bus) {
    std::cerr << "Failed to create a bus" << std::endl;
    return false;
  }
  gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

  g_object_set(G_OBJECT(gst_.fpssink), "text-overlay", TRUE, NULL);
  g_object_set(G_OBJECT(gst_.fpssink), "sync", FALSE, NULL);
  g_object_set(G_OBJECT(gst_.fpssink), "video-sink", gst_.video_sink, NULL);

  // Sets properties to fakesink to get the callback of a decoded frame.
  g_object_set(G_OBJECT(gst_.video_sink), "sync", FALSE, "qos", FALSE, NULL);
  g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", TRUE, NULL);
  g_signal_connect(G_OBJECT(gst_.video_sink), "handoff",
                   G_CALLBACK(HandoffHandler), this);

  if (video_src == "playbin3")
    gst_bin_add_many(GST_BIN(gst_.output), gst_.video_convert, gst_.caps_filter,
                     gst_.fpssink, NULL);
  else
    gst_bin_add_many(GST_BIN(gst_.pipeline), gst_.video_src, gst_.camera_caps,
                     gst_.camera_dec, gst_.video_convert, gst_.caps_filter,
                     gst_.fpssink, NULL);

  // Adds caps to the converter to convert the color format to RGBA.
  auto* caps = gst_caps_from_string(capsStr.c_str());
  if (!caps) {
    std::cerr << "Failed to create caps" << std::endl;
    return false;
  }
  g_object_set(G_OBJECT(gst_.caps_filter), "caps", caps, NULL);

  // Sets properties to playbin.
  if (video_src == "playbin3") {
    gst_element_link_many(gst_.video_convert, gst_.caps_filter, gst_.fpssink,
                          NULL);

    auto* sinkpad = gst_element_get_static_pad(gst_.video_convert, "sink");
    auto* ghost_sinkpad = gst_ghost_pad_new("sink", sinkpad);
    gst_pad_set_active(ghost_sinkpad, TRUE);
    gst_element_add_pad(gst_.output, ghost_sinkpad);

    g_object_set(gst_.video_src, "uri", uri_.c_str(), NULL);
    g_object_set(gst_.video_src, "video-sink", gst_.output, NULL);
    gst_bin_add_many(GST_BIN(gst_.pipeline), gst_.video_src, NULL);
  } else {
    gst_element_link_many(gst_.video_src, gst_.camera_caps, gst_.camera_dec,
                          gst_.video_convert, gst_.caps_filter, gst_.video_sink,
                          NULL);

    g_object_set(gst_.video_src, "device", uri_.c_str(), NULL);
  }
  return true;
}

void GstVideoPlayer::Preroll() {
  if (!gst_.video_src) {
    return;
  }

  auto result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
  if (result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return;
  }

  // Waits until the state becomes GST_STATE_PAUSED.
  if (result == GST_STATE_CHANGE_ASYNC) {
    GstState state;
    result =
        gst_element_get_state(gst_.pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
    if (result == GST_STATE_CHANGE_FAILURE) {
      std::cerr << "Failed to get the current state" << std::endl;
    }
  }
}

void GstVideoPlayer::DestroyPipeline() {
  if (gst_.video_sink) {
    g_signal_handlers_disconnect_by_func(
        G_OBJECT(gst_.video_sink), reinterpret_cast<gpointer>(HandoffHandler),
        this);

    auto* sink_pad = gst_element_get_static_pad(gst_.video_sink, "sink");
    if (!sink_pad) {
      std::cerr << "Failed to get a pad" << std::endl;
      return;
    }

    g_signal_handlers_disconnect_by_func(
        sink_pad, reinterpret_cast<gpointer>(OnCapsChanged), this);

    gst_object_unref(sink_pad);

    g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
  }

  if (gst_.pipeline) {
    gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
  }

  if (gst_.buffer) {
    gst_buffer_unref(gst_.buffer);
    gst_.buffer = nullptr;
  }

  if (gst_.bus) {
    gst_object_unref(gst_.bus);
    gst_.bus = nullptr;
  }

  if (gst_.pipeline) {
    gst_object_unref(gst_.pipeline);
    gst_.pipeline = nullptr;
  }

  if (gst_.video_src) {
    gst_.video_src = nullptr;
  }

  if (gst_.output) {
    gst_.output = nullptr;
  }

  if (gst_.video_sink) {
    gst_.video_sink = nullptr;
  }

  if (gst_.fpssink) {
    gst_.fpssink = nullptr;
  }

  if (gst_.video_convert) {
    gst_.video_convert = nullptr;
  }
}

std::string GstVideoPlayer::ParseUri(const std::string& uri) {
  if (gst_uri_is_valid(uri.c_str())) {
    return uri;
  }

  const auto* filename_uri = gst_filename_to_uri(uri.c_str(), NULL);
  if (!filename_uri) {
    std::cerr << "Failed to open " << uri.c_str() << std::endl;
    return uri;
  }
  std::string result_uri(filename_uri);
  delete filename_uri;

  return result_uri;
}

// static
void GstVideoPlayer::OnCapsChanged(GstPad* pad, GParamSpec* pspec,
                                   gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  auto* caps = gst_pad_get_current_caps(pad);
  if (!caps) {
    std::cerr << "Failed to get caps" << std::endl;
    return;
  }

  auto* structure = gst_caps_get_structure(caps, 0);
  if (!structure) {
    std::cerr << "Failed to get a structure" << std::endl;
    return;
  }

  int width;
  int height;
  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);

  if (width == self->width_ && height == self->height_ && !self->initialized_) {
    self->initialized_ = true;
    return;
  }

  if (width != self->width_ || height != self->height_) {
    self->width_ = width;
    self->height_ = height;
    std::cout << "Caps changed: width = " << width << ", height = " << height
              << std::endl;
    self->pixels_.reset(new uint32_t[self->width_ * self->height_]);
  }

  self->initialized_ = true;
  gst_caps_unref(caps);
}

// static
void GstVideoPlayer::HandoffHandler(GstElement* fakesink, GstBuffer* buf,
                                    GstPad* new_pad, gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  auto* caps = gst_pad_get_current_caps(new_pad);
  auto* structure = gst_caps_get_structure(caps, 0);
  int width;
  int height;
  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);
  if (width != self->width_ || height != self->height_) {
    self->width_ = width;
    self->height_ = height;
    self->pixels_.reset(new uint32_t[width * height]);
    std::cout << "Pixel buffer size: width = " << width
              << ", height = " << height << std::endl;
  }

  std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
  if (self->gst_.buffer) {
    gst_buffer_unref(self->gst_.buffer);
    self->gst_.buffer = nullptr;
  }
  self->gst_.buffer = gst_buffer_ref(buf);
  self->stream_handler_->OnNotifyFrameDecoded();
}

// static
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus* bus,
                                                 GstMessage* message,
                                                 gpointer user_data) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      std::lock_guard<std::mutex> lock(self->mutex_event_completed_);
      self->is_completed_ = true;
      break;
    }
    case GST_MESSAGE_WARNING: {
      gchar* debug;
      GError* error;
      gst_message_parse_warning(message, &error, &debug);
      g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(message->src),
                 error->message);
      g_printerr("Warning details: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR: {
      gchar* debug;
      GError* error;
      gst_message_parse_error(message, &error, &debug);
      g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(message->src),
                 error->message);
      g_printerr("Error details: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      self->stream_handler_->OnNotifyError();
      break;
    }
    case GST_MESSAGE_STREAM_STATUS: {
      GstStreamStatusType type;
      GstElement* owner;
      gst_message_parse_stream_status(message, &type, &owner);
      if (type == GST_STREAM_STATUS_TYPE_LEAVE) {
        auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
        self->stream_handler_->OnNotifyError();

      } else if (type == GST_STREAM_STATUS_TYPE_DESTROY) {
        std::cout << "Stream status: " << "DESTROY" << std::endl;
      }
      break;
    }
    default:
      break;
  }
  return GST_BUS_PASS;
}