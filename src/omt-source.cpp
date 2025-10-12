/******************************************************************************
	Copyright (C) 2016-2024 DistroAV <contact@distroav.org>

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, see <https://www.gnu.org/licenses/>.
******************************************************************************/

#include "plugin-main.h"
#include "libomt.h"

#include <util/platform.h>
#include <util/threading.h>

#include <QDesktopServices>
#include <QUrl>

#include <thread>

#define PROP_SOURCE "omt_source_name"
#define PROP_BEHAVIOR "omt_behavior"
#define PROP_TIMEOUT "omt_timeout"
#define PROP_QUALITY "omt_quality"
#define PROP_COLOR_SPACE "omt_colorspace"
#define PROP_PREVIEW "omt_preview"

#define PROP_QUALITY_DEFAULT -1
#define PROP_QUALITY_LOW 0
#define PROP_QUALITY_MEDIUM 1
#define PROP_QUALITY_HIGH 2

#define PROP_CS_DEFAULT -1
#define PROP_CS_601 0
#define PROP_CS_709 1
#define PROP_CS_709_P010 2
#define PROP_CS_2100_HLG_P010 3
#define PROP_CS_2100_PQ_P010 4

#define PROP_BEHAVIOR_KEEP_ACTIVE 0
#define PROP_BEHAVIOR_STOP_RESUME_BLANK 1
#define PROP_BEHAVIOR_STOP_RESUME_LAST_FRAME 2

#define PROP_TIMEOUT_CLEAR_CONTENT 0
#define PROP_TIMEOUT_KEEP_CONTENT 1


typedef struct omt_source_config_t {
	bool reset_omt_receiver = true;
	// Initialize value to true to ensure a receiver reset on OBS launch.

	//
	// Changes that require the OMT receiver to be reset:
	//
	char *omt_source_name;
	int bandwidth;
	int latency;

	//
	// Changes that do NOT require the OMT receiver to be reset:
	//
	int behavior;
	int timeout_action;
	int quality;
	int color_space;
	bool preview;
	video_range_type yuv_range;
	video_colorspace yuv_colorspace;
	bool audio_enabled;
	OMTTally tally;
} omt_source_config_t;

typedef struct omt_source_t {
	obs_source_t *obs_source;
	omt_source_config_t config;

	bool running;
	pthread_t av_thread;

	uint32_t width;
	uint32_t height;

	uint64_t last_frame_timestamp;
} omt_source_t;

static speaker_layout channel_count_to_layout(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(21, 0, 0)
		return SPEAKERS_4POINT0;
#else
		return SPEAKERS_QUAD;
#endif
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static video_colorspace prop_to_colorspace(int index)
{
	switch (index) {
	case PROP_CS_601:
		return VIDEO_CS_601;
	case PROP_CS_2100_HLG_P010:
		return VIDEO_CS_2100_HLG;
	case PROP_CS_2100_PQ_P010:
		return VIDEO_CS_2100_PQ;
	case PROP_CS_709:
	case PROP_CS_709_P010:
	default:
		return VIDEO_CS_709;
	}
}

static video_trc prop_to_frame_trc(int index) 
{
	switch (index) {
	case PROP_CS_2100_HLG_P010:
		return VIDEO_TRC_HLG;
	case PROP_CS_2100_PQ_P010:
		return VIDEO_TRC_PQ;
	case PROP_CS_601:
	case PROP_CS_709:
	case PROP_CS_709_P010:
	default:
		return VIDEO_TRC_DEFAULT;
	}
}

static OMTQuality prop_to_quality(int index)
{
	switch (index) {
	case PROP_QUALITY_HIGH:
		return OMTQuality_High;
	case PROP_QUALITY_MEDIUM:
		return OMTQuality_Medium;
	case PROP_QUALITY_LOW:
		return OMTQuality_Low;
	case PROP_QUALITY_DEFAULT:
	default:
		return OMTQuality_Default;
	}
}

const char *omt_source_getname(void *)
{
	return obs_module_text("NDIPlugin.OMTSourceName");
}

obs_properties_t *omt_source_getproperties(void *)
{
	// auto s = (omt_source_t *)data;
	obs_log(LOG_DEBUG, "+omt_source_getproperties(…)");

	obs_properties_t *props = obs_properties_create();

	obs_property_t *source_list = obs_properties_add_list(props, PROP_SOURCE,
							      obs_module_text("NDIPlugin.SourceProps.SourceName"),
							      OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	
	int count = 0;
	auto omt_sources = omt_discovery_getaddresses(&count);

	for (int i = 0; i < count; i++) {
		obs_property_list_add_string(source_list, omt_sources[i], omt_sources[i]);
	}

	obs_property_t *behavior_list = obs_properties_add_list(props, PROP_BEHAVIOR,
								obs_module_text("NDIPlugin.SourceProps.Behavior"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(behavior_list, obs_module_text("NDIPlugin.SourceProps.Behavior.KeepActive"),
				  PROP_BEHAVIOR_KEEP_ACTIVE);
	obs_property_list_add_int(behavior_list, obs_module_text("NDIPlugin.SourceProps.Behavior.StopResumeBlank"),
				  PROP_BEHAVIOR_STOP_RESUME_BLANK);
	obs_property_list_add_int(behavior_list, obs_module_text("NDIPlugin.SourceProps.Behavior.StopResumeLastFrame"),
				  PROP_BEHAVIOR_STOP_RESUME_LAST_FRAME);

	obs_property_t *timeout_list = obs_properties_add_list(props, PROP_TIMEOUT,
							       obs_module_text("NDIPlugin.SourceProps.Timeout"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(timeout_list, obs_module_text("NDIPlugin.SourceProps.Timeout.KeepContent"),
				  PROP_TIMEOUT_KEEP_CONTENT);
	obs_property_list_add_int(timeout_list, obs_module_text("NDIPlugin.SourceProps.Timeout.ClearContent"),
				  PROP_TIMEOUT_CLEAR_CONTENT);

	obs_property_t *quality_list = obs_properties_add_list(props, PROP_QUALITY, "Suggested Quality",
							     OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_INT);
	if (quality_list != nullptr) {
		obs_property_list_add_int(quality_list, "Default", PROP_QUALITY_DEFAULT);
		obs_property_list_add_int(quality_list, "Low", PROP_QUALITY_LOW);
		obs_property_list_add_int(quality_list, "Medium", PROP_QUALITY_MEDIUM);
		obs_property_list_add_int(quality_list, "High", PROP_QUALITY_HIGH);
	}

	auto colorspace_list = obs_properties_add_list(props, PROP_COLOR_SPACE, "Color Space",
							OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	if (colorspace_list != nullptr) {
		obs_property_list_add_int(colorspace_list, "Default", PROP_CS_DEFAULT);
		obs_property_list_add_int(colorspace_list, "BT601", PROP_CS_601);
		obs_property_list_add_int(colorspace_list, "BT709", PROP_CS_709);
		obs_property_list_add_int(colorspace_list, "BT709_P010", PROP_CS_709_P010);
		obs_property_list_add_int(colorspace_list, "BT2100_HLG_P010", PROP_CS_2100_HLG_P010);
		obs_property_list_add_int(colorspace_list, "BT2100_PQ_P010", PROP_CS_2100_PQ_P010);
	}
	obs_properties_add_bool(props, PROP_PREVIEW, "Preview Mode");

	obs_log(LOG_DEBUG, "-omt_source_getproperties(…)");

	return props;
}

void omt_source_getdefaults(obs_data_t *settings)
{
	obs_log(LOG_DEBUG, "+omt_source_getdefaults(…)");
	obs_data_set_default_int(settings, PROP_BEHAVIOR, PROP_BEHAVIOR_STOP_RESUME_LAST_FRAME);
	obs_data_set_default_int(settings, PROP_TIMEOUT, PROP_TIMEOUT_KEEP_CONTENT);
	obs_data_set_default_int(settings, PROP_QUALITY, PROP_QUALITY_DEFAULT);
	obs_data_set_default_int(settings, PROP_COLOR_SPACE, PROP_CS_DEFAULT);
	obs_data_set_default_bool(settings, PROP_PREVIEW, false);	
	obs_log(LOG_DEBUG, "-omt_source_getdefaults(…)");
}

void deactivate_source_output_video_texture(omt_source_t *source)
{
	// Per https://docs.obsproject.com/reference-sources#c.obs_source_output_video
	// ```
	// void obs_source_output_video(obs_source_t *source, const struct obs_source_frame *frame)
	// Outputs asynchronous video data. Set to NULL to deactivate the texture.
	// ```
	if (source->width == 0 && source->height == 0)
		return;

	source->width = 0;
	source->height = 0;
	obs_log(LOG_DEBUG, "'%s' deactivate_source_output_video_texture(…)", obs_source_get_name(source->obs_source));
	obs_source_output_video(source->obs_source, NULL);
}

void process_empty_frame(omt_source_t *source)
{
	if (source->config.timeout_action == PROP_TIMEOUT_KEEP_CONTENT)
		return;

	uint64_t now = os_gettime_ns();

	// 3 second timeout on no new data received for the source
	uint64_t source_timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(3)).count();

	uint64_t target_timestamp = source->last_frame_timestamp + source_timeout;

	if (now > target_timestamp) {
		deactivate_source_output_video_texture(source);
	}
}

void omt_source_thread_process_audio3(omt_source_config_t *config, OMTMediaFrame *omt_audio_frame,
				      obs_source_t *obs_source, obs_source_audio *obs_audio_frame);

void omt_source_thread_process_video2(omt_source_t *source, OMTMediaFrame *omt_video_frame,
				      obs_source *obs_source, obs_source_frame *obs_video_frame);

// The creation structure that is used to create a new OMT receiver
typedef struct OMT_recv_create_t {
	OMTPreferredVideoFormat video_format = OMTPreferredVideoFormat_UYVY;
	OMTFrameType frameTypes = (OMTFrameType)(OMTFrameType_Audio | OMTFrameType_Video);
	OMTReceiveFlags flags = OMTReceiveFlags_None;
	const char *p_omt_source_name = nullptr;
} OMT_recv_create_t;

void *omt_source_thread(void *data)
{
	auto s = (omt_source_t *)data;
	auto obs_source_name = obs_source_get_name(s->obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_thread(…)", obs_source_name);

	auto config = Config::Current();
	OMTTally tally = {};

	obs_source_audio obs_audio_frame = {};
	obs_source_frame obs_video_frame = {};

	OMT_recv_create_t recv_desc;

	omt_receive_t *omt_receiver = nullptr;
	OMTMediaFrame *frame_received = nullptr;

	//
	// Main OMT receiver loop: BEGIN
	//
	while (s->running) {
		//
		// reset_omt_receiver: BEGIN
		//
		if (s->config.reset_omt_receiver) {
			s->config.reset_omt_receiver = false;

			// If config.omt_receiver_name changed, then so did obs_source_name
			obs_source_name = obs_source_get_name(s->obs_source);

			//
			// Update recv_desc.p_omt_source_name
			//
			recv_desc.p_omt_source_name = s->config.omt_source_name;
			obs_log(LOG_DEBUG,
				"'%s' omt_source_thread: reset_omt_receiver; Setting recv_desc.p_omt_source_name='%s'",
				obs_source_name, //
				recv_desc.p_omt_source_name);

			//
			// Update recv_desc
			//
			recv_desc.frameTypes = (OMTFrameType)(OMTFrameType_Audio | OMTFrameType_Video);

			if (s->config.preview) {
				recv_desc.flags = (OMTReceiveFlags_Preview);
			} else {
				recv_desc.flags = (OMTReceiveFlags_None);
			}

			//
			// Update obs_video_frame
			//
			video_format_get_parameters(prop_to_colorspace(s->config.color_space), VIDEO_RANGE_PARTIAL,
						obs_video_frame.color_matrix, obs_video_frame.color_range_min,
						obs_video_frame.color_range_max);

			obs_video_frame.trc = prop_to_frame_trc(s->config.color_space);

			//
			// recv_desc is fully populated;
			// now reset the OMT receiver, destroying any existing omt_frame_sync or omt_receiver.
			//
			obs_log(LOG_DEBUG, "'%s' omt_source_thread: reset_omt_receiver: Resetting OMT receiver…",
				obs_source_name);

			if (omt_receiver) {
				obs_log(LOG_DEBUG,
					"'%s' omt_source_thread: reset_omt_receiver: omtLib->recv_destroy(omt_receiver)",
					obs_source_name);
				omt_receive_destroy(omt_receiver);
				omt_receiver = nullptr;
			}

			obs_log(LOG_DEBUG,
				"'%s' omt_source_thread: reset_omt_receiver: recv_desc = { p_omt_source_name='%s' }",
				obs_source_name, //
			    recv_desc.p_omt_source_name);
			obs_log(LOG_DEBUG,
				"'%s' omt_source_thread: reset_omt_receiver: +omt_receiver = omtLib->recv_create_v3(&recv_desc)",
				obs_source_name);

			omt_receiver = omt_receive_create(recv_desc.p_omt_source_name, recv_desc.frameTypes,
							  recv_desc.video_format, recv_desc.flags);
			omt_receive_setsuggestedquality(omt_receiver, prop_to_quality(s->config.quality));

			obs_log(LOG_DEBUG,
				"'%s' omt_source_thread: reset_omt_receiver: -omt_receiver = omtLib->recv_create_v3(&recv_desc)",
				obs_source_name);
			if (!omt_receiver) {
				obs_log(LOG_ERROR, "ERR-407 - Error creating the OMT Receiver '%s' set for '%s'",
					recv_desc.p_omt_source_name, obs_source_name);
				obs_log(LOG_DEBUG,
					"'%s' omt_source_thread: reset_omt_receiver: Cannot create omt_receiver for OMT source '%s'",
					obs_source_name, recv_desc.p_omt_source_name);
				break;
			}
		}

		//
		// reset_omt_receiver: END
		//

		//
		// Change Tally: Enable/Disable updated from Plugin settings UI
		//
#if 0
		obs_log(LOG_DEBUG, "'%s' t{pre=%d,pro=%d}",
			obs_source_name, //
			s->config.tally2.on_preview,
			s->config.tally2.on_program);
#endif
		if ((config->TallyPreviewEnabled && s->config.tally.preview != tally.preview) ||
		    (config->TallyProgramEnabled && s->config.tally.program != tally.program)) {
			tally.preview = s->config.tally.preview;
			tally.program = s->config.tally.program;
			obs_log(LOG_INFO, "'%s': Tally status : preview=%d, program=%d", obs_source_name,
				tally.preview, tally.program);
			obs_log(LOG_DEBUG,
				"'%s' omt_source_thread: tally changed; Seomtng tally on_preview=%d, on_program=%d",
				obs_source_name, tally.preview, tally.program);
			omt_receive_settally(omt_receiver, &tally);
		}

		frame_received = omt_receive(omt_receiver, recv_desc.frameTypes, 100); // &video_frame, &audio_frame, nullptr, 100);

		if (frame_received && (frame_received->Type & OMTFrameType_Audio) == OMTFrameType_Audio) {
			//
			// AUDIO
			//
			// obs_log(LOG_DEBUG, "%s: New Audio Frame (Framesync OFF): ts=%d tc=%d", obs_source_name, audio_frame.timestamp, audio_frame.timecode);
			omt_source_thread_process_audio3(&s->config, frame_received, s->obs_source,
								&obs_audio_frame);
		}

		if (frame_received && (frame_received->Type & OMTFrameType_Video) == OMTFrameType_Video) {
			//
			// VIDEO
			//
			// obs_log(LOG_DEBUG, "%s: New Video Frame (Framesync OFF): ts=%d tc=%d", obs_source_name, video_frame.timestamp, video_frame.timecode);
			omt_source_thread_process_video2(s, frame_received, s->obs_source, &obs_video_frame);
		}
	}
	//
	// Main OMT receiver loop: END
	//

	if (omt_receiver) {
		obs_log(LOG_DEBUG, "'%s' omt_source_thread: omtLib->recv_destroy(omt_receiver)",
			obs_source_name);
		omt_receive_destroy(omt_receiver);
		obs_log(LOG_DEBUG, "'%s' omt_source_thread: Reset OMT Receiver", obs_source_name);
		omt_receiver = nullptr;
	}

	obs_log(LOG_DEBUG, "'%s' -omt_source_thread(…)", obs_source_name);

	return nullptr;
}

void omt_source_thread_process_audio3(omt_source_config_t *config, OMTMediaFrame *omt_audio_frame,
				      obs_source_t *obs_source, obs_source_audio *obs_audio_frame)
{
	if (!config->audio_enabled) {
		return;
	}
	if (omt_audio_frame->Type != OMTFrameType_Audio) {
		obs_log(LOG_DEBUG,
			"omt_source_thread_process_audio3: warning: called with non-audio frame of type %d",
			omt_audio_frame->Type);
		return;
	}
	const int channelCount = omt_audio_frame->Channels > 8 ? 8 : omt_audio_frame->Channels;

	obs_audio_frame->speakers = channel_count_to_layout(channelCount);

	obs_audio_frame->timestamp = (uint64_t)(omt_audio_frame->Timestamp);
	
	obs_audio_frame->samples_per_sec = omt_audio_frame->SampleRate;
	obs_audio_frame->format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_audio_frame->frames = channelCount;
	for (int i = 0; i < channelCount; ++i) {
		obs_audio_frame->data[i] =
			(uint8_t *)omt_audio_frame->Data + (i * omt_audio_frame->SamplesPerChannel * 4);
	}

	obs_source_output_audio(obs_source, obs_audio_frame);
}

void omt_source_thread_process_video2(omt_source_t *source, OMTMediaFrame *omt_video_frame,
				      obs_source *obs_source, obs_source_frame *obs_video_frame)
{
	if (omt_video_frame->Type != OMTFrameType_Video) {
		obs_log(LOG_DEBUG,
			"omt_source_thread_process_video2: warning: called with non-video frame of type %d",
			omt_video_frame->Type);
		return;
	}

	switch (omt_video_frame->Codec) {
	case OMTCodec::OMTCodec_BGRA:
		obs_video_frame->format = VIDEO_FORMAT_BGRA;
		break;

	case OMTCodec::OMTCodec_UYVY:
		obs_video_frame->format = VIDEO_FORMAT_UYVY;
		break;

	default:
		obs_log(LOG_ERROR, "ERR-430 - OMT Source uses an unsupported video pixel format: %d.",
			omt_video_frame->Codec);
		obs_log(LOG_DEBUG, "omt_source_thread_process_video2: warning: unsupported video pixel format: %d",
			omt_video_frame->Codec);
		break;
	}

	obs_video_frame->timestamp = (uint64_t)(omt_video_frame->Timestamp);

	source->width = omt_video_frame->Width;
	source->height = omt_video_frame->Height;
	source->last_frame_timestamp = obs_get_video_frame_time();

	obs_video_frame->width = omt_video_frame->Width;
	obs_video_frame->height = omt_video_frame->Height;
	obs_video_frame->linesize[0] = omt_video_frame->Stride;
	obs_video_frame->data[0] = (uint8_t*)omt_video_frame->Data;

	obs_source_output_video(obs_source, obs_video_frame);
}

void omt_source_thread_start(omt_source_t *s)
{
	s->config.reset_omt_receiver = true;
	s->running = true;
	pthread_create(&s->av_thread, nullptr, omt_source_thread, s);
	obs_log(LOG_INFO, "'Started Receiver Thread for OBS source: '%s' and OMT Source Name: %s'",
		obs_source_get_name(s->obs_source), s->config.omt_source_name);
	obs_log(LOG_DEBUG, "'%s' omt_source_thread_start: Started A/V omt_source_thread for OMT source '%s'",
		obs_source_get_name(s->obs_source), s->config.omt_source_name);
}

void omt_source_thread_stop(omt_source_t *s)
{
	if (s->running) {
		s->running = false;
		pthread_join(s->av_thread, NULL);
		auto obs_source = s->obs_source;
		auto obs_source_name = obs_source_get_name(obs_source);
		obs_log(LOG_DEBUG, "'%s' omt_source_thread_stop: Stopped A/V omt_source_thread for OMT source '%s'",
			obs_source_name, s->config.omt_source_name);
	}
}

int omt_safe_strcmp(const char *str1, const char *str2)
{
	if (str1 == str2)
		return 0;
	if (!str1)
		return -1;
	if (!str2)
		return 1;
	return strcmp(str1, str2);
}

void omt_source_update(void *data, obs_data_t *settings)
{
	auto s = (omt_source_t *)data;
	auto obs_source = s->obs_source;
	auto obs_source_name = obs_source_get_name(obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_update(…)", obs_source_name);

	//
	// reset_omt_receiver: BEGIN
	//

	bool reset_omt_receiver = false;
	// TODO : Should this ba a if statement and simplify each following check ?

	auto new_omt_source_name = obs_data_get_string(settings, PROP_SOURCE);
	reset_omt_receiver |= omt_safe_strcmp(s->config.omt_source_name, new_omt_source_name) != 0;
	obs_log(LOG_DEBUG,
		"'%s' omt_source_update: Check for 'OMT Source Name' changes: new_omt_source_name='%s' vs config.omt_source_name='%s'",
		obs_source_name, new_omt_source_name, s->config.omt_source_name);

	if (s->config.omt_source_name != nullptr) {
		bfree(s->config.omt_source_name);
	}

	s->config.omt_source_name = bstrdup(new_omt_source_name);

	auto new_quality = (int)obs_data_get_int(settings, PROP_QUALITY);
	reset_omt_receiver |= (s->config.quality != new_quality);
	obs_log(LOG_DEBUG,
		"'%s' omt_source_update: Check for 'Quality' setting changes: new_quality='%d' vs config.quality='%d'",
		obs_source_name, new_quality, s->config.quality);
	s->config.quality = new_quality;

	auto new_colorspace = (int)obs_data_get_int(settings, PROP_COLOR_SPACE);
	reset_omt_receiver |= (s->config.color_space != new_colorspace);
	obs_log(LOG_DEBUG,
		"'%s' omt_source_update: Check for 'Colorspace' setting changes: new_colorspace='%d' vs config.color_space='%d'",
		obs_source_name, new_colorspace, s->config.color_space);
	s->config.color_space = new_colorspace;

	auto new_preview = obs_data_get_bool(settings, PROP_PREVIEW);
	reset_omt_receiver |= (s->config.preview != new_preview);
	obs_log(LOG_DEBUG,
		"'%s' omt_source_update: Check for 'Preview' setting changes: new_preview='%d' vs config.preview='%d'",
		obs_source_name, new_preview, s->config.preview);
	s->config.preview = new_preview;

	//
	// reset_omt_receiver: END
	//

#if 0
	// Test overloading these in the config file at:
	// Linux: ~/.config/obs-studio/basic/scenes/...
	// MacOS: ~/Library/Application Support/obs-studio/basic/scenes/...
	// Windows: %APPDATA%\obs-studio\basic\scenes\...
	Example:
	        "name": "OMT™ Source MACBOOK",
            "uuid": "be1ef1d6-5eb6-404d-8cb9-7f6d0755f7f1",
            "id": "omt_source",
            "versioned_id": "omt_source",
            "settings": {
                "omt_fix_alpha_bleomtng": false,
                "omt_source_name": "MACBOOK.LOCAL (Scan Converter)",
                "omt_behavior_lastframe": true,
                "omt_bw_mode": 0,
                "omt_behavior": 1
            },
#endif

	// Source visibility settings update: START
	// In 4.14.x, the "Visibility Behavior" property was used to control the visibility of the source via dropdown and an additional tickbox, creating confusion.
	// In 6.0.0, the "Visibility Behavior" property was replaced with a single dropdown.
	// This is a breaking change in v6.0.0 and invalid "Visibility Behavior" are set to "Keep Active" which is the default from previous versions.

	auto behavior = obs_data_get_int(settings, PROP_BEHAVIOR);

	obs_log(LOG_DEBUG,
		"'%s' omt_source_update: Check for 'Behavior' setting changes: behavior='%d' vs config.behavior='%d'",
		obs_source_name, behavior, s->config.behavior);

	if (behavior == PROP_BEHAVIOR_KEEP_ACTIVE) {
		// Keep connection active.
		s->config.behavior = PROP_BEHAVIOR_KEEP_ACTIVE;

	} else if (behavior == PROP_BEHAVIOR_STOP_RESUME_BLANK) {
		// Stop the connection and resume it with a clean frame.
		s->config.behavior = PROP_BEHAVIOR_STOP_RESUME_BLANK;

	} else if (behavior == PROP_BEHAVIOR_STOP_RESUME_LAST_FRAME) {
		// Stop the connection and resume it with the last diplayed frame.
		s->config.behavior = PROP_BEHAVIOR_STOP_RESUME_LAST_FRAME;

	} else {
		// Fallback option. If the behavior is invalid, force it to "Keep Active" as it most likely came from the 4.14.x version.
		obs_log(LOG_DEBUG, "'%s' omt_source_update: Invalid or unknown behavior detected :'%s' forced to '%d'",
			obs_source_name, behavior, PROP_BEHAVIOR_KEEP_ACTIVE);
		obs_log(LOG_WARNING,
			"WARN-414 - Invalid or unknown behavior detected in config file for source '%s': '%s' forced to '%d'",
			obs_source_name, behavior, PROP_BEHAVIOR_KEEP_ACTIVE);
		obs_data_set_int(settings, PROP_BEHAVIOR, PROP_BEHAVIOR_KEEP_ACTIVE);
		s->config.behavior = PROP_BEHAVIOR_KEEP_ACTIVE;
	}

	s->config.timeout_action = obs_data_get_int(settings, PROP_TIMEOUT);

	// Clean the source content when settings change unless requested otherwise.
	// Always clean if the receiver is reset as well.
	if (s->config.behavior == PROP_BEHAVIOR_STOP_RESUME_BLANK ||
	    reset_omt_receiver) {
		obs_log(LOG_DEBUG,
			"'%s' omt_source_update: Deactivate source output video (Actively reset the frame content)",
			obs_source_name);
		deactivate_source_output_video_texture(s);
	}

	//
	// Source visibility settings update END
	//

	// Update tally status
	auto config = Config::Current();
	s->config.tally.preview = config->TallyPreviewEnabled && obs_source_showing(obs_source);
	s->config.tally.program = config->TallyProgramEnabled && obs_source_active(obs_source);

	if (strlen(s->config.omt_source_name) == 0) {
		obs_log(LOG_DEBUG, "'%s' omt_source_update: No OMT Source selected; Requesting Source Thread Stop.",
			obs_source_name);
		omt_source_thread_stop(s);
	} else {
		obs_log(LOG_DEBUG, "'%s' omt_source_update: OMT Source '%s' selected.", obs_source_name,
			s->config.omt_source_name);
		if (s->running) {
			//
			// Thread is running; notify it if it needs to reset the OMT receiver
			//
			s->config.reset_omt_receiver = reset_omt_receiver;
		} else {
			//
			// Thread is not running; start it if either:
			// 1. the source is active
			//    -or-
			// 2. the behavior property is set to keep the OMT receiver running
			//
			if (obs_source_active(obs_source) || s->config.behavior == PROP_BEHAVIOR_KEEP_ACTIVE) {
				obs_log(LOG_DEBUG, "'%s' omt_source_update: Requesting Source Thread Start.",
					obs_source_name);
				omt_source_thread_start(s);
			}
		}
	}
	// Provide all the source config when updated
	obs_log(LOG_INFO,
		"OMT Source Updated: '%s', 'Bandwidth'='%d', Latency='%d', behavior='%d', timeoutmode='%d', yuv_range='%d', yuv_colorspace='%d'",
		s->config.omt_source_name, s->config.bandwidth, s->config.latency,
		s->config.behavior, s->config.timeout_action, s->config.yuv_range, s->config.yuv_colorspace);

	obs_log(LOG_DEBUG, "'%s' -omt_source_update(…)", obs_source_name);
}

void omt_source_shown(void *data)
{
	// NOTE: This does NOT fire when showing a source in Preview that is also in Program.
	auto s = (omt_source_t *)data;
	auto obs_source_name = obs_source_get_name(s->obs_source);
	obs_log(LOG_DEBUG, "'%s' omt_source_shown(…)", obs_source_name);
	s->config.tally.preview = (Config::Current())->TallyPreviewEnabled;
	if (!s->running) {
		obs_log(LOG_DEBUG, "'%s' omt_source_shown: Requesting Source Thread Start.", obs_source_name);
		omt_source_thread_start(s);
	}
}

void omt_source_hidden(void *data)
{
	// NOTE: This does NOT fire when hiding a source in Preview that is also in Program.
	auto s = (omt_source_t *)data;
	auto obs_source_name = obs_source_get_name(s->obs_source);
	obs_log(LOG_DEBUG, "'%s' omt_source_hidden(…)", obs_source_name);
	s->config.tally.preview = false;
	if (s->running && s->config.behavior != PROP_BEHAVIOR_KEEP_ACTIVE) {
		obs_log(LOG_DEBUG, "'%s' omt_source_hidden: Requesting Source Thread Stop.", obs_source_name);
		// Stopping the thread may result in `on_preview=false` not getting sent,
		// but the thread's `omtLib->recv_destroy` results in an implicit tally off.
		omt_source_thread_stop(s);
	}
}

void omt_source_activated(void *data)
{
	auto s = (omt_source_t *)data;
	auto obs_source_name = obs_source_get_name(s->obs_source);
	obs_log(LOG_DEBUG, "'%s' omt_source_activated(…)", obs_source_name);
	s->config.tally.program = (Config::Current())->TallyProgramEnabled;
	if (!s->running) {
		obs_log(LOG_DEBUG, "'%s' omt_source_activated: Requesting Source Thread Start.", obs_source_name);
		omt_source_thread_start(s);
	}
}

void omt_source_deactivated(void *data)
{
	auto s = (omt_source_t *)data;
	obs_log(LOG_DEBUG, "'%s' omt_source_deactivated(…)", obs_source_get_name(s->obs_source));
	s->config.tally.program = false;
}

void *omt_source_create(obs_data_t *settings, obs_source_t *obs_source)
{
	auto obs_source_name = obs_source_get_name(obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_create(…)", obs_source_name);

	auto s = (omt_source_t *)bzalloc(sizeof(omt_source_t));
	s->obs_source = obs_source;

	omt_source_update(s, settings);

	obs_log(LOG_DEBUG, "'%s' -omt_source_create(…)", obs_source_name);

	return s;
}

void omt_source_destroy(void *data)
{
	auto s = (omt_source_t *)data;
	auto obs_source_name = obs_source_get_name(s->obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_destroy(…)", obs_source_name);

	omt_source_thread_stop(s);

	if (s->config.omt_source_name) {
		bfree(s->config.omt_source_name);
		s->config.omt_source_name = nullptr;
	}

	bfree(s);

	obs_log(LOG_DEBUG, "'%s' -omt_source_destroy(…)", obs_source_name);
}

uint32_t omt_source_get_width(void *data)
{
	auto s = (omt_source_t *)data;
	return s->width;
}

uint32_t omt_source_get_height(void *data)
{
	auto s = (omt_source_t *)data;
	return s->height;
}

obs_source_info create_omt_source_info()
{
	// https://docs.obsproject.com/reference-sources#source-definition-structure-obs-source-info
	obs_source_info omt_source_info = {};
	omt_source_info.id = "omt_source";
	omt_source_info.type = OBS_SOURCE_TYPE_INPUT;
	omt_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;

	omt_source_info.get_name = omt_source_getname;
	omt_source_info.get_properties = omt_source_getproperties;
	omt_source_info.get_defaults = omt_source_getdefaults;

	omt_source_info.create = omt_source_create;
	omt_source_info.activate = omt_source_activated;
	omt_source_info.show = omt_source_shown;
	omt_source_info.update = omt_source_update;
	omt_source_info.hide = omt_source_hidden;
	omt_source_info.deactivate = omt_source_deactivated;
	omt_source_info.destroy = omt_source_destroy;

	omt_source_info.get_width = omt_source_get_width;
	omt_source_info.get_height = omt_source_get_height;

	return omt_source_info;
}
