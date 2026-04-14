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
#include <util/threading.h>
#include <chrono>
#include <sstream>
#include <ndi-shared.h>

// #include "plugin-support.h"

static FORCE_INLINE uint32_t min_uint32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

typedef void (*uyvy_conv_function)(uint8_t *input[], uint32_t in_linesize[], uint32_t start_y, uint32_t end_y,
				   uint8_t *output, uint32_t out_linesize);

static void convert_i444_to_uyvy(uint8_t *input[], uint32_t in_linesize[], uint32_t start_y, uint32_t end_y,
				 uint8_t *output, uint32_t out_linesize)
{
	uint8_t *_Y;
	uint8_t *_U;
	uint8_t *_V;
	uint8_t *_out;
	uint32_t width = min_uint32(in_linesize[0], out_linesize);
	for (uint32_t y = start_y; y < end_y; ++y) {
		_Y = input[0] + ((size_t)y * (size_t)in_linesize[0]);
		_U = input[1] + ((size_t)y * (size_t)in_linesize[1]);
		_V = input[2] + ((size_t)y * (size_t)in_linesize[2]);

		_out = output + ((size_t)y * (size_t)out_linesize);

		for (uint32_t x = 0; x < width; x += 2) {
			// Quality loss here. Some chroma samples are ignored.
			*(_out++) = *(_U++);
			_U++;
			*(_out++) = *(_Y++);
			*(_out++) = *(_V++);
			_V++;
			*(_out++) = *(_Y++);
		}
	}
}

typedef struct ndi_server_connection_t {
	HANDLE hShmReq;
	HANDLE hShmReqA;
	HANDLE hShmRsp;
	HANDLE hEvtCmd;
	HANDLE hEvtRsp;
	HANDLE hEvtCmdA;
	HANDLE hEvtRspA;
	HANDLE hEvtReady;
	RequestBlock *pReq;
	RequestBlock *pReqA;
	ResponseBlock *pRsp;
	int error;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	bool running;
	bool sender_created;
} ndi_server_connection_t;

typedef struct {
	obs_output_t *output;
	const char *ndi_name;
	const char *ndi_groups;
	bool uses_video;
	bool uses_audio;

	bool started;

	NDIlib_send_instance_t ndi_sender;
	pthread_mutex_t ndi_sender_mutex;

	uint32_t frame_width;
	uint32_t frame_height;
	NDIlib_FourCC_video_type_e frame_fourcc;
	double video_framerate;

	size_t audio_channels;
	uint32_t audio_samplerate;

	uint8_t *conv_buffer;
	uint32_t conv_linesize;
	uyvy_conv_function conv_function;

	uint8_t *audio_conv_buffer;
	size_t audio_conv_buffer_size;
	int32_t no_connections;
	std::chrono::time_point<std::chrono::steady_clock> last_conn_check;
	ndi_server_connection_t server_conn;
} ndi_output_t;

void destroy_ndi_server(ndi_server_connection_t &conn)
{
	// Cleanup
	// Kill the server process if it's still running
	if (conn.running) {
		conn.pReq->command = NDI_SHUTDOWN;
		SetEvent(conn.hEvtCmd);
		WaitForSingleObject(conn.hEvtRsp, NDI_SERVER_WAIT);
	}

	if (conn.pReq)
		UnmapViewOfFile(conn.pReq);
	if (conn.pReqA)
		UnmapViewOfFile(conn.pReqA);
	if (conn.pRsp)
		UnmapViewOfFile(conn.pRsp);
	if (conn.hShmReq)
		CloseHandle(conn.hShmReq);
	if (conn.hShmReq)
		CloseHandle(conn.hShmReq);
	if (conn.hShmRsp)
		CloseHandle(conn.hShmRsp);
	if (conn.hEvtCmd)
		CloseHandle(conn.hEvtCmd);
	if (conn.hEvtRsp)
		CloseHandle(conn.hEvtRsp);
	if (conn.hEvtCmdA)
		CloseHandle(conn.hEvtCmdA);
	if (conn.hEvtRspA)
		CloseHandle(conn.hEvtRspA);
	if (conn.hEvtReady)
		CloseHandle(conn.hEvtReady);
	conn = {0};
	return;
};

// Create an NDI server and launch the server process. Returns a struct with connection handles and pointers, or an error code if failed.
ndi_server_connection_t create_ndi_server(const char *output_name)
{
	ndi_server_connection_t conn = {0};
	conn.running = false;
	conn.sender_created = false;

	const WCHAR *memPrefix = NDI_MEM_NAME_PREFIX;

	// Create a unique ID for shared memory so ndi-servers do not collide
	std::wostringstream uniqueID;
	uniqueID << output_name << GetCurrentProcessId();
	std::wstring ustr = uniqueID.str();

	WCHAR connectionName[256] = {0};
	WCHAR requestShmName[256] = {0};
	WCHAR requestShmNameA[256] = {0};
	WCHAR responseShmName[256] = {0};
	WCHAR commandEventName[256] = {0};
	WCHAR commandEventNameA[256] = {0};
	WCHAR readyEventName[256] = {0};
	WCHAR responseEventName[256] = {0};
	WCHAR responseEventNameA[256] = {0};

	// argv[1] is the shared memory name (narrow). Convert to wide.
	swprintf_s(connectionName, 256, L"%s%s", NDI_MEM_NAME_PREFIX, ustr.c_str());
	swprintf_s(requestShmName, 256, L"%s%s", connectionName, NDI_REQUEST_SHM_SUFFIX);
	swprintf_s(requestShmNameA, 256, L"%s%s", connectionName, NDI_AREQUEST_SHM_SUFFIX);
	swprintf_s(responseShmName, 256, L"%s%s", connectionName, NDI_RESPONSE_SHM_SUFFIX);
	swprintf_s(commandEventName, 256, L"%s%s", connectionName, NDI_COMMAND_EVENT_SUFFIX);
	swprintf_s(readyEventName, 256, L"%s%s", connectionName, NDI_READY_EVENT_SUFFIX);
	swprintf_s(responseEventName, 256, L"%s%s", connectionName, NDI_RESPONSE_EVENT_SUFFIX);
	swprintf_s(commandEventNameA, 256, L"%s%s", connectionName, NDI_ACOMMAND_EVENT_SUFFIX);
	swprintf_s(responseEventNameA, 256, L"%s%s", connectionName, NDI_ARESPONSE_EVENT_SUFFIX);

	//    Boost priority
	//SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	//SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	//
	//  1.  Create shared-memory file mappings backed by the system pagefile.
	//      Using INVALID_HANDLE_VALUE means "pagefile-backed" (anonymous shm).
	//      High 32-bit size word first for the large response buffer.
	//
	const DWORD reqHi = (DWORD)(((UINT64)sizeof(RequestBlock)) >> 32);
	const DWORD reqLo = (DWORD)((UINT64)sizeof(RequestBlock) & 0xFFFFFFFFu);
	conn.hShmReq = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, reqHi, reqLo, requestShmName);
	if (!conn.hShmReq || GetLastError() == ERROR_ALREADY_EXISTS) {
		obs_log(LOG_ERROR, "CreateFileMapping(request) is another instance already running?");
		conn.error = 1;
		return conn;
	}

	conn.hShmReqA = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, reqHi, reqLo, requestShmNameA);
	if (!conn.hShmReqA || GetLastError() == ERROR_ALREADY_EXISTS) {
		obs_log(LOG_ERROR, "CreateFileMapping(requestA) is another instance already running?");
		conn.error = 1;
		return conn;
	}

	const DWORD rspHi = (DWORD)(((UINT64)sizeof(ResponseBlock)) >> 32);
	const DWORD rspLo = (DWORD)((UINT64)sizeof(ResponseBlock) & 0xFFFFFFFFu);
	conn.hShmRsp = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, rspHi, rspLo, responseShmName);
	if (!conn.hShmRsp || GetLastError() == ERROR_ALREADY_EXISTS) {
		obs_log(LOG_ERROR, "CreateFileMapping(response) is another instance already running?");
		destroy_ndi_server(conn);
		conn.error = 2;
		return conn;
	}

	//    2.  Create auto-reset named events
	//   FALSE initial state, FALSE manual-reset (= auto-reset)
	conn.hEvtCmd = CreateEventW(nullptr, FALSE, FALSE, commandEventName);
	if (!conn.hEvtCmd) {
		destroy_ndi_server(conn);
		obs_log(LOG_ERROR, "CreateEvent(cmd)");
		conn.error = 3;
		return conn;
	}

	conn.hEvtRsp = CreateEventW(nullptr, FALSE, FALSE, responseEventName);
	if (!conn.hEvtRsp) {
		destroy_ndi_server(conn);
		obs_log(LOG_ERROR, "CreateEvent(rsp)");
		conn.error = 4;
		return conn;
	}

	conn.hEvtCmdA = CreateEventW(nullptr, FALSE, FALSE, commandEventNameA);
	if (!conn.hEvtCmdA) {
		destroy_ndi_server(conn);
		obs_log(LOG_ERROR, "CreateEvent(cmdA)");
		conn.error = 3;
		return conn;
	}

	conn.hEvtRspA = CreateEventW(nullptr, FALSE, FALSE, responseEventNameA);
	if (!conn.hEvtRspA) {
		destroy_ndi_server(conn);
		obs_log(LOG_ERROR, "CreateEvent(rspA)");
		conn.error = 4;
		return conn;
	}

	// The "ready" event is manual-reset so we can call WaitForSingleObject
	// after the server has already set it (won't miss it).
	conn.hEvtReady = CreateEventW(nullptr, TRUE /*manual*/, FALSE, readyEventName);
	if (!conn.hEvtReady) {
		destroy_ndi_server(conn);
		conn.error = 5;
		obs_log(LOG_ERROR, "CreateEvent(ready)");
		return conn;
	}

	//    3.  Map views
	conn.pReq =
		static_cast<RequestBlock *>(MapViewOfFile(conn.hShmReq, FILE_MAP_WRITE, 0, 0, sizeof(RequestBlock)));
	if (!conn.pReq) {
		destroy_ndi_server(conn);
		conn.error = 6;
		obs_log(LOG_ERROR, "MapViewOfFile(request)");
		return conn;
	}
	
	conn.pReqA =
		static_cast<RequestBlock *>(MapViewOfFile(conn.hShmReqA, FILE_MAP_WRITE, 0, 0, sizeof(RequestBlock)));
	if (!conn.pReqA) {
		destroy_ndi_server(conn);
		conn.error = 6;
		obs_log(LOG_ERROR, "MapViewOfFile(requestA)");
		return conn;
	}

	conn.pRsp =
		static_cast<ResponseBlock *>(MapViewOfFile(conn.hShmRsp, FILE_MAP_READ, 0, 0, sizeof(ResponseBlock)));
	if (!conn.pRsp) {
		conn.error = 7;
		destroy_ndi_server(conn);
		obs_log(LOG_ERROR, "MapViewOfFile(response)");
		return conn;
	}

	//    4.  Write client PID so the server can monitor us
	memset(conn.pReq, 0, sizeof(RequestBlock));
	conn.pReq->client_pid = GetCurrentProcessId();

	//    5.  Launch server.exe
	// --- Spawn the ndi-server from same directory as this module ---
	WCHAR modulePath[MAX_PATH] = {0};
	WCHAR exePath[MAX_PATH] = {0};

	// Get HMODULE for this code module using address of this function
	HMODULE hMod = NULL;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       reinterpret_cast<LPCWSTR>(reinterpret_cast<LPVOID>(destroy_ndi_server)), &hMod)) {
		if (GetModuleFileNameW(hMod, modulePath, MAX_PATH) > 0) {
			WCHAR *last = wcsrchr(modulePath, L'\\');
			if (last) {
				// Keep trailing backslash
				*(last + 1) = L'\0';
				swprintf_s(exePath, MAX_PATH, L"%sndi-server.exe", modulePath);
			}
		}
	}

	// Fallback to simple name if resolution failed
	if (exePath[0] == L'\0') {
		swprintf_s(exePath, MAX_PATH, L"ndi-server.exe");
	}

	// Check that the executable exists and is not a directory. If missing, log an error so it's visible.
	DWORD attrs = GetFileAttributesW(exePath);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		obs_log(LOG_ERROR, "Missing ndi-server.exe at expected path: %ls\n", exePath);
		destroy_ndi_server(conn);
		conn.error = 7;
		return conn;
	}

	// Build command line (quote path in case it contains spaces)
	WCHAR cmdLine[512];

	swprintf_s(cmdLine, 512, L"\"%s\" \"%s\" --log", exePath, ustr.c_str());
	//swprintf_s(cmdLine, 512, L"\"%s\" \"%s\"", exePath, ustr.c_str());
	conn.si = {sizeof(conn.si)};
	conn.pi = {};
	ZeroMemory(&conn.si, sizeof(conn.si));
	conn.si.cb = sizeof(conn.si);
	if (!NDI_SERVER_DEBUG) {
		conn.si.dwFlags |= STARTF_USESHOWWINDOW;
		conn.si.wShowWindow = SW_HIDE;
	}

	ZeroMemory(&conn.pi, sizeof(conn.pi));
	DWORD creationFlags = CREATE_NO_WINDOW;
	if (NDI_SERVER_DEBUG)
		creationFlags = CREATE_NEW_CONSOLE;
	obs_log(LOG_INFO, "Spawning ndi-server with command line: %ls", cmdLine);

	if (!CreateProcessW(exePath,          // application
			    cmdLine,          // command line
			    nullptr, nullptr, // process / thread security
			    FALSE,            // do NOT inherit handles (keeps things clean)
			    creationFlags, nullptr, nullptr, &conn.si, &conn.pi)) {

		obs_log(LOG_INFO, "CreateProcess(ndi-server.exe)  � make sure ndi-server.exe is next to distroav.dll");
		destroy_ndi_server(conn);
		conn.error = 9;
		return conn;
	}

	CloseHandle(conn.pi.hThread); // we don't need the thread handle
	obs_log(LOG_INFO, "[Client] Server PID: %u.  Waiting for ready signal (up to 5 s)...", conn.pi.dwProcessId);

	//    6.  Wait for the server to finish pre-faulting and signal ready
	if (WaitForSingleObject(conn.hEvtReady, NDI_SERVER_WAIT) != WAIT_OBJECT_0) {
		destroy_ndi_server(conn);
		conn.error = 8;
		obs_log(LOG_INFO, "[Client] ndi-server did not signal ready within 5 s.\n");
		return conn;
	}
	obs_log(LOG_INFO, "[Client] ndi-server is ready.");

	//    7.  Pre-fault our read view of the response buffer
	obs_log(LOG_INFO, "[Client] Pre-faulting shm (%u MB)...", sizeof(ResponseBlock) / (1024u * 1024u));
	PrefaultRegionRead(conn.pRsp, sizeof(ResponseBlock));
	PrefaultRegionRead(conn.pReq, sizeof(ResponseBlock));
	PrefaultRegionRead(conn.pReqA, sizeof(ResponseBlock));
	obs_log(LOG_INFO, "[Client] Pre-fault complete.");

	conn.running = true;
	conn.sender_created = false;
	return conn;
}

// If the ndi name is unchanged from the existing connection, will not create a new connection, but will
// send command to create new receiver on server with recv_desc parameters.
void create_ndi_sender(ndi_server_connection_t &conn, NDIlib_send_create_t send_desc)
{
	size_t out_written = 0;
	serialize_send_desc(send_desc, &conn.pReq->payload, sizeof(conn.pReq->payload), out_written);

	// Signal the consumer that new data is ready
	conn.pReq->command = NDI_CREATE_SENDER;
	SetEvent(conn.hEvtCmd);
	DWORD w = WaitForSingleObject(conn.hEvtRsp, NDI_SERVER_WAIT);
	if (w != WAIT_OBJECT_0) {
		obs_log(LOG_ERROR, "Timed out waiting for ndi-server create a sender");
		destroy_ndi_server(conn);
		return;
	}
	obs_log(LOG_INFO, "NDI sender creation command sent to server, out_written=%zu", out_written);
	conn.error = 0;
	conn.sender_created = true;
	return;
}

const char *ndi_output_getname(void *)
{
	return obs_module_text("NDIPlugin.OutputName");
}

obs_properties_t *ndi_output_getproperties(void *)
{
	obs_log(LOG_DEBUG, "+ndi_output_getproperties()");

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "ndi_name", obs_module_text("NDIPlugin.OutputProps.NDIName"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "ndi_groups", obs_module_text("NDIPlugin.OutputProps.NDIGroups"),
				OBS_TEXT_DEFAULT);

	obs_log(LOG_DEBUG, "-ndi_output_getproperties()");

	return props;
}

void ndi_output_getdefaults(obs_data_t *settings)
{
	obs_log(LOG_DEBUG, "+ndi_output_getdefaults()");
	obs_data_set_default_string(settings, "ndi_name", "DistroAV output (changeme)");
	obs_data_set_default_string(settings, "ndi_groups", "DistroAV output (changeme)");
	obs_data_set_default_bool(settings, "uses_video", true);
	obs_data_set_default_bool(settings, "uses_audio", true);
	obs_log(LOG_DEBUG, "-ndi_output_getdefaults()");
}

void ndi_output_update(void *data, obs_data_t *settings);

void *ndi_output_create(obs_data_t *settings, obs_output_t *output)
{
	auto name = obs_data_get_string(settings, "ndi_name");
	auto groups = obs_data_get_string(settings, "ndi_groups");
	obs_log(LOG_DEBUG, "+ndi_output_create(name='%s', groups='%s', ...)", name, groups);
	auto o = (ndi_output_t *)bzalloc(sizeof(ndi_output_t));
	o->output = output;
	pthread_mutex_init(&o->ndi_sender_mutex, NULL);
	ndi_output_update(o, settings);

	// initialize last_conn_check so first check will occur immediately
	o->no_connections = -1;
	o->last_conn_check = std::chrono::steady_clock::time_point();

	obs_log(LOG_DEBUG, "-ndi_output_create(name='%s', groups='%s', ...)", name, groups);
	return o;
}

static const std::map<video_format, std::string> video_to_color_format_map = {{VIDEO_FORMAT_P010, "P010"},
									      {VIDEO_FORMAT_I010, "I010"},
									      {VIDEO_FORMAT_P216, "P216"},
									      {VIDEO_FORMAT_P416, "P416"}};

bool ndi_output_start(void *data)
{
	auto o = (ndi_output_t *)data;
	auto name = o->ndi_name;
	auto groups = o->ndi_groups;
	obs_log(LOG_DEBUG, "+ndi_output_start(name='%s', groups='%s', ...)", name, groups);
	if (o->started) {
		obs_log(LOG_INFO, "NDI Output already started: '%s'", name);
		obs_log(LOG_DEBUG, "-ndi_output_start(name='%s', groups='%s', ...)", name, groups);
		return false;
	}

	uint32_t flags = 0;
	video_t *video = obs_output_video(o->output);
	audio_t *audio = obs_output_audio(o->output);
	obs_output_set_last_error(o->output, "");

	if (!video && !audio) {
		obs_log(LOG_WARNING, "WARN-413 - NDI Output could not start. No Audio/Video data available. ('%s')",
			name);
		obs_log(LOG_DEBUG, "'%s'('%s') ndi_output_start: no video nor audio available", name, groups);
		return false;
	}

	if (o->uses_video && video) {
		video_format format = video_output_get_format(video);
		uint32_t width = video_output_get_width(video);
		uint32_t height = video_output_get_height(video);

		switch (format) {
		case VIDEO_FORMAT_I444:
			o->conv_function = convert_i444_to_uyvy;
			o->frame_fourcc = NDIlib_FourCC_video_type_UYVY;
			o->conv_linesize = width * 2;
			o->conv_buffer = new uint8_t[(size_t)height * (size_t)o->conv_linesize * 2]();
			break;

		case VIDEO_FORMAT_NV12:
			o->frame_fourcc = NDIlib_FourCC_video_type_NV12;
			break;

		case VIDEO_FORMAT_I420:
			o->frame_fourcc = NDIlib_FourCC_video_type_I420;
			break;

		case VIDEO_FORMAT_RGBA:
			o->frame_fourcc = NDIlib_FourCC_video_type_RGBA;
			break;

		case VIDEO_FORMAT_BGRA:
			o->frame_fourcc = NDIlib_FourCC_video_type_BGRA;
			break;

		case VIDEO_FORMAT_BGRX:
			o->frame_fourcc = NDIlib_FourCC_video_type_BGRX;
			break;

		default:
			obs_log(LOG_ERROR, "ERR-410 - NDI Output cannot start : Unsupported pixel format %d. ('%s')",
				format, name);
			obs_log(LOG_DEBUG, "-ndi_output_start(name='%s', groups='%s', ...)", name, groups);
			auto error_string = obs_module_text("NDIPlugin.OutputSettings.LastError") +
					    video_to_color_format_map.at(format);
			obs_output_set_last_error(o->output, error_string.c_str());
			return false;
		}

		o->frame_width = width;
		o->frame_height = height;
		o->video_framerate = video_output_get_frame_rate(video);
		flags |= OBS_OUTPUT_VIDEO;
	}

	if (o->uses_audio && audio) {
		o->audio_samplerate = audio_output_get_sample_rate(audio);
		o->audio_channels = audio_output_get_channels(audio);
		flags |= OBS_OUTPUT_AUDIO;
	}

	NDIlib_send_create_t send_desc{};
	send_desc.p_ndi_name = name;
	if (groups && groups[0])
		send_desc.p_groups = groups;
	else
		send_desc.p_groups = nullptr;
	send_desc.clock_video = false;
	send_desc.clock_audio = false;

	pthread_mutex_lock(&o->ndi_sender_mutex);
	if (!o->server_conn.running) {
		o->server_conn = create_ndi_server(name);
		if (o->server_conn.error != 0) {
			obs_log(LOG_ERROR, "Failed to create NDI server for output '%s', error code %d", name, o->server_conn.error);
			obs_log(LOG_DEBUG, "'%s' ndi_output_start: failed to create ndi server, error code %d", name,
				o->server_conn.error);
		} else {
			create_ndi_sender(o->server_conn, send_desc);
		}
	}

	if (!o->server_conn.running || !o->server_conn.sender_created) {
		o->ndi_sender = ndiLib->send_create(&send_desc);
	}

	if (o->ndi_sender || (o->server_conn.running && o->server_conn.sender_created)) {
		o->started = obs_output_begin_data_capture(o->output, flags);
		if (o->started) {
			obs_log(LOG_INFO, "NDI Output started successfully. '%s'", name);
			obs_log(LOG_DEBUG, "'%s' ndi_output_start: ndi output started", name);
		} else {
			obs_log(LOG_WARNING, "WARN-415 - NDI Sender data capture failed. '%s'", name);
			obs_log(LOG_DEBUG, "'%s' ndi_output_start: data capture start failed", name);
		}
	} else {
		obs_log(LOG_WARNING, "WARN-416 - NDI Sender initialisation failed. '%s'", name);
		obs_log(LOG_DEBUG, "'%s' ndi_output_start: ndi sender init failed", name);
	}

	obs_log(LOG_DEBUG, "-ndi_output_start(name='%s', groups='%s'...)", name, groups);
	pthread_mutex_unlock(&o->ndi_sender_mutex);

	return o->started;
}

void ndi_output_update(void *data, obs_data_t *settings)
{
	auto o = (ndi_output_t *)data;
	auto name = obs_data_get_string(settings, "ndi_name");
	auto groups = obs_data_get_string(settings, "ndi_groups");
	obs_log(LOG_DEBUG, "ndi_output_update(name='%s', groups='%s', ...)", name, groups);

	o->ndi_name = name;
	o->ndi_groups = groups;
	o->uses_video = obs_data_get_bool(settings, "uses_video");
	o->uses_audio = obs_data_get_bool(settings, "uses_audio");

	obs_log(LOG_INFO, "NDI Output Updated. '%s'", name);
	obs_log(LOG_DEBUG, "ndi_output_update(name='%s', groups='%s', uses_video='%s', uses_audio='%s')", name, groups,
		o->uses_video ? "true" : "false", o->uses_audio ? "true" : "false");
}

void ndi_output_stop(void *data, uint64_t)
{
	auto o = (ndi_output_t *)data;
	auto name = o->ndi_name;
	auto groups = o->ndi_groups;
	obs_log(LOG_DEBUG, "+ndi_output_stop(name='%s', groups='%s', ...)", name, groups);
	if (o->started) {
		o->started = false;

		obs_output_end_data_capture(o->output);

		if (o->ndi_sender) {
			obs_log(LOG_DEBUG, "ndi_output_stop: +ndiLib->send_destroy(o->ndi_sender)");
			pthread_mutex_lock(&o->ndi_sender_mutex);
			ndiLib->send_destroy(o->ndi_sender);
			obs_log(LOG_DEBUG, "ndi_output_stop: -ndiLib->send_destroy(o->ndi_sender)");
			o->ndi_sender = nullptr;
			pthread_mutex_unlock(&o->ndi_sender_mutex);
		}
		if (o->server_conn.running && o->server_conn.sender_created)
		{
			destroy_ndi_server(o->server_conn);
			obs_log(LOG_DEBUG, "ndi_output_stop: destroy_ndi_server");
		}
		if (o->conv_buffer) {
			delete[] o->conv_buffer;
			o->conv_buffer = nullptr;
			o->conv_function = nullptr;
		}

		o->frame_width = 0;
		o->frame_height = 0;
		o->video_framerate = 0.0;
		o->audio_channels = 0;
		o->audio_samplerate = 0;

		obs_log(LOG_INFO, "NDI Output Stopped. '%s'", name);
	}

	obs_log(LOG_DEBUG, "-ndi_output_stop(name='%s', groups='%s', ...)", name, groups);
}

void ndi_output_destroy(void *data)
{
	auto o = (ndi_output_t *)data;
	auto name = o->ndi_name;
	auto groups = o->ndi_groups;

	pthread_mutex_destroy(&o->ndi_sender_mutex);

	obs_log(LOG_DEBUG, "+ndi_output_destroy(name='%s', groups='%s', ...)", name, groups);

	if (o->audio_conv_buffer) {
		obs_log(LOG_DEBUG, "ndi_output_destroy: freeing %zu bytes", o->audio_conv_buffer_size);
		bfree(o->audio_conv_buffer);
		o->audio_conv_buffer = nullptr;
	}
	obs_log(LOG_DEBUG, "-ndi_output_destroy(name='%s', groups='%s', ...)", name, groups);
	bfree(o);
}

void ndi_output_rawvideo(void *data, video_data *frame)
{
	auto o = (ndi_output_t *)data;
	if (!o->started || !o->frame_width || !o->frame_height)
		return;

	pthread_mutex_lock(&o->ndi_sender_mutex);
	if (!o->ndi_sender && !(o->server_conn.running && o->server_conn.sender_created)) {
		pthread_mutex_unlock(&o->ndi_sender_mutex);
		return;
	}

	// Throttle calls to send_get_no_connections to at most once per second
	if (o->ndi_sender) {
		auto now = std::chrono::steady_clock::now();
		if (now - o->last_conn_check >= std::chrono::seconds(1)) {
			int nc = ndiLib->send_get_no_connections(o->ndi_sender, 10);
			o->last_conn_check = now;

			if (nc != o->no_connections) {
				auto ndi_source = ndiLib->send_get_source_name(o->ndi_sender);
				if (nc <= 0)
					obs_log(LOG_DEBUG, "NDI Output video '%s' has no connections.",
						ndi_source->p_ndi_name);
				else if (o->no_connections == 0)
					obs_log(LOG_DEBUG, "NDI Output video '%s' has %d connections.",
						ndi_source->p_ndi_name, nc);
				o->no_connections = nc;
			}
		}
	}

	pthread_mutex_unlock(&o->ndi_sender_mutex);

	uint32_t width = o->frame_width;
	uint32_t height = o->frame_height;

	NDIlib_video_frame_v2_t video_frame = {0};
	video_frame.xres = width;
	video_frame.yres = height;
	video_frame.frame_rate_N = (int)(o->video_framerate * 100);
	// TODO fixme: broken on fractional framerates
	video_frame.frame_rate_D =
		100; // TODO : investigate if there is a better way to get both _D & _N set to the proper framerate from OBS output.
	video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
	video_frame.timecode = NDIlib_send_timecode_synthesize;
	video_frame.FourCC = o->frame_fourcc;

	if (video_frame.FourCC == NDIlib_FourCC_type_UYVY) {
		o->conv_function(frame->data, frame->linesize, 0, height, o->conv_buffer, o->conv_linesize);
		video_frame.p_data = o->conv_buffer;
		video_frame.line_stride_in_bytes = o->conv_linesize;
	} else {
		video_frame.p_data = frame->data[0];
		video_frame.line_stride_in_bytes = frame->linesize[0];
	}

	if (o->ndi_sender) {
		ndiLib->send_send_video_async_v2(o->ndi_sender, &video_frame);
	}
	if (o->server_conn.running && o->server_conn.sender_created) {
		serialize_frame(NDIlib_frame_type_video, (const void *)&video_frame, o->server_conn.pReq->payload,
				sizeof(o->server_conn.pReq->payload),true);
		NDIlib_frame_type_e frame_received = NDIlib_frame_type_none;
		NDIlib_video_frame_v2_t video_frame_test;
		NDIlib_audio_frame_v3_t audio_frame_test;
		uint8_t *in_buf = o->server_conn.pReq->payload;
		size_t frame_len = deserialize_frame(in_buf, sizeof(o->server_conn.pReq->payload), frame_received,
						     &video_frame_test,
						     &audio_frame_test, true);
		if (frame_received != NDIlib_frame_type_video ||
		    video_frame_test.xres != video_frame.xres || video_frame_test.yres != video_frame.yres) {
			printf("Video deserialization failed or resolution mismatch (received type %d, res %dx%d, expected type %d, res %dx%d)\n",
			       frame_received, video_frame_test.xres, video_frame_test.yres, NDIlib_frame_type_video,
			       video_frame.xres, video_frame.yres);
		}
		o->server_conn.pReq->command = NDI_SEND_VIDEO_FRAME;
		SetEvent(o->server_conn.hEvtCmd);

		DWORD w = WaitForSingleObject(o->server_conn.hEvtRsp, NDI_SERVER_WAIT);
		if (w != WAIT_OBJECT_0) {
			obs_log(LOG_ERROR, "Timed out waiting for ndi-server send video frame");
			destroy_ndi_server(o->server_conn);
			return;
		}
	}
}

void ndi_output_rawaudio(void *data, audio_data *frame)
{
	// NOTE: The logic in this function should be similar to
	// ndi-filter.cpp/ndi_filter_asyncaudio(...)
	auto o = (ndi_output_t *)data;
	if (!o->started || !o->audio_samplerate || !o->audio_channels)
		return;

	pthread_mutex_lock(&o->ndi_sender_mutex);
	if (!o->ndi_sender && !(o->server_conn.running && o->server_conn.sender_created)) {
		pthread_mutex_unlock(&o->ndi_sender_mutex);
		return;
	}

	if (o->ndi_sender) {
		auto now = std::chrono::steady_clock::now();
		if (now - o->last_conn_check >= std::chrono::seconds(1)) {
			o->last_conn_check = now;

			int nc = ndiLib->send_get_no_connections(o->ndi_sender, 10);

			if (nc != o->no_connections) {
				auto ndi_source = ndiLib->send_get_source_name(o->ndi_sender);
				if (nc <= 0)
					obs_log(LOG_DEBUG, "NDI Output audio '%s' has no connections.",
						ndi_source->p_ndi_name);
				else if (o->no_connections == 0)
					obs_log(LOG_DEBUG, "NDI Output audio '%s' has %d connections.",
						ndi_source->p_ndi_name, nc);
				o->no_connections = nc;
			}
		}
	}
	pthread_mutex_unlock(&o->ndi_sender_mutex);

	NDIlib_audio_frame_v3_t audio_frame = {0};
	audio_frame.sample_rate = o->audio_samplerate;
	audio_frame.no_channels = (int)o->audio_channels;
	audio_frame.timecode = NDIlib_send_timecode_synthesize;
	audio_frame.no_samples = frame->frames;
	audio_frame.channel_stride_in_bytes = frame->frames * 4;
	audio_frame.FourCC = NDIlib_FourCC_audio_type_FLTP;

	const size_t data_size = audio_frame.no_channels * audio_frame.channel_stride_in_bytes;

	if (data_size > o->audio_conv_buffer_size) {
		obs_log(LOG_DEBUG, "ndi_output_rawaudio('%s'): growing audio_conv_buffer from %zu to %zu bytes",
			o->ndi_name, o->audio_conv_buffer_size, data_size);
		if (o->audio_conv_buffer) {
			obs_log(LOG_DEBUG, "ndi_output_rawaudio('%s'): freeing %zu bytes", o->ndi_name,
				o->audio_conv_buffer_size);
			bfree(o->audio_conv_buffer);
		}
		obs_log(LOG_DEBUG, "ndi_output_rawaudio('%s'): allocating %zu bytes", o->ndi_name, data_size);
		o->audio_conv_buffer = (uint8_t *)bmalloc(data_size);
		o->audio_conv_buffer_size = data_size;
	}

	for (int i = 0; i < audio_frame.no_channels; ++i) {
		memcpy(o->audio_conv_buffer + (i * audio_frame.channel_stride_in_bytes), frame->data[i],
		       audio_frame.channel_stride_in_bytes);
	}

	audio_frame.p_data = o->audio_conv_buffer;
	if (o->ndi_sender) {
		ndiLib->send_send_audio_v3(o->ndi_sender, &audio_frame);
	}

	if (o->server_conn.running && o->server_conn.sender_created) {
		serialize_frame(NDIlib_frame_type_audio, (const void *)&audio_frame, o->server_conn.pReqA->payload,
				sizeof(o->server_conn.pReqA->payload), true);
		o->server_conn.pReqA->command = NDI_SEND_AUDIO_FRAME;
		SetEvent(o->server_conn.hEvtCmdA);
		DWORD w = WaitForSingleObject(o->server_conn.hEvtRspA, NDI_SERVER_WAIT);
		if (w != WAIT_OBJECT_0) {
			obs_log(LOG_ERROR, "Timed out waiting for ndi-server send audio");
			destroy_ndi_server(o->server_conn);
			return;
		}
	}
}

obs_output_info create_ndi_output_info()
{
	obs_output_info ndi_output_info = {};
	ndi_output_info.id = "ndi_output";
	ndi_output_info.flags = OBS_OUTPUT_AV;

	ndi_output_info.get_name = ndi_output_getname;
	ndi_output_info.get_properties = ndi_output_getproperties;
	ndi_output_info.get_defaults = ndi_output_getdefaults;

	ndi_output_info.create = ndi_output_create;
	ndi_output_info.start = ndi_output_start;
	ndi_output_info.update = ndi_output_update;
	ndi_output_info.stop = ndi_output_stop;
	ndi_output_info.destroy = ndi_output_destroy;

	ndi_output_info.raw_video = ndi_output_rawvideo;
	ndi_output_info.raw_audio = ndi_output_rawaudio;

	return ndi_output_info;
}
