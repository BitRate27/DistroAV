#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

typedef struct {
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    AVFrame *bgra_frame;
    AVPacket *packet;
    struct SwsContext *sws_ctx;
    SOCKET socket_fd;
    int width;
    int height;
    int frame_count;
} VideoReceiver;

static int receive_exact(int socket_fd, uint8_t *buffer, int size) {
    int total_received = 0;
    while (total_received < size) {
        int received = recv(socket_fd, (char*)(buffer + total_received), 
                          size - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                printf("Connection closed by peer\n");
            } else {
#ifdef _WIN32
                printf("Error receiving data: %d\n", WSAGetLastError());
#else
                printf("Error receiving data: %s\n", strerror(errno));
#endif
            }
            return -1;
        }
        total_received += received;
    }
    return 0;
}

static int receive_packet_header(int socket_fd, int *packet_size) {
    uint32_t size_be;
    if (receive_exact(socket_fd, (uint8_t*)&size_be, sizeof(size_be)) < 0) {
        return -1;
    }
    *packet_size = ntohl(size_be);
    return 0;
}

VideoReceiver* video_receiver_create(const char *host, int port) {
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return NULL;
    }
#endif

    VideoReceiver *receiver = calloc(1, sizeof(VideoReceiver));
    if (!receiver) return NULL;
    
    receiver->frame_count = 0;
    
    // Initialize FFmpeg
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("H264 decoder not found\n");
        free(receiver);
        return NULL;
    }
    
    receiver->codec_ctx = avcodec_alloc_context3(codec);
    if (!receiver->codec_ctx) {
        printf("Could not allocate codec context\n");
        free(receiver);
        return NULL;
    }
    
    if (avcodec_open2(receiver->codec_ctx, codec, NULL) < 0) {
        printf("Could not open codec\n");
        avcodec_free_context(&receiver->codec_ctx);
        free(receiver);
        return NULL;
    }
    
    // Allocate frames
    receiver->frame = av_frame_alloc();
    receiver->bgra_frame = av_frame_alloc();
    if (!receiver->frame || !receiver->bgra_frame) {
        printf("Could not allocate frames\n");
        if (receiver->frame) av_frame_free(&receiver->frame);
        if (receiver->bgra_frame) av_frame_free(&receiver->bgra_frame);
        avcodec_free_context(&receiver->codec_ctx);
        free(receiver);
        return NULL;
    }
    
    // Allocate packet
    receiver->packet = av_packet_alloc();
    if (!receiver->packet) {
        printf("Could not allocate packet\n");
        av_frame_free(&receiver->frame);
        av_frame_free(&receiver->bgra_frame);
        avcodec_free_context(&receiver->codec_ctx);
        free(receiver);
        return NULL;
    }
    
    // Create TCP socket
    receiver->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (receiver->socket_fd < 0) {
#ifdef _WIN32
        printf("Error creating socket: %d\n", WSAGetLastError());
#else
        printf("Error creating socket: %s\n", strerror(errno));
#endif
        av_packet_free(&receiver->packet);
        av_frame_free(&receiver->frame);
        av_frame_free(&receiver->bgra_frame);
        avcodec_free_context(&receiver->codec_ctx);
        free(receiver);
        return NULL;
    }
    
    // Connect to server
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        printf("Invalid address: %s\n", host);
        close(receiver->socket_fd);
        av_packet_free(&receiver->packet);
        av_frame_free(&receiver->frame);
        av_frame_free(&receiver->bgra_frame);
        avcodec_free_context(&receiver->codec_ctx);
        free(receiver);
        return NULL;
    }
    
    if (connect(receiver->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        printf("Error connecting to %s:%d: %d\n", host, port, WSAGetLastError());
#else
        printf("Error connecting to %s:%d: %s\n", host, port, strerror(errno));
#endif
        close(receiver->socket_fd);
        av_packet_free(&receiver->packet);
        av_frame_free(&receiver->frame);
        av_frame_free(&receiver->bgra_frame);
        avcodec_free_context(&receiver->codec_ctx);
        free(receiver);
        return NULL;
    }
    
    printf("Connected to %s:%d\n", host, port);
    
    return receiver;
}

int video_receiver_setup_conversion(VideoReceiver *receiver, int width, int height) {
    receiver->width = width;
    receiver->height = height;
    
    // Setup BGRA frame
    receiver->bgra_frame->format = AV_PIX_FMT_BGRA;
    receiver->bgra_frame->width = width;
    receiver->bgra_frame->height = height;
    
    if (av_frame_get_buffer(receiver->bgra_frame, 32) < 0) {
        printf("Could not allocate BGRA frame buffer\n");
        return -1;
    }
    
    // Initialize swscale context for YUV420P to BGRA conversion
    receiver->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                                     width, height, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, NULL, NULL, NULL);
    if (!receiver->sws_ctx) {
        printf("Could not initialize swscale context\n");
        return -1;
    }
    
    return 0;
}

int video_receiver_receive_frame(VideoReceiver *receiver, uint8_t **bgra_data, 
                                int *width, int *height) {
    if (!receiver) return -1;
    
    // Receive packet size
    int packet_size;
    if (receive_packet_header((int)receiver->socket_fd, &packet_size) < 0) {
        return -1;
    }
    
    if (packet_size <= 0 || packet_size > 1024 * 1024) { // 1MB limit
        printf("Invalid packet size: %d\n", packet_size);
        return -1;
    }
    
    // Allocate buffer for packet data
    uint8_t *packet_data = malloc(packet_size);
    if (!packet_data) {
        printf("Could not allocate packet buffer\n");
        return -1;
    }
    
    // Receive packet data
    if (receive_exact((int)receiver->socket_fd, packet_data, packet_size) < 0) {
        free(packet_data);
        return -1;
    }
    
    // Set packet data
    receiver->packet->data = packet_data;
    receiver->packet->size = packet_size;
    
    // Decode packet
    int ret = avcodec_send_packet(receiver->codec_ctx, receiver->packet);
    free(packet_data);
    
    if (ret < 0) {
        printf("Error sending packet for decoding: %s\n", av_err2str(ret));
        return -1;
    }
    
    ret = avcodec_receive_frame(receiver->codec_ctx, receiver->frame);
    if (ret == AVERROR(EAGAIN)) {
        // Need more packets
        return 0;
    } else if (ret < 0) {
        printf("Error receiving frame: %s\n", av_err2str(ret));
        return -1;
    }
    
    // Setup conversion context if not already done
    if (!receiver->sws_ctx) {
        if (video_receiver_setup_conversion(receiver, receiver->frame->width, 
                                          receiver->frame->height) < 0) {
            return -1;
        }
    }
    
    // Convert YUV420P to BGRA
    sws_scale(receiver->sws_ctx, 
              (const uint8_t**)receiver->frame->data, receiver->frame->linesize,
              0, receiver->frame->height,
              receiver->bgra_frame->data, receiver->bgra_frame->linesize);
    
    // Return frame data
    *bgra_data = receiver->bgra_frame->data[0];
    *width = receiver->bgra_frame->width;
    *height = receiver->bgra_frame->height;
    
    receiver->frame_count++;
    return 1; // Successfully received frame
}

void video_receiver_destroy(VideoReceiver *receiver) {
    if (!receiver) return;
    
    if (receiver->socket_fd >= 0) {
        close(receiver->socket_fd);
    }
    
    if (receiver->sws_ctx) {
        sws_freeContext(receiver->sws_ctx);
    }
    
    if (receiver->packet) {
        av_packet_free(&receiver->packet);
    }
    
    if (receiver->frame) {
        av_frame_free(&receiver->frame);
    }
    
    if (receiver->bgra_frame) {
        av_frame_free(&receiver->bgra_frame);
    }
    
    if (receiver->codec_ctx) {
        avcodec_free_context(&receiver->codec_ctx);
    }
    
    free(receiver);
}

// Function to save BGRA frame as PPM (for testing)
void save_frame_as_ppm(const uint8_t *bgra_data, int width, int height, 
                      const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            // Convert BGRA to RGB
            fputc(bgra_data[idx + 2], f); // R
            fputc(bgra_data[idx + 1], f); // G
            fputc(bgra_data[idx + 0], f); // B
        }
    }
    
    fclose(f);
}

/*
// Example usage
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    
    const char *host = argv[1];
    int port = atoi(argv[2]);
    
    VideoReceiver *receiver = video_receiver_create(host, port);
    if (!receiver) {
        return 1;
    }
    
    uint8_t *bgra_data;
    int width, height;
    int frame_num = 0;
    
    printf("Starting to receive frames...\n");
    
    while (1) {
        int ret = video_receiver_receive_frame(receiver, &bgra_data, &width, &height);
        
        if (ret < 0) {
            printf("Error receiving frame\n");
            break;
        } else if (ret == 0) {
            // Need more data, continue
            continue;
        }
        
        printf("Received frame %d: %dx%d\n", frame_num, width, height);
        
        // Save first few frames as PPM files for verification
        if (frame_num < 5) {
            char filename[256];
            snprintf(filename, sizeof(filename), "frame_%03d.ppm", frame_num);
            save_frame_as_ppm(bgra_data, width, height, filename);
            printf("Saved %s\n", filename);
        }
        
        frame_num++;
        
        // Process the BGRA frame data here
        // bgra_data points to the decoded frame in BGRA format
        // The data is valid until the next call to video_receiver_receive_frame
    }
    
    video_receiver_destroy(receiver);
    
    return 0;
}
*/
