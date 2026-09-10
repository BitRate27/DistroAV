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

#pragma once

// NdiReceiverBackend abstracts *how* frames are pulled from an NDI source:
// either by calling the NDI SDK (NDIlib) directly in this process, or by
// proxying the same operations to a separate ndi-server.exe helper process
// over shared memory (see ndi-shared.h / ndi-receiver-backend.cpp).
//
// ndi-source.cpp talks only to this interface via a single
// std::unique_ptr<NdiReceiverBackend> obtained from create_ndi_receiver_backend().
// It never touches NDIlib_recv_instance_t/NDIlib_framesync_instance_t or the
// shared-memory protocol directly, and never needs to branch on which backend
// is active - that choice (Config::UseNdiServer) is resolved once, inside the
// factory, and everything after that is a uniform virtual call.

#include <Processing.NDI.Lib.h>

#include <cstdint>
#include <memory>

// Result of a single (non-frame-sync) capture attempt.
enum class NdiCaptureResult {
	None,  // no new data this call (e.g. a timeout, or no active connection)
	Audio, // audio was filled in - call releaseAudioFrame() once done with it
	Video, // video was filled in - call releaseVideoFrame() once done with it
	Error, // the receiver is no longer usable and should be recreated
};

class NdiReceiverBackend {
public:
	virtual ~NdiReceiverBackend() = default;

	// One-time setup/teardown for whatever this backend needs before a
	// receiver can be created (e.g. spawning ndi-server.exe). Returns false
	// if the backend could not be started at all.
	virtual bool start() = 0;
	virtual void stop() = 0;

	// (Re)creates the receiver for the given description, tearing down any
	// previous receiver/frame-sync state first. Returns false on failure.
	virtual bool createReceiver(const NDIlib_recv_create_v3_t &recv_desc, bool use_framesync) = 0;
	virtual void destroyReceiver() = 0;

	// Whether the current receiver appears to have an active connection.
	// Backends that can't cheaply answer this out-of-band (the server
	// backend only learns it from a capture response) always return true
	// here; callers fall back to NdiCaptureResult::None from the capture
	// calls to detect "nothing is connected" in that case.
	virtual bool hasConnections() = 0;

	// Best-effort request; not every backend can honor every one of these.
	virtual void setHardwareAcceleration(bool enabled) = 0;
	virtual void setTally(const NDIlib_tally_t &tally) = 0;
	virtual bool ptzIsSupported() = 0;
	virtual void ptzPanTilt(float pan, float tilt) = 0;
	virtual void ptzZoom(float zoom) = 0;

	// Best-effort query of the NDI SDK's own performance/queue counters for
	// the current receiver (dropped frame/sample counts, queued frame
	// counts). Returns false if there is no active receiver, or the backend
	// can't answer this out-of-band (the server backend's receiver lives in
	// another process; not yet implemented there). Callers should leave
	// their stats unchanged when this returns false.
	virtual bool getPerformanceAndQueue(NDIlib_recv_performance_t &total, NDIlib_recv_performance_t &dropped,
					    NDIlib_recv_queue_t &queue) = 0;

	// Blocking capture of a single frame (non-frame-sync mode).
	virtual NdiCaptureResult captureFrame(NDIlib_video_frame_v2_t &video, NDIlib_audio_frame_v3_t &audio,
					      uint32_t timeout_ms) = 0;
	virtual void releaseVideoFrame(NDIlib_video_frame_v2_t &video) = 0;
	virtual void releaseAudioFrame(NDIlib_audio_frame_v3_t &audio) = 0;

	// Frame-sync mode: always fills in both video and audio with whatever
	// is currently available; the caller compares frame timestamps against
	// the last ones it processed to tell whether either is actually new.
	// Returns false if the receiver is no longer usable and should be
	// recreated (video/audio are left untouched in that case).
	virtual bool captureFrameSync(NDIlib_video_frame_v2_t &video, NDIlib_audio_frame_v3_t &audio) = 0;
	virtual void releaseFrameSyncVideo(NDIlib_video_frame_v2_t &video) = 0;
	virtual void releaseFrameSyncAudio(NDIlib_audio_frame_v3_t &audio) = 0;
};

// Creates the backend selected by Config::UseNdiServer, already started.
// If the server backend is selected but fails to start (helper process
// missing, timed out, etc.), transparently falls back to the direct backend
// so the caller always gets back a usable instance.
std::unique_ptr<NdiReceiverBackend> create_ndi_receiver_backend();
