/******************************************************************************
	Copyright (C) 2016-2026 DistroAV <contact@distroav.org>

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

#include "ndi-receiver-backend.h"
#include "plugin-main.h"
#include "ndi-shared.h" // shared-memory protocol + NDIlib_recv_create_v3_t (de)serialization

#include <sstream>
#include <thread>

namespace {

// ---------------------------------------------------------------------------
// DirectNdiReceiverBackend: calls NDIlib directly, in-process. This is
// exactly what ndi-source.cpp used to do inline before it had any notion of
// a server.
// ---------------------------------------------------------------------------
class DirectNdiReceiverBackend : public NdiReceiverBackend {
public:
	bool start() override { return true; }

	void stop() override { destroyReceiver(); }

	bool createReceiver(const NDIlib_recv_create_v3_t &recv_desc, bool use_framesync) override
	{
		destroyReceiver();

		m_receiver = ndiLib->recv_create_v3(&recv_desc);
		if (!m_receiver)
			return false;

		if (use_framesync) {
			m_frame_sync = ndiLib->framesync_create(m_receiver);
			if (!m_frame_sync)
				return false;
		}
		return true;
	}

	void destroyReceiver() override
	{
		if (m_frame_sync) {
			ndiLib->framesync_destroy(m_frame_sync);
			m_frame_sync = nullptr;
		}
		if (m_receiver) {
			ndiLib->recv_destroy(m_receiver);
			m_receiver = nullptr;
		}
	}

	bool hasConnections() override { return m_receiver && ndiLib->recv_get_no_connections(m_receiver) > 0; }

	void setHardwareAcceleration(bool enabled) override
	{
		if (!enabled || !m_receiver)
			return;
		NDIlib_metadata_frame_t hwAccelMetadata;
		hwAccelMetadata.p_data = (char *)"<ndi_video_codec type=\"hardware\"/>";
		ndiLib->recv_send_metadata(m_receiver, &hwAccelMetadata);
	}

	void setTally(const NDIlib_tally_t &tally) override
	{
		if (m_receiver)
			ndiLib->recv_set_tally(m_receiver, &tally);
	}

	bool ptzIsSupported() override { return m_receiver && ndiLib->recv_ptz_is_supported(m_receiver); }

	void ptzPanTilt(float pan, float tilt) override
	{
		if (m_receiver)
			ndiLib->recv_ptz_pan_tilt(m_receiver, pan, tilt);
	}

	void ptzZoom(float zoom) override
	{
		if (m_receiver)
			ndiLib->recv_ptz_zoom(m_receiver, zoom);
	}

	bool getPerformanceAndQueue(NDIlib_recv_performance_t &total, NDIlib_recv_performance_t &dropped,
				    NDIlib_recv_queue_t &queue) override
	{
		if (!m_receiver)
			return false;
		ndiLib->recv_get_performance(m_receiver, &total, &dropped);
		ndiLib->recv_get_queue(m_receiver, &queue);
		return true;
	}

	NdiCaptureResult captureFrame(NDIlib_video_frame_v2_t &video, NDIlib_audio_frame_v3_t &audio,
				      uint32_t timeout_ms) override
	{
		if (!m_receiver)
			return NdiCaptureResult::Error;

		switch (ndiLib->recv_capture_v3(m_receiver, &video, &audio, nullptr, timeout_ms)) {
		case NDIlib_frame_type_audio:
			return NdiCaptureResult::Audio;
		case NDIlib_frame_type_video:
			return NdiCaptureResult::Video;
		default:
			return NdiCaptureResult::None;
		}
	}

	void releaseVideoFrame(NDIlib_video_frame_v2_t &video) override
	{
		if (m_receiver)
			ndiLib->recv_free_video_v2(m_receiver, &video);
	}

	void releaseAudioFrame(NDIlib_audio_frame_v3_t &audio) override
	{
		if (m_receiver)
			ndiLib->recv_free_audio_v3(m_receiver, &audio);
	}

	bool captureFrameSync(NDIlib_video_frame_v2_t &video, NDIlib_audio_frame_v3_t &audio) override
	{
		if (!m_frame_sync)
			return false;

		audio = {};
		ndiLib->framesync_capture_audio_v2(m_frame_sync, &audio,
						   0,     // "The desired sample rate. 0 to get the source value."
						   0,     // "The desired channel count. 0 to get the source value."
						   1024); // "The desired sample count. 0 to get the source value."

		video = {};
		ndiLib->framesync_capture_video(m_frame_sync, &video, NDIlib_frame_format_type_progressive);
		return true;
	}

	void releaseFrameSyncVideo(NDIlib_video_frame_v2_t &video) override
	{
		if (m_frame_sync)
			ndiLib->framesync_free_video(m_frame_sync, &video);
	}

	void releaseFrameSyncAudio(NDIlib_audio_frame_v3_t &audio) override
	{
		if (m_frame_sync)
			ndiLib->framesync_free_audio_v2(m_frame_sync, &audio);
	}

private:
	NDIlib_recv_instance_t m_receiver = nullptr;
	NDIlib_framesync_instance_t m_frame_sync = nullptr;
};

// ---------------------------------------------------------------------------
// ServerNdiReceiverBackend: proxies the same operations to a separate
// ndi-server.exe helper process over shared memory (see ndi-shared.h). The
// server owns the real NDIlib_recv_instance_t/NDIlib_framesync_instance_t;
// this class only ever talks the RequestBlock/ResponseBlock protocol.
// ---------------------------------------------------------------------------
class ServerNdiReceiverBackend : public NdiReceiverBackend {
public:
	~ServerNdiReceiverBackend() override { stop(); }

	bool start() override
	{
		const WCHAR *memPrefix = NDI_MEM_NAME_PREFIX;

		// Unique ID so multiple sources' ndi-server instances don't collide.
		std::wostringstream uniqueID;
		uniqueID << std::this_thread::get_id() << GetCurrentProcessId();
		std::wstring ustr = uniqueID.str();

		WCHAR connectionName[256] = {0};
		WCHAR requestShmName[256] = {0};
		WCHAR responseShmName[256] = {0};
		WCHAR commandEventName[256] = {0};
		WCHAR readyEventName[256] = {0};
		WCHAR responseEventName[256] = {0};

		swprintf_s(connectionName, 256, L"%s%s", memPrefix, ustr.c_str());
		swprintf_s(requestShmName, 256, L"%s%s", connectionName, NDI_REQUEST_SHM_SUFFIX);
		swprintf_s(responseShmName, 256, L"%s%s", connectionName, NDI_RESPONSE_SHM_SUFFIX);
		swprintf_s(commandEventName, 256, L"%s%s", connectionName, NDI_COMMAND_EVENT_SUFFIX);
		swprintf_s(readyEventName, 256, L"%s%s", connectionName, NDI_READY_EVENT_SUFFIX);
		swprintf_s(responseEventName, 256, L"%s%s", connectionName, NDI_RESPONSE_EVENT_SUFFIX);

		// 1. Create shared-memory file mappings backed by the system pagefile.
		m_hShmReq = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RequestBlock),
					       requestShmName);
		if (!m_hShmReq || GetLastError() == ERROR_ALREADY_EXISTS) {
			obs_log(LOG_ERROR, "CreateFileMapping(request) - is another instance already running?");
			return fail();
		}

		const DWORD rspHi = (DWORD)(((UINT64)sizeof(ResponseBlock)) >> 32);
		const DWORD rspLo = (DWORD)((UINT64)sizeof(ResponseBlock) & 0xFFFFFFFFu);
		m_hShmRsp = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, rspHi, rspLo,
					       responseShmName);
		if (!m_hShmRsp || GetLastError() == ERROR_ALREADY_EXISTS) {
			obs_log(LOG_ERROR, "CreateFileMapping(response) - is another instance already running?");
			return fail();
		}

		// 2. Auto-reset named events for command/response signalling.
		m_hEvtCmd = CreateEventW(nullptr, FALSE, FALSE, commandEventName);
		if (!m_hEvtCmd) {
			obs_log(LOG_ERROR, "CreateEvent(cmd)");
			return fail();
		}

		m_hEvtRsp = CreateEventW(nullptr, FALSE, FALSE, responseEventName);
		if (!m_hEvtRsp) {
			obs_log(LOG_ERROR, "CreateEvent(rsp)");
			return fail();
		}

		// Manual-reset so WaitForSingleObject still sees it if the server
		// already signalled ready before we get here.
		m_hEvtReady = CreateEventW(nullptr, TRUE, FALSE, readyEventName);
		if (!m_hEvtReady) {
			obs_log(LOG_ERROR, "CreateEvent(ready)");
			return fail();
		}

		// 3. Map views.
		m_pReq = static_cast<RequestBlock *>(MapViewOfFile(m_hShmReq, FILE_MAP_WRITE, 0, 0, sizeof(RequestBlock)));
		if (!m_pReq) {
			obs_log(LOG_ERROR, "MapViewOfFile(request)");
			return fail();
		}

		m_pRsp = static_cast<ResponseBlock *>(
			MapViewOfFile(m_hShmRsp, FILE_MAP_READ, 0, 0, sizeof(ResponseBlock)));
		if (!m_pRsp) {
			obs_log(LOG_ERROR, "MapViewOfFile(response)");
			return fail();
		}

		// 4. Write our PID so the server can monitor us and self-terminate
		// if we ever disappear without a clean shutdown.
		memset(m_pReq, 0, sizeof(RequestBlock));
		m_pReq->client_pid = GetCurrentProcessId();

		// 5. Spawn ndi-server.exe from next to this module.
		WCHAR exePath[MAX_PATH] = {0};
		HMODULE hMod = NULL;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
						GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(reinterpret_cast<LPVOID>(&ndi_receiver_backend_module_marker)),
					&hMod)) {
			WCHAR modulePath[MAX_PATH] = {0};
			if (GetModuleFileNameW(hMod, modulePath, MAX_PATH) > 0) {
				WCHAR *last = wcsrchr(modulePath, L'\\');
				if (last) {
					*(last + 1) = L'\0';
					swprintf_s(exePath, MAX_PATH, L"%sndi-server.exe", modulePath);
				}
			}
		}
		if (exePath[0] == L'\0')
			swprintf_s(exePath, MAX_PATH, L"ndi-server.exe");

		DWORD attrs = GetFileAttributesW(exePath);
		if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			obs_log(LOG_ERROR, "Missing ndi-server.exe at expected path: %ls", exePath);
			return fail();
		}

		WCHAR cmdLine[512];
		swprintf_s(cmdLine, 512, L"\"%s\" \"%s\" ", exePath, ustr.c_str());

		STARTUPINFOW si = {sizeof(si)};
		if (!NDI_SERVER_DEBUG) {
			si.dwFlags |= STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
		}
		DWORD creationFlags = NDI_SERVER_DEBUG ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;

		obs_log(LOG_INFO, "Spawning ndi-server with command line: %ls", cmdLine);
		if (!CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE, creationFlags, nullptr, nullptr, &si,
				    &m_pi)) {
			obs_log(LOG_INFO,
				"CreateProcess(ndi-server.exe) failed - make sure ndi-server.exe is next to distroav.dll");
			return fail();
		}
		CloseHandle(m_pi.hThread);
		m_pi.hThread = nullptr;

		obs_log(LOG_INFO, "[Client] Server PID: %u. Waiting for ready signal (up to %d ms)...", m_pi.dwProcessId,
			NDI_SERVER_WAIT);

		// 6. Wait for the server to finish pre-faulting and signal ready.
		if (WaitForSingleObject(m_hEvtReady, NDI_SERVER_WAIT) != WAIT_OBJECT_0) {
			obs_log(LOG_INFO, "[Client] ndi-server did not signal ready in time.");
			return fail();
		}
		obs_log(LOG_INFO, "[Client] ndi-server is ready.");

		// 7. Pre-fault our read view of the (large) response buffer.
		PrefaultRegionRead(m_pRsp, sizeof(ResponseBlock));

		m_running = true;
		return true;
	}

	void stop() override
	{
		if (m_running && m_pReq && m_hEvtCmd) {
			m_pReq->command = NDI_SHUTDOWN;
			SetEvent(m_hEvtCmd);
			WaitForSingleObject(m_hEvtRsp, NDI_SERVER_WAIT);
		}

		if (m_pReq)
			UnmapViewOfFile(m_pReq);
		if (m_pRsp)
			UnmapViewOfFile(m_pRsp);
		if (m_hShmReq)
			CloseHandle(m_hShmReq);
		if (m_hShmRsp)
			CloseHandle(m_hShmRsp);
		if (m_hEvtCmd)
			CloseHandle(m_hEvtCmd);
		if (m_hEvtRsp)
			CloseHandle(m_hEvtRsp);
		if (m_hEvtReady)
			CloseHandle(m_hEvtReady);
		if (m_pi.hProcess)
			CloseHandle(m_pi.hProcess);
		if (m_pi.hThread)
			CloseHandle(m_pi.hThread);

		m_pReq = nullptr;
		m_pRsp = nullptr;
		m_hShmReq = m_hShmRsp = m_hEvtCmd = m_hEvtRsp = m_hEvtReady = nullptr;
		m_pi = {};
		m_running = false;
		m_receiver_created = false;
	}

	bool createReceiver(const NDIlib_recv_create_v3_t &recv_desc, bool /*use_framesync*/) override
	{
		// The server always maintains its own frame-sync object alongside
		// the receiver (see NDI_CREATE_RECEIVER in ndi-server.cpp), so
		// use_framesync doesn't need to be communicated separately here.
		if (!m_running)
			return false;

		size_t out_written = 0;
		serialize_recv_desc(recv_desc, &m_pReq->payload, sizeof(m_pReq->payload), out_written);

		m_pReq->command = NDI_CREATE_RECEIVER;
		SetEvent(m_hEvtCmd);
		if (WaitForSingleObject(m_hEvtRsp, NDI_SERVER_WAIT) != WAIT_OBJECT_0) {
			obs_log(LOG_ERROR, "Timed out waiting for ndi-server to create a receiver");
			m_receiver_created = false;
			return false;
		}

		m_receiver_created = true;
		return true;
	}

	// Nothing to do here: the server tears down/replaces its receiver the
	// next time createReceiver() sends NDI_CREATE_RECEIVER, and stop()
	// covers the "we're done entirely" case.
	void destroyReceiver() override {}

	// The server only reports "no connections" as part of a capture
	// response (NDI_NO_FRAME); there's no separate cheap query for it.
	bool hasConnections() override { return true; }

	void setHardwareAcceleration(bool enabled) override
	{
		if (!enabled || !m_receiver_created)
			return;
		m_pReq->command = NDI_HARDWARE_ACCELERATION;
		SetEvent(m_hEvtCmd);
		WaitForSingleObject(m_hEvtRsp, NDI_SERVER_WAIT);
	}

	// Not yet implemented on the server side.
	void setTally(const NDIlib_tally_t &) override {}
	bool ptzIsSupported() override { return false; }
	void ptzPanTilt(float, float) override {}
	void ptzZoom(float) override {}

	// Not yet implemented on the server side: the receiver lives in
	// ndi-server.exe, so there's no local NDIlib_recv_instance_t to query.
	bool getPerformanceAndQueue(NDIlib_recv_performance_t &, NDIlib_recv_performance_t &,
				    NDIlib_recv_queue_t &) override
	{
		return false;
	}

	NdiCaptureResult captureFrame(NDIlib_video_frame_v2_t &video, NDIlib_audio_frame_v3_t &audio,
				      uint32_t /*timeout_ms*/) override
	{
		if (!m_receiver_created)
			return NdiCaptureResult::Error;

		m_pReq->command = NDI_CAPTURE_FRAME;
		SetEvent(m_hEvtCmd);
		if (WaitForSingleObject(m_hEvtRsp, NDI_SERVER_WAIT) != WAIT_OBJECT_0) {
			obs_log(LOG_ERROR, "Timed out waiting for ndi-server to return a frame");
			return NdiCaptureResult::Error;
		}

		if (m_pRsp->payload_type == NDI_NO_FRAME)
			return NdiCaptureResult::None;

		NDIlib_frame_type_e received = NDIlib_frame_type_none;
		deserialize_frame(m_pRsp->payload, sizeof(m_pRsp->payload), received, &video, &audio);

		if (received == NDIlib_frame_type_audio) {
			audio.p_data = m_pRsp->payload + AUDIO_FRAME_OFFSET;
			return NdiCaptureResult::Audio;
		}
		if (received == NDIlib_frame_type_video) {
			video.p_data = m_pRsp->payload + VIDEO_FRAME_OFFSET;
			return NdiCaptureResult::Video;
		}
		return NdiCaptureResult::None;
	}

	// Frames live in the shared-memory response view for as long as it's
	// mapped; there's no per-frame ownership to hand back to the server.
	void releaseVideoFrame(NDIlib_video_frame_v2_t &) override {}
	void releaseAudioFrame(NDIlib_audio_frame_v3_t &) override {}

	bool captureFrameSync(NDIlib_video_frame_v2_t &video, NDIlib_audio_frame_v3_t &audio) override
	{
		if (!m_receiver_created)
			return false;

		m_pReq->command = NDI_CAPTURE_FRAME_SYNC;
		SetEvent(m_hEvtCmd);
		if (WaitForSingleObject(m_hEvtRsp, NDI_SERVER_WAIT) != WAIT_OBJECT_0) {
			obs_log(LOG_ERROR, "Timed out waiting for ndi-server to return a frame-sync frame");
			return false;
		}

		if (m_pRsp->payload_type == NDI_NO_FRAME) {
			video.p_data = nullptr;
			audio.p_data = nullptr;
			return true;
		}

		NDIlib_frame_type_e received = NDIlib_frame_type_none;
		uint8_t *in_buf = m_pRsp->payload;
		size_t frame_len = deserialize_frame(in_buf, sizeof(m_pRsp->payload), received, &video, &audio);
		in_buf += frame_len;
		deserialize_frame(in_buf, sizeof(m_pRsp->payload) - frame_len, received, &video, &audio);

		audio.p_data = m_pRsp->payload + AUDIO_FRAME_OFFSET;
		video.p_data = m_pRsp->payload + VIDEO_FRAME_OFFSET;
		return true;
	}

	void releaseFrameSyncVideo(NDIlib_video_frame_v2_t &) override {}
	void releaseFrameSyncAudio(NDIlib_audio_frame_v3_t &) override {}

private:
	bool fail()
	{
		stop();
		return false;
	}

	// Used only as a stable address to resolve this module's own HMODULE
	// via GetModuleHandleExW (so the ndi-server.exe path can be built
	// relative to wherever distroav.dll actually is).
	static void ndi_receiver_backend_module_marker() {}

	HANDLE m_hShmReq = nullptr;
	HANDLE m_hShmRsp = nullptr;
	HANDLE m_hEvtCmd = nullptr;
	HANDLE m_hEvtRsp = nullptr;
	HANDLE m_hEvtReady = nullptr;
	RequestBlock *m_pReq = nullptr;
	ResponseBlock *m_pRsp = nullptr;
	PROCESS_INFORMATION m_pi = {};
	bool m_running = false;
	bool m_receiver_created = false;
};

} // namespace

std::unique_ptr<NdiReceiverBackend> create_ndi_receiver_backend()
{
	if (Config::UseNdiServer) {
		auto server = std::make_unique<ServerNdiReceiverBackend>();
		if (server->start())
			return server;
		obs_log(LOG_WARNING, "ndi-server unavailable; falling back to direct NDI SDK calls");
	}

	auto direct = std::make_unique<DirectNdiReceiverBackend>();
	direct->start();
	return direct;
}
