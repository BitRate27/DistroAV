#ifndef FFMPEG_VIDEO_STREAM_H
#define FFMPEG_VIDEO_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ffmpeg_video_stream.h
 * @brief FFmpeg-based video streaming over TCP
 * 
 * This library provides functionality to compress and transmit video frames
 * over TCP using FFmpeg. Supports BGRA pixel format input/output with H.264
 * compression for efficient streaming.
 */

/**
 * @brief Opaque structure representing a video sender
 * 
 * Contains FFmpeg encoding context, network socket, and frame conversion
 * utilities for compressing and transmitting BGRA frames over TCP.
 */
typedef struct VideoSender VideoSender;

/**
 * @brief Opaque structure representing a video receiver
 * 
 * Contains FFmpeg decoding context, network socket, and frame conversion
 * utilities for receiving and decompressing frames to BGRA format.
 */
typedef struct VideoReceiver VideoReceiver;

/* ====== VIDEO SENDER FUNCTIONS ====== */

/**
 * @brief Create and initialize a video sender
 * 
 * Creates a TCP server socket, initializes H.264 encoder with optimized
 * settings for low-latency streaming, and sets up BGRA to YUV420P conversion.
 * 
 * @param width Frame width in pixels
 * @param height Frame height in pixels  
 * @param port TCP port to listen on
 * @return Pointer to VideoSender instance, or NULL on failure
 * 
 * @note The sender will bind to all interfaces (INADDR_ANY) on the specified port.
 *       Encoder uses "ultrafast" preset and "zerolatency" tune for minimal delay.
 */
VideoSender* video_sender_create(int width, int height, int port);

/**
 * @brief Wait for and accept a client connection
 * 
 * Blocks until a client connects to the sender's listening socket.
 * Once connected, closes the listening socket and uses the client connection.
 * 
 * @param sender Pointer to VideoSender instance
 * @return 0 on success, -1 on failure
 * 
 * @note This function must be called after video_sender_create() and before
 *       sending any frames.
 */
int video_sender_wait_connection(VideoSender *sender);

/**
 * @brief Compress and send a single video frame
 * 
 * Converts the input BGRA frame to YUV420P, encodes it using H.264,
 * and transmits the compressed packet(s) over TCP with size headers.
 * 
 * @param sender Pointer to VideoSender instance
 * @param bgra_data Pointer to BGRA pixel data (width * height * 4 bytes)
 * @return 0 on success, -1 on failure
 * 
 * @note BGRA data should be tightly packed with no padding between rows.
 *       The function may generate multiple packets per frame which are all sent.
 */
int video_sender_send_frame(VideoSender *sender, const uint8_t *bgra_data);

/**
 * @brief Clean up and destroy video sender
 * 
 * Closes network connections, frees FFmpeg resources, and deallocates memory.
 * Also performs platform-specific cleanup (WSACleanup on Windows).
 * 
 * @param sender Pointer to VideoSender instance (can be NULL)
 * 
 * @note Safe to call with NULL pointer. Should always be called to prevent
 *       resource leaks.
 */
void video_sender_destroy(VideoSender *sender);

/* ====== VIDEO RECEIVER FUNCTIONS ====== */

/**
 * @brief Create and initialize a video receiver
 * 
 * Creates a TCP client socket, connects to the specified host/port,
 * and initializes H.264 decoder for receiving compressed video frames.
 * 
 * @param host IP address or hostname of the sender
 * @param port TCP port to connect to
 * @return Pointer to VideoReceiver instance, or NULL on failure
 * 
 * @note The function blocks until connection is established or fails.
 *       Frame conversion context is set up lazily on first frame.
 */
VideoReceiver* video_receiver_create(const char *host, int port);

/**
 * @brief Set up frame conversion context (internal function)
 * 
 * Initializes the swscale context for converting decoded YUV420P frames
 * to BGRA format. Called automatically when the first frame is received.
 * 
 * @param receiver Pointer to VideoReceiver instance
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @return 0 on success, -1 on failure
 * 
 * @note This is typically called internally and doesn't need to be called
 *       directly by user code.
 */
int video_receiver_setup_conversion(VideoReceiver *receiver, int width, int height);

/**
 * @brief Receive and decode a video frame
 * 
 * Receives compressed video packets from TCP connection, decodes them,
 * and converts the result to BGRA format for use by the application.
 * 
 * @param receiver Pointer to VideoReceiver instance
 * @param bgra_data Output pointer to BGRA pixel data (managed internally)
 * @param width Output frame width in pixels
 * @param height Output frame height in pixels
 * @return 1 if frame received successfully, 0 if more data needed, -1 on error
 * 
 * @note The returned bgra_data pointer is valid until the next call to this
 *       function. Copy the data if you need to retain it longer.
 *       The function handles variable-length packets and frame buffering.
 */
int video_receiver_receive_frame(VideoReceiver *receiver, uint8_t **bgra_data, 
                                int *width, int *height);

/**
 * @brief Clean up and destroy video receiver
 * 
 * Closes network connections, frees FFmpeg resources, and deallocates memory.
 * Also performs platform-specific cleanup (WSACleanup on Windows).
 * 
 * @param receiver Pointer to VideoReceiver instance (can be NULL)
 * 
 * @note Safe to call with NULL pointer. Should always be called to prevent
 *       resource leaks.
 */
void video_receiver_destroy(VideoReceiver *receiver);

/* ====== UTILITY FUNCTIONS ====== */

/**
 * @brief Save BGRA frame data as PPM image file
 * 
 * Converts BGRA pixel data to RGB and saves as a PPM (Portable Pixmap)
 * image file for debugging and verification purposes.
 * 
 * @param bgra_data Pointer to BGRA pixel data
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @param filename Output filename (should end with .ppm)
 * 
 * @note PPM format is uncompressed and widely supported by image viewers.
 *       Useful for verifying frame content during development.
 */
void save_frame_as_ppm(const uint8_t *bgra_data, int width, int height, 
                      const char *filename);

/* ====== PROTOCOL DETAILS ====== */

/**
 * @brief Network protocol specification
 * 
 * The streaming protocol uses TCP with a simple framing format:
 * 
 * Packet Structure:
 * - 4 bytes: Packet size (uint32_t, network byte order)
 * - N bytes: H.264 compressed video packet data
 * 
 * Features:
 * - Reliable delivery via TCP
 * - Variable-length packet support
 * - Network byte order for cross-platform compatibility
 * - No authentication or encryption (add as needed)
 * 
 * Typical Usage:
 * 1. Create sender and wait for connection
 * 2. Create receiver and connect to sender
 * 3. Send/receive frames in a loop
 * 4. Clean up resources when done
 */

/* ====== ERROR CODES ====== */

/**
 * @brief Function return values
 * 
 * Most functions return:
 * -  0: Success
 * - -1: Error (check console output for details)
 * 
 * video_receiver_receive_frame() returns:
 * -  1: Frame successfully received and decoded
 * -  0: More data needed (continue calling)
 * - -1: Error occurred
 */

#ifdef __cplusplus
}
#endif

#endif /* FFMPEG_VIDEO_STREAM_H */
