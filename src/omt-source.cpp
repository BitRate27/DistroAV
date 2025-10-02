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
#include "obs.h"
#include "libomt.h"

#include <util/platform.h>
#include <util/threading.h>

#include <QDesktopServices>
#include <QUrl>

#include <thread>

// OMT source structure
typedef struct {
	obs_source_t *obs_source;
	std::string address;
	omt_receive_t *receiver;
} omt_source_t;

#define PROP_SOURCE "source_name"
#define PROP_URL "url"

const char *omt_source_getname(void *)
{
	return "OMT";
}

obs_properties_t *omt_source_getproperties(void *data)
{
	auto s = (omt_source_t *)data;
	obs_log(LOG_DEBUG, "+omt_source_getproperties(…)");

	obs_properties_t *props = obs_properties_create();

	obs_property_t *source_list = obs_properties_add_list(props, PROP_SOURCE,
							      "DistroAV OMT",
							      OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

	int count = 0;
	auto omt_sources = omt_discovery_getaddresses(&count);

	for (int i = 0; i < count; i++) {
		obs_property_list_add_string(source_list, omt_sources[i], omt_sources[i]);
	}

	return props;
}

void omt_source_getdefaults(obs_data_t *settings)
{
	obs_log(LOG_DEBUG, "+omt_source_getdefaults(…)");
	obs_data_set_default_string(settings, PROP_SOURCE, "/omt");
	obs_log(LOG_DEBUG, "-omt_source_getdefaults(…)");
}

void omt_source_update(void *data, obs_data_t *settings)
{
	auto s = (omt_source_t *)data;
	auto obs_source = s->obs_source;
	auto obs_source_name = obs_source_get_name(obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_update(…)", obs_source_name);

	s->address = std::string(obs_data_get_string(settings, PROP_SOURCE));

	obs_log(LOG_DEBUG, "'%s' -omt_source_update(…)", obs_source_name);
}

void omt_source_shown(void *data) {}

void omt_source_hidden(void *data) {}

void omt_source_activated(void *data) {}

void omt_source_deactivated(void *data) {}

void *omt_source_create(obs_data_t *settings, obs_source_t *obs_source)
{
	auto obs_source_name = obs_source_get_name(obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_create(…)", obs_source_name);

	auto s = (omt_source_t *)bzalloc(sizeof(omt_source_t));
	s->obs_source = obs_source;

	omt_source_update(s, settings);

	s->receiver = omt_receive_create(
		s->address.c_str(), (OMTFrameType)(OMTFrameType::OMTFrameType_Audio | OMTFrameType::OMTFrameType_Video),
		OMTPreferredVideoFormat::OMTPreferredVideoFormat_UYVY, OMTReceiveFlags::OMTReceiveFlags_None);

	if (!s->receiver) {
		obs_log(LOG_ERROR, "'%s' ERR-500 - omt_source_create: Unable to create OMT receiver for address '%s'",
			obs_source_name, s->address.c_str());
		// Cleanup
		bfree(s);
		return nullptr;
	}
	obs_log(LOG_DEBUG, "'%s' -omt_source_create(%s)", obs_source_name, s->address.c_str());

	return s;
}

void omt_source_destroy(void *data)
{
	auto s = (omt_source_t *)data;
	auto obs_source_name = obs_source_get_name(s->obs_source);
	obs_log(LOG_DEBUG, "'%s' +omt_source_destroy(…)", obs_source_name);

	bfree(s);

	obs_log(LOG_DEBUG, "'%s' -omt_source_destroy(…)", obs_source_name);
}

uint32_t omt_source_get_width(void *data)
{
	auto s = (omt_source_t *)data;
	return 1280;
}

uint32_t omt_source_get_height(void *data)
{
	auto s = (omt_source_t *)data;
	return 720;
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
