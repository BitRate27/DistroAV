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
    #define sleep Sleep
    #define usleep(x) Sleep((x)/1000)
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
    AVPacket *packet;
    struct SwsContext *sws_ctx;
    SOCKET socket_fd;
    int width;
    int height;
    int frame_count;
} VideoSender;

static int send_packet_header(int socket_fd, int packet_size) {
    uint32_t size_be = htonl(packet_size);
    int sent = send(socket_fd, (const char*)&size_be, sizeof(size_be), 0);
    return sent == sizeof(size_be) ? 0 : -1;
}

static int send_packet_data(int socket_fd, const uint8_t *data, int size) {
    int total_sent = 0;
    while (total_sent < size) {
        int sent = send(socket_fd, (const char*)(data + total_sent), size - total_sent, 0);
        if (sent <= 0) {
#ifdef _WIN32
            printf("Error sending data: %d\n", WSAGetLastError());
#else
            printf("Error sending data: %s\n", strerror(errno));
#endif
            return -1;
        }
        total_sent += sent;
    }
    return 0;
}

VideoSender* video_sender_create(int width, int height, int port) {
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return NULL;
    }
#endif

    VideoSender *sender = calloc(1, sizeof(VideoSender));
    if (!sender) return NULL;
    
    sender->width = width;
    sender->height = height;
    sender->frame_count = 0;
    
    // Initialize FFmpeg
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("H264 encoder not found\n");
        free(sender);
        return NULL;
    }
    
    sender->codec_ctx = avcodec_alloc_context3(codec);
    if (!sender->codec_ctx) {
        printf("Could not allocate codec context\n");
        free(sender);
        return NULL;
    }
    
    // Set codec parameters
    sender->codec_ctx->bit_rate = 2000000; // 2 Mbps
    sender->codec_ctx->width = width;
    sender->codec_ctx->height = height;
    sender->codec_ctx->time_base = (AVRational){1, 30}; // 30 FPS
    sender->codec_ctx->framerate = (AVRational){30, 1};
    sender->codec_ctx->gop_size = 10;
    sender->codec_ctx->max_b_frames = 1;
    sender->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    // Set preset for faster encoding
    av_opt_set(sender->codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(sender->codec_ctx->priv_data, "tune", "zerolatency", 0);
    
    if (avcodec_open2(sender->codec_ctx, codec, NULL) < 0) {
        printf("Could not open codec\n");
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    // Allocate frame
    sender->frame = av_frame_alloc();
    if (!sender->frame) {
        printf("Could not allocate frame\n");
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    sender->frame->format = sender->codec_ctx->pix_fmt;
    sender->frame->width = width;
    sender->frame->height = height;
    
    if (av_frame_get_buffer(sender->frame, 32) < 0) {
        printf("Could not allocate frame buffer\n");
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    // Allocate packet
    sender->packet = av_packet_alloc();
    if (!sender->packet) {
        printf("Could not allocate packet\n");
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    // Initialize swscale context for BGRA to YUV420P conversion
    sender->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                   width, height, AV_PIX_FMT_YUV420P,
                                   SWS_BILINEAR, NULL, NULL, NULL);
    if (!sender->sws_ctx) {
        printf("Could not initialize swscale context\n");
        av_packet_free(&sender->packet);
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    // Create TCP socket
    sender->socket_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sender->socket_fd < 0) {
#ifdef _WIN32
        printf("Error creating socket: %d\n", WSAGetLastError());
#else
        printf("Error creating socket: %s\n", strerror(errno));
#endif
        sws_freeContext(sender->sws_ctx);
        av_packet_free(&sender->packet);
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(sender->socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#ifdef _WIN32
        printf("Error setting socket options: %d\n", WSAGetLastError());
#else
        printf("Error setting socket options: %s\n", strerror(errno));
#endif
    }
    
    // Bind and listen
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sender->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        printf("Error binding socket: %d\n", WSAGetLastError());
#else
        printf("Error binding socket: %s\n", strerror(errno));
#endif
        close(sender->socket_fd);
        sws_freeContext(sender->sws_ctx);
        av_packet_free(&sender->packet);
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    if (listen(sender->socket_fd, 1) < 0) {
#ifdef _WIN32
        printf("Error listening on socket: %d\n", WSAGetLastError());
#else
        printf("Error listening on socket: %s\n", strerror(errno));
#endif
        close(sender->socket_fd);
        sws_freeContext(sender->sws_ctx);
        av_packet_free(&sender->packet);
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
        return NULL;
    }
    
    printf("Waiting for connection on port %d...\n", port);
    
    return sender;
}

int video_sender_wait_connection(VideoSender *sender) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = (int)accept((int)sender->socket_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
#ifdef _WIN32
        printf("Error accepting connection: %d\n", WSAGetLastError());
#else
        printf("Error accepting connection: %s\n", strerror(errno));
#endif
        return -1;
    }
    
    //printf("Client connected from %s:%d\n", 
    //       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    
    close(sender->socket_fd);
    sender->socket_fd = client_fd;
    
    return 0;
}

int video_sender_send_frame(VideoSender *sender, const uint8_t *bgra_data) {
    if (!sender || !bgra_data) return -1;
    
    // Convert BGRA to YUV420P
    const uint8_t *src_data[1] = { bgra_data };
    int src_linesize[1] = { sender->width * 4 }; // BGRA = 4 bytes per pixel
    
    sws_scale(sender->sws_ctx, src_data, src_linesize, 0, sender->height,
              sender->frame->data, sender->frame->linesize);
    
    sender->frame->pts = sender->frame_count++;
    
    // Encode frame
    int ret = avcodec_send_frame(sender->codec_ctx, sender->frame);
    if (ret < 0) {
        printf("Error sending frame for encoding: %s\n", av_err2str(ret));
        return -1;
    }
    
    while (ret >= 0) {
        ret = avcodec_receive_packet(sender->codec_ctx, sender->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            printf("Error receiving packet: %s\n", av_err2str(ret));
            return -1;
        }
        
        // Send packet size first, then packet data
        if (send_packet_header((int)sender->socket_fd, sender->packet->size) < 0) {
            printf("Error sending packet header\n");
            av_packet_unref(sender->packet);
            return -1;
        }
        
        if (send_packet_data((int)sender->socket_fd, sender->packet->data, sender->packet->size) < 0) {
            printf("Error sending packet data\n");
            av_packet_unref(sender->packet);
            return -1;
        }
        
        av_packet_unref(sender->packet);
    }
    
    return 0;
}

void video_sender_destroy(VideoSender *sender) {
    if (!sender) return;
    
    if (sender->socket_fd >= 0) {
        close(sender->socket_fd);
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    if (sender->sws_ctx) {
        sws_freeContext(sender->sws_ctx);
    }
    
    if (sender->packet) {
        av_packet_free(&sender->packet);
    }
    
    if (sender->frame) {
        av_frame_free(&sender->frame);
    }
    
    if (sender->codec_ctx) {
        avcodec_free_context(&sender->codec_ctx);
    }
    
    free(sender);
}

/* Example usage
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <width> <height> <port>\n", argv[0]);
        return 1;
    }
    
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);
    int port = atoi(argv[3]);
    
    VideoSender *sender = video_sender_create(width, height, port);
    if (!sender) {
        return 1;
    }
    
    if (video_sender_wait_connection(sender) < 0) {
        video_sender_destroy(sender);
        return 1;
    }
    
    // Generate and send test frames (colored rectangles)
    uint8_t *frame_data = malloc(width * height * 4); // BGRA
    
    for (int frame = 0; frame < 300; frame++) { // Send 10 seconds at 30fps
        // Generate test pattern
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                frame_data[idx + 0] = (frame + x) % 256;     // B
                frame_data[idx + 1] = (frame + y) % 256;     // G
                frame_data[idx + 2] = (frame + x + y) % 256; // R
                frame_data[idx + 3] = 255;                   // A
            }
        }
        
        if (video_sender_send_frame(sender, frame_data) < 0) {
            break;
        }
        
        printf("Sent frame %d\n", frame);
#ifdef _WIN32
        Sleep(33); // ~30 FPS
#else
        usleep(33333); // ~30 FPS
#endif
    }
    
    free(frame_data);
    video_sender_destroy(sender);
    
    return 0;
}
*/
