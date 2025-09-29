// srt_receiver.cpp
#include <iostream>
#include <memory>
#include <cstring>
#include <thread>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

class SRTReceiver {
private:
	AVFormatContext *fmtCtx = nullptr;
	// Video components
	AVCodecContext *videoCodecCtx = nullptr;
	SwsContext *swsCtx = nullptr;
	AVFrame *videoFrame = nullptr;
	AVFrame *videoFrameConverted = nullptr;
	int videoStreamIdx = -1;
	uint8_t *videoBuffer = nullptr;

	// Audio components
	AVCodecContext *audioCodecCtx = nullptr;
	SwrContext *swrCtx = nullptr;
	AVFrame *audioFrame = nullptr;
	AVFrame *audioFrameConverted = nullptr;
	int audioStreamIdx = -1;
	uint8_t **audioBuffer = nullptr;

	AVPacket *packet = nullptr;
	std::atomic<bool> running{true};

public:
	SRTReceiver() = default;

	~SRTReceiver() { cleanup(); }

	bool init(const std::string &srtUrl)
	{
		int ret;

		// Allocate format context
		fmtCtx = avformat_alloc_context();
		if (!fmtCtx) {
			std::cerr << "Failed to allocate format context" << std::endl;
			return false;
		}

		// Set SRT options for receiver
		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "protocol_whitelist", "file,udp,rtp,srt", 0);
		av_dict_set(&opts, "mode", "listener", 0);
		av_dict_set(&opts, "latency", "200000", 0);
		av_dict_set(&opts, "recv_buffer_size", "4000000", 0);

		// Open input stream
		ret = avformat_open_input(&fmtCtx, srtUrl.c_str(), nullptr, &opts);
		av_dict_free(&opts);

		if (ret < 0) {
			char errBuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errBuf, sizeof(errBuf));
			std::cerr << "Failed to open SRT stream: " << errBuf << std::endl;
			return false;
		}

		// Retrieve stream information
		ret = avformat_find_stream_info(fmtCtx, nullptr);
		if (ret < 0) {
			std::cerr << "Failed to find stream info" << std::endl;
			return false;
		}

		// Find video and audio streams
		for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
			if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIdx == -1) {
				videoStreamIdx = i;
			} else if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
				   audioStreamIdx == -1) {
				audioStreamIdx = i;
			}
		}

		if (videoStreamIdx == -1) {
			std::cerr << "No video stream found" << std::endl;
			return false;
		}

		// Initialize video decoder
		if (!initVideoDecoder()) {
			return false;
		}

		// Initialize audio decoder if audio stream exists
		if (audioStreamIdx != -1) {
			if (!initAudioDecoder()) {
				std::cerr << "Warning: Failed to initialize audio decoder" << std::endl;
			}
		}

		packet = av_packet_alloc();
		if (!packet) {
			std::cerr << "Failed to allocate packet" << std::endl;
			return false;
		}

		std::cout << "SRT Receiver initialized successfully" << std::endl;
		return true;
	}

	bool receiveFrame(AVFrame **outVideoFrame, AVFrame **outAudioFrame)
	{
		*outVideoFrame = nullptr;
		*outAudioFrame = nullptr;

		while (running) {
			int ret = av_read_frame(fmtCtx, packet);
			if (ret < 0) {
				if (ret == AVERROR_EOF) {
					std::cout << "End of stream" << std::endl;
					return false;
				}
				char errBuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errBuf, sizeof(errBuf));
				std::cerr << "Error reading frame: " << errBuf << std::endl;
				return false;
			}

			if (packet->stream_index == videoStreamIdx) {
				if (decodeVideoPacket(outVideoFrame)) {
					av_packet_unref(packet);
					return true;
				}
			} else if (packet->stream_index == audioStreamIdx && audioCodecCtx) {
				if (decodeAudioPacket(outAudioFrame)) {
					av_packet_unref(packet);
					return true;
				}
			}

			av_packet_unref(packet);
		}

		return false;
	}

	void getVideoInfo(int &width, int &height, AVPixelFormat &format, double &fps)
	{
		if (videoFrameConverted) {
			width = videoFrameConverted->width;
			height = videoFrameConverted->height;
			format = (AVPixelFormat)videoFrameConverted->format;
		}
		if (videoStreamIdx >= 0) {
			AVRational fpsRational = fmtCtx->streams[videoStreamIdx]->r_frame_rate;
			fps = av_q2d(fpsRational);
		}
	}

	void getAudioInfo(int &sampleRate, int &channels, AVSampleFormat &format)
	{
		if (audioFrameConverted && audioCodecCtx) {
			sampleRate = audioFrameConverted->sample_rate;
			channels = audioFrameConverted->ch_layout.nb_channels;
			format = (AVSampleFormat)audioFrameConverted->format;
		}
	}

	void stop() { running = false; }

private:
	bool initVideoDecoder()
	{
		AVStream *videoStream = fmtCtx->streams[videoStreamIdx];
		AVCodecParameters *codecPar = videoStream->codecpar;

		const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
		if (!codec) {
			std::cerr << "Video codec not found" << std::endl;
			return false;
		}

		videoCodecCtx = avcodec_alloc_context3(codec);
		if (!videoCodecCtx) {
			std::cerr << "Failed to allocate video codec context" << std::endl;
			return false;
		}

		if (avcodec_parameters_to_context(videoCodecCtx, codecPar) < 0) {
			std::cerr << "Failed to copy video codec parameters" << std::endl;
			return false;
		}

		if (avcodec_open2(videoCodecCtx, codec, nullptr) < 0) {
			std::cerr << "Failed to open video codec" << std::endl;
			return false;
		}

		videoFrame = av_frame_alloc();
		videoFrameConverted = av_frame_alloc();
		if (!videoFrame || !videoFrameConverted) {
			std::cerr << "Failed to allocate video frames" << std::endl;
			return false;
		}

		// Set output to 1920x1080 YUV420P
		int dstWidth = 1920;
		int dstHeight = 1080;
		AVPixelFormat dstFormat = AV_PIX_FMT_YUV420P;

		int numBytes = av_image_get_buffer_size(dstFormat, dstWidth, dstHeight, 1);
		videoBuffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
		av_image_fill_arrays(videoFrameConverted->data, videoFrameConverted->linesize, videoBuffer, dstFormat,
				     dstWidth, dstHeight, 1);

		swsCtx = sws_getContext(videoCodecCtx->width, videoCodecCtx->height, videoCodecCtx->pix_fmt, dstWidth,
					dstHeight, dstFormat, SWS_BILINEAR, nullptr, nullptr, nullptr);

		if (!swsCtx) {
			std::cerr << "Failed to initialize video scaler" << std::endl;
			return false;
		}

		videoFrameConverted->width = dstWidth;
		videoFrameConverted->height = dstHeight;
		videoFrameConverted->format = dstFormat;

		std::cout << "Video: " << dstWidth << "x" << dstHeight << " @ " << av_q2d(videoStream->r_frame_rate)
			  << " fps" << std::endl;
		std::cout << "Video format: " << av_get_pix_fmt_name(dstFormat) << std::endl;

		return true;
	}

	bool initAudioDecoder()
	{
		AVStream *audioStream = fmtCtx->streams[audioStreamIdx];
		AVCodecParameters *codecPar = audioStream->codecpar;

		const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
		if (!codec) {
			std::cerr << "Audio codec not found" << std::endl;
			return false;
		}

		audioCodecCtx = avcodec_alloc_context3(codec);
		if (!audioCodecCtx) {
			std::cerr << "Failed to allocate audio codec context" << std::endl;
			return false;
		}

		if (avcodec_parameters_to_context(audioCodecCtx, codecPar) < 0) {
			std::cerr << "Failed to copy audio codec parameters" << std::endl;
			return false;
		}

		if (avcodec_open2(audioCodecCtx, codec, nullptr) < 0) {
			std::cerr << "Failed to open audio codec" << std::endl;
			return false;
		}

		audioFrame = av_frame_alloc();
		audioFrameConverted = av_frame_alloc();
		if (!audioFrame || !audioFrameConverted) {
			std::cerr << "Failed to allocate audio frames" << std::endl;
			return false;
		}

		// Set output format to floating point planar (FLTP)
		AVSampleFormat dstFormat = AV_SAMPLE_FMT_FLTP;

		// Allocate resampler
		int ret = swr_alloc_set_opts2(&swrCtx, &audioCodecCtx->ch_layout, dstFormat, audioCodecCtx->sample_rate,
					      &audioCodecCtx->ch_layout, audioCodecCtx->sample_fmt,
					      audioCodecCtx->sample_rate, 0, nullptr);

		if (ret < 0 || !swrCtx) {
			std::cerr << "Failed to allocate audio resampler" << std::endl;
			return false;
		}

		if (swr_init(swrCtx) < 0) {
			std::cerr << "Failed to initialize audio resampler" << std::endl;
			return false;
		}

		audioFrameConverted->format = dstFormat;
		audioFrameConverted->sample_rate = audioCodecCtx->sample_rate;
		av_channel_layout_copy(&audioFrameConverted->ch_layout, &audioCodecCtx->ch_layout);

		std::cout << "Audio: " << audioCodecCtx->sample_rate << " Hz, " << audioCodecCtx->ch_layout.nb_channels
			  << " channels" << std::endl;
		std::cout << "Audio format: " << av_get_sample_fmt_name(dstFormat) << std::endl;

		return true;
	}

	bool decodeVideoPacket(AVFrame **outFrame)
	{
		int ret = avcodec_send_packet(videoCodecCtx, packet);
		if (ret < 0) {
			std::cerr << "Error sending video packet to decoder" << std::endl;
			return false;
		}

		ret = avcodec_receive_frame(videoCodecCtx, videoFrame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return false;
		} else if (ret < 0) {
			std::cerr << "Error receiving video frame from decoder" << std::endl;
			return false;
		}

		sws_scale(swsCtx, videoFrame->data, videoFrame->linesize, 0, videoCodecCtx->height,
			  videoFrameConverted->data, videoFrameConverted->linesize);

		videoFrameConverted->pts = videoFrame->pts;
		*outFrame = videoFrameConverted;
		return true;
	}

	bool decodeAudioPacket(AVFrame **outFrame)
	{
		int ret = avcodec_send_packet(audioCodecCtx, packet);
		if (ret < 0) {
			std::cerr << "Error sending audio packet to decoder" << std::endl;
			return false;
		}

		ret = avcodec_receive_frame(audioCodecCtx, audioFrame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return false;
		} else if (ret < 0) {
			std::cerr << "Error receiving audio frame from decoder" << std::endl;
			return false;
		}

		// Allocate output buffer
		audioFrameConverted->nb_samples = audioFrame->nb_samples;
		av_channel_layout_copy(&audioFrameConverted->ch_layout, &audioFrame->ch_layout);
		audioFrameConverted->sample_rate = audioFrame->sample_rate;

		ret = av_frame_get_buffer(audioFrameConverted, 0);
		if (ret < 0) {
			std::cerr << "Failed to allocate audio output buffer" << std::endl;
			return false;
		}

		ret = swr_convert_frame(swrCtx, audioFrameConverted, audioFrame);
		if (ret < 0) {
			std::cerr << "Error converting audio frame" << std::endl;
			av_frame_unref(audioFrameConverted);
			return false;
		}

		audioFrameConverted->pts = audioFrame->pts;
		*outFrame = audioFrameConverted;
		return true;
	}

	void cleanup()
	{
		if (videoBuffer)
			av_free(videoBuffer);
		if (audioBuffer)
			av_freep(&audioBuffer[0]);
		if (audioBuffer)
			av_freep(&audioBuffer);
		if (swsCtx)
			sws_freeContext(swsCtx);
		if (swrCtx)
			swr_free(&swrCtx);
		if (videoFrame)
			av_frame_free(&videoFrame);
		if (videoFrameConverted)
			av_frame_free(&videoFrameConverted);
		if (audioFrame)
			av_frame_free(&audioFrame);
		if (audioFrameConverted)
			av_frame_free(&audioFrameConverted);
		if (packet)
			av_packet_free(&packet);
		if (videoCodecCtx)
			avcodec_free_context(&videoCodecCtx);
		if (audioCodecCtx)
			avcodec_free_context(&audioCodecCtx);
		if (fmtCtx)
			avformat_close_input(&fmtCtx);
	}
};

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::cout << "Usage: " << argv[0] << " <srt_url>" << std::endl;
		std::cout << "Example: " << argv[0] << " srt://0.0.0.0:9999?mode=listener" << std::endl;
		return 1;
	}

	std::string srtUrl = argv[1];

	SRTReceiver receiver;

	if (!receiver.init(srtUrl)) {
		std::cerr << "Failed to initialize SRT receiver" << std::endl;
		return 1;
	}

	int width, height;
	AVPixelFormat videoFormat;
	double fps;
	receiver.getVideoInfo(width, height, videoFormat, fps);

	int sampleRate, channels;
	AVSampleFormat audioFormat;
	receiver.getAudioInfo(sampleRate, channels, audioFormat);

	std::cout << "\nReceiving frames..." << std::endl;
	std::cout << "Press Ctrl+C to stop\n" << std::endl;

	int videoFrameCount = 0;
	int audioFrameCount = 0;
	AVFrame *videoFrame = nullptr;
	AVFrame *audioFrame = nullptr;

	while (receiver.receiveFrame(&videoFrame, &audioFrame)) {
		if (videoFrame) {
			videoFrameCount++;

			// Process video frame
			// Y plane: videoFrame->data[0], stride: videoFrame->linesize[0]
			// U plane: videoFrame->data[1], stride: videoFrame->linesize[1]
			// V plane: videoFrame->data[2], stride: videoFrame->linesize[2]

			if (videoFrameCount % 30 == 0) {
				std::cout << "Video frames: " << videoFrameCount << std::endl;
			}
		}

		if (audioFrame) {
			audioFrameCount++;

			// Process audio frame (planar float format)
			// For stereo FLTP:
			// Left channel: (float*)audioFrame->data[0]
			// Right channel: (float*)audioFrame->data[1]
			// Number of samples: audioFrame->nb_samples

			if (audioFrameCount % 100 == 0) {
				std::cout << "Audio frames: " << audioFrameCount << std::endl;
			}
		}
	}

	std::cout << "\nTotal video frames: " << videoFrameCount << std::endl;
	std::cout << "Total audio frames: " << audioFrameCount << std::endl;

	return 0;
}

// CMakeLists.txt
/*
cmake_minimum_required(VERSION 3.10)
project(SRTReceiver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

# Add executable
add_executable(srt_receiver srt_receiver.cpp)

# Include directories
target_include_directories(srt_receiver PRIVATE ${FFMPEG_INCLUDE_DIRS})

# Link libraries
target_link_libraries(srt_receiver ${FFMPEG_LIBRARIES})

# Compiler flags
target_compile_options(srt_receiver PRIVATE ${FFMPEG_CFLAGS_OTHER})

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    target_link_libraries(srt_receiver ws2_32 secur32 bcrypt)
elseif(APPLE)
    # macOS-specific settings
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(SECURITY_LIBRARY Security)
    find_library(VIDEOTOOLBOX_LIBRARY VideoToolbox)
    target_link_libraries(srt_receiver 
        ${COREFOUNDATION_LIBRARY}
        ${SECURITY_LIBRARY}
        ${VIDEOTOOLBOX_LIBRARY}
    )
endif()

# Installation
install(TARGETS srt_receiver DESTINATION bin)
*/