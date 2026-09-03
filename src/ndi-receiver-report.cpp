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

#include "ndi-receiver-report.h"
#include "plugin-main.h" // provides extern ndiLib and network_monitor

#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>

static int64_t signed_delta(uint64_t current, uint64_t previous)
{
	if (current >= previous) {
		uint64_t d = current - previous;
		return d > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(d);
	}
	uint64_t d = previous - current;
	return d > static_cast<uint64_t>(INT64_MAX) ? INT64_MIN : -static_cast<int64_t>(d);
}

static int64_t projected_relation(uint64_t a_ts_ns, uint64_t a_wall_ns, uint64_t b_ts_ns, uint64_t b_wall_ns)
{
	if (!a_ts_ns || !a_wall_ns || !b_ts_ns || !b_wall_ns)
		return 0;
	return signed_delta(a_ts_ns, b_ts_ns) + signed_delta(b_wall_ns, a_wall_ns);
}

// Constructor
ReceiverInfo::ReceiverInfo(obs_source_t *ndi_source)
	: m_source(ndi_source),
	  m_receiver(nullptr),
	  ndi_name(""),
	  snapshot(),
	  running()
{
}

// ChangeNotifier registration
void ReceiverInfo::registerChangeNotifier(ChangeNotifier *notifier, const std::string &key)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!notifier || key.empty())
		return;
	changeNotifiers[key] = notifier;
}

void ReceiverInfo::unregisterChangeNotifier(ChangeNotifier *notifier, const std::string &key)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = changeNotifiers.find(key);
	if (it == changeNotifiers.end())
		return;
	if (notifier == nullptr || it->second == notifier) {
		changeNotifiers.erase(it);
	}
}

void ReceiverInfo::notify_change()
{
	// Copy notifiers under lock, then emit outside lock.
	std::vector<ChangeNotifier *> notifiers;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		notifiers.reserve(changeNotifiers.size());
		for (auto &p : changeNotifiers)
			notifiers.push_back(p.second);
	}
	for (auto *n : notifiers) {
		if (n)
			n->emitChanged();
	}
}

// NDI name helpers
void ReceiverInfo::set_ndi_name(const std::string &name)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	ndi_name = name;
}

std::string ReceiverInfo::get_ndi_name() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return ndi_name;
}

// Snapshot accessor
NDIReceiverStats ReceiverInfo::getReportSnapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return snapshot;
}

void ReceiverInfo::set_receiver(NDIlib_recv_instance_t receiver)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_receiver = receiver;
}

// Short textual status for logging/debug UI.
std::string ReceiverInfo::get_brief_receiver_status() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_receiver == nullptr) {
		return "Receiver: Not active";
	}

	std::string status = "Video: ";
	if (snapshot.osFPS > 0.0) {
		status += "Active";
		status += ", Format: " + network_monitor->getFormatDescription(snapshot.xres, snapshot.yres,
									       snapshot.frame_rate_D,
									       snapshot.frame_rate_N, snapshot.fourcc);
	} else {
		status += "Inactive";
	}

	status += ", Audio: ";
	status += (snapshot.osSPS > 0) ? "Active" : "Inactive";
	return status;
}

std::string ReceiverInfo::get_missing_receiver_status()
{
	return "Video: Inactive, Audio: Inactive";
}

void ReceiverInfo::add_drift_sample(uint64_t wall_ns, int64_t drift_ns)
{
	if (reg_n_ == 0.0)
		reg_start_wall_ns_ = wall_ns;

	// x = seconds since the first sample, y = drift in ns.
	const double x = static_cast<double>(signed_delta(wall_ns, reg_start_wall_ns_)) / 1e9;
	const double y = static_cast<double>(drift_ns);

	reg_n_ += 1.0;
	reg_sum_x_ += x;
	reg_sum_y_ += y;
	reg_sum_xy_ += x * y;
	reg_sum_xx_ += x * x;

	// Standard least-squares slope: (n?xy - ?x?y) / (n?xx - (?x)^2)
	const double denom = reg_n_ * reg_sum_xx_ - reg_sum_x_ * reg_sum_x_;
	double slope_ns_per_sec = 0.0;
	if (reg_n_ >= 2.0 && denom > 1e-9)
		slope_ns_per_sec = (reg_n_ * reg_sum_xy_ - reg_sum_x_ * reg_sum_y_) / denom;

	running.av_drift_ns_per_hour = slope_ns_per_sec * 3600.0;
}
// Frame callbacks
void ReceiverInfo::receive_video_frame(uint64_t t0, uint64_t t1, uint64_t t2, NDIlib_video_frame_v2_t &ndi_frame)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (running.video_frame_count == 0) {
		running.start_video_frame_os = t1;
		running.start_video_frame_timestamp = ndi_frame.timestamp * 100ULL;
		running.tot_frame_interval = 0;
		running.max_frame_interval = 0;
		running.fourcc = ndi_frame.FourCC;
		running.xres = ndi_frame.xres;
		running.yres = ndi_frame.yres;
		running.frame_rate_D = ndi_frame.frame_rate_D;
		running.frame_rate_N = ndi_frame.frame_rate_N;
		running.tot_video_capture = 0;
		running.max_video_capture = 0;
		running.tot_video_processing = 0;
		running.max_video_processing = 0;
	}

	if (running.last_video_frame_timestamp != 0) {
		uint64_t frame_interval = (ndi_frame.timestamp * 100ULL) - running.last_video_frame_timestamp;
		running.tot_frame_interval += frame_interval;
		running.max_frame_interval = std::max<uint64_t>(running.max_frame_interval, frame_interval);
	}

	running.last_video_frame_timestamp = ndi_frame.timestamp * 100ULL;
	running.target_fps = (double)ndi_frame.frame_rate_N / (double)ndi_frame.frame_rate_D;

	uint64_t capture_interval = t1 - t0;
	running.tot_video_capture += capture_interval;
	running.max_video_capture = std::max<uint64_t>(running.max_video_capture, capture_interval);

	uint64_t processing_interval = t2 - t1;
	running.tot_video_processing += processing_interval;
	running.max_video_processing = std::max<uint64_t>(running.max_video_processing, processing_interval);

	running.last_video_frame_os = t1;
	++running.video_frame_count;

	if (ndi_frame.timestamp != NDIlib_recv_timestamp_undefined) {
		const uint64_t video_ts_ns = static_cast<uint64_t>(ndi_frame.timestamp) * 100ULL;
		const uint64_t video_wall_ns = t1;

		if (last_audio_ts_ns_ && last_audio_wall_ns_) {
			const int64_t drift =
				projected_relation(video_ts_ns, video_wall_ns, last_audio_ts_ns_, last_audio_wall_ns_);
			running.av_drift_ns = drift;
			add_drift_sample(video_wall_ns, drift);
			if (!have_av_drift_) {
				running.av_drift_min_ns = drift;
				running.av_drift_max_ns = drift;
				have_av_drift_ = true;
			} else {
				running.av_drift_min_ns = std::min<int64_t>(running.av_drift_min_ns, drift);
				running.av_drift_max_ns = std::max<int64_t>(running.av_drift_max_ns, drift);
			}
		}
		last_video_ts_ns_ = video_ts_ns;
		last_video_wall_ns_ = video_wall_ns;
	}
}

void ReceiverInfo::receive_audio_frame(uint64_t t0, uint64_t t1, uint64_t, NDIlib_audio_frame_v3_t &ndi_frame)
{
	if (running.audio_sample_count == 0) {
		running.start_audio_sample_os = t0;
		running.start_audio_sample_timestamp = ndi_frame.timestamp * 100ULL;
	}
	running.last_audio_sample_timestamp = ndi_frame.timestamp * 100ULL;
	running.last_audio_sample_os = t0;
	running.target_sps = (double)ndi_frame.sample_rate;
	running.audio_sample_count += ndi_frame.no_samples;

	if (ndi_frame.timestamp != NDIlib_recv_timestamp_undefined) {
		const uint64_t audio_ts_ns = static_cast<uint64_t>(ndi_frame.timestamp) * 100ULL;
		const uint64_t audio_wall_ns = t1;

		if (last_video_ts_ns_ && last_video_wall_ns_) {
			const int64_t drift =
				projected_relation(last_video_ts_ns_, last_video_wall_ns_, audio_ts_ns, audio_wall_ns);
			running.av_drift_ns = drift;
			add_drift_sample(audio_wall_ns, drift);
			if (!have_av_drift_) {
				running.av_drift_min_ns = drift;
				running.av_drift_max_ns = drift;
				have_av_drift_ = true;
			} else {
				running.av_drift_min_ns = std::min<int64_t>(running.av_drift_min_ns, drift);
				running.av_drift_max_ns = std::max<int64_t>(running.av_drift_max_ns, drift);
			}
		}
		last_audio_ts_ns_ = audio_ts_ns;
		last_audio_wall_ns_ = audio_wall_ns;
	}
}

// Calculate stats and update snapshot
void ReceiverInfo::calculate_stats()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		snapshot = running;            // Copy running stats to snapshot for reporting
		running.video_frame_count = 0; // Reset running frame count after snapshot
		running.audio_sample_count = 0;
	}

	snapshot.osFPS = 0.0;
	snapshot.tsFPS = 0.0;
	if (snapshot.video_frame_count > 0) {
		double elapsedSeconds = (double)(snapshot.last_video_frame_os - snapshot.start_video_frame_os) / 1e9;
		if (elapsedSeconds > 0.0) {
			double dfps = (double)snapshot.video_frame_count / elapsedSeconds;
			snapshot.osFPS = dfps;
		}

		elapsedSeconds =
			(double)(snapshot.last_video_frame_timestamp - snapshot.start_video_frame_timestamp) / 1e9;
		if (elapsedSeconds > 0.0) {
			double dfps = (double)snapshot.video_frame_count / elapsedSeconds;
			snapshot.tsFPS = dfps;
		}
		snapshot.avg_frame_interval =
			snapshot.video_frame_count > 0 ? snapshot.tot_frame_interval / snapshot.video_frame_count : 0;
		snapshot.avg_video_capture =
			snapshot.video_frame_count > 0 ? snapshot.tot_video_capture / snapshot.video_frame_count : 0;
		snapshot.avg_video_processing =
			snapshot.video_frame_count > 0 ? snapshot.tot_video_processing / snapshot.video_frame_count : 0;
	}

	if (snapshot.audio_sample_count > 0) {
		double elapsedSeconds = (double)(snapshot.last_audio_sample_os - snapshot.start_audio_sample_os) / 1e9;
		if (elapsedSeconds > 0.0) {
			double dsps = (double)snapshot.audio_sample_count / elapsedSeconds;
			snapshot.osSPS = dsps;
		}
		elapsedSeconds =
			(double)(snapshot.last_audio_sample_timestamp - snapshot.start_audio_sample_timestamp) / 1e9;
		if (elapsedSeconds > 0.0) {
			double dsps = (double)snapshot.audio_sample_count / elapsedSeconds;
			snapshot.tsSPS = dsps;
		}
	}

	snapshot.deficit_fps = 0.0;
	snapshot.budget_used_per_frame_capture = 0.0;
	snapshot.budget_used_per_frame_processing = 0.0;
	snapshot.max_capture_pct = 0.0;
	snapshot.max_process_pct = 0.0;
	if (snapshot.target_fps > 0) {
		snapshot.deficit_fps = ((snapshot.target_fps - snapshot.osFPS) / snapshot.target_fps);
		snapshot.budget_used_per_frame_capture = snapshot.avg_video_capture / (1e9 / snapshot.target_fps);
		snapshot.budget_used_per_frame_processing = snapshot.avg_video_processing / (1e9 / snapshot.target_fps);

		// Worst-case capture/processing time as a fraction of the target frame period
		// (not divided by the average - so a max that equals one full frame period at
		// the target fps always reads as 100%, regardless of how small the average is).
		snapshot.max_capture_pct = (double)snapshot.max_video_capture / (1e9 / snapshot.target_fps);
		snapshot.max_process_pct = (double)snapshot.max_video_processing / (1e9 / snapshot.target_fps);
	}

	snapshot.deficit_sps = 0.0;
	if (snapshot.target_sps > 0)
		snapshot.deficit_sps = ((snapshot.target_sps - snapshot.osSPS) / snapshot.target_sps);

	snapshot.jitter_ratio = 0.0;
	if (snapshot.avg_frame_interval > 0)
		snapshot.jitter_ratio =
			(double)snapshot.max_frame_interval /
			(double)snapshot.avg_frame_interval; // TODO: reset max_frame_interval periodically?

	snapshot.format_description = network_monitor->getFormatDescription(
		snapshot.xres, snapshot.yres, snapshot.frame_rate_D, snapshot.frame_rate_N, snapshot.fourcc);

	// Query NDI SDK for performance & queue info (if available)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (ndiLib && m_receiver != nullptr) {

			NDIlib_recv_performance_t total, dropped;
			ndiLib->recv_get_performance(m_receiver, &total, &dropped);
			snapshot.video_frames_dropped = dropped.video_frames;
			snapshot.audio_samples_dropped = dropped.audio_frames;

			NDIlib_recv_queue_t queue;
			ndiLib->recv_get_queue(m_receiver, &queue);
			snapshot.video_queue_size = queue.video_frames;
			snapshot.audio_queue_size = queue.audio_frames;
		}

		snapshot.format_description = network_monitor->getFormatDescription(
			snapshot.xres, snapshot.yres, snapshot.frame_rate_D, snapshot.frame_rate_N, snapshot.fourcc);

		snapshot.obs_source_name = obs_source_get_name(m_source);
	}
	// connections/discoverable are set by network-monitor elsewhere; we don't call it here
	// to avoid header cycles. If needed, network_monitor usage should occur in another translation unit.
}
