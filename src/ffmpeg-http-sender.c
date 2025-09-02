#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
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

// Base64 encoding for binary data transmission
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const uint8_t *data, size_t input_length) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    if (!encoded_data) return NULL;
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        
        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }
    
    for (size_t i = 0; i < (3 - input_length % 3) % 3; i++)
        encoded_data[output_length - 1 - i] = '=';
    
    encoded_data[output_length] = '\0';
    return encoded_data;
}

typedef struct {
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *sws_ctx;
    int server_fd;
    int port;
    int width;
    int height;
    int frame_count;
    char *cert_file;
    char *key_file;
} VideoSender;

static const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    
    return "application/octet-stream";
}

static void send_http_response(int client_fd, int status_code, const char* status_text, 
                              const char* content_type, const char* body, size_t body_len) {
    char header[1024];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);
    
    send(client_fd, header, header_len, 0);
    if (body && body_len > 0) {
        send(client_fd, body, body_len, 0);
    }
}

static void send_viewer_html(int client_fd) {
    const char* html = 
        "<!DOCTYPE html>\n"
        "<html><head><title>Video Stream Viewer</title></head>\n"
        "<body style='margin:0;background:#000;display:flex;justify-content:center;align-items:center;min-height:100vh;'>\n"
        "<div style='text-align:center;color:white;font-family:Arial,sans-serif;'>\n"
        "<h1>Live Video Stream</h1>\n"
        "<canvas id='canvas' style='border:2px solid #333;max-width:90vw;max-height:70vh;'></canvas>\n"
        "<div id='status' style='margin-top:10px;'>Connecting...</div>\n"
        "<script>\n"
        "const canvas = document.getElementById('canvas');\n"
        "const ctx = canvas.getContext('2d');\n"
        "const status = document.getElementById('status');\n"
        "let frameCount = 0;\n"
        "\n"
        "function pollFrame() {\n"
        "  fetch('/frame')\n"
        "    .then(response => {\n"
        "      if (!response.ok) throw new Error('Network response was not ok');\n"
        "      return response.json();\n"
        "    })\n"
        "    .then(data => {\n"
        "      if (data.frame) {\n"
        "        const img = new Image();\n"
        "        img.onload = () => {\n"
        "          canvas.width = data.width;\n"
        "          canvas.height = data.height;\n"
        "          ctx.drawImage(img, 0, 0);\n"
        "          frameCount++;\n"
        "          status.textContent = `Frame: ${frameCount} (${data.width}x${data.height})`;\n"
        "        };\n"
        "        img.src = 'data:image/png;base64,' + data.frame;\n"
        "      }\n"
        "      setTimeout(pollFrame, 33); // ~30 FPS\n"
        "    })\n"
        "    .catch(err => {\n"
        "      status.textContent = 'Connection error: ' + err.message;\n"
        "      setTimeout(pollFrame, 1000); // Retry after 1 second\n"
        "    });\n"
        "}\n"
        "\n"
        "pollFrame();\n"
        "</script>\n"
        "</div></body></html>";
    
    send_http_response(client_fd, 200, "OK", "text/html", html, strlen(html));
}

static void handle_http_request(VideoSender *sender, int client_fd, const char* request) {
    char method[16], path[256], version[16];
    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3) {
        send_http_response(client_fd, 400, "Bad Request", "text/plain", "Bad Request", 11);
        return;
    }
    
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/viewer") == 0) {
            send_viewer_html(client_fd);
        } else if (strcmp(path, "/frame") == 0) {
            // Return current frame as base64-encoded PNG
            if (sender->packet && sender->packet->size > 0) {
                char* encoded = base64_encode(sender->packet->data, sender->packet->size);
                if (encoded) {
                    char json_response[1024 + strlen(encoded)];
                    int json_len = snprintf(json_response, sizeof(json_response),
                        "{\"frame\":\"%s\",\"width\":%d,\"height\":%d,\"timestamp\":%d}",
                        encoded, sender->width, sender->height, sender->frame_count);
                    
                    send_http_response(client_fd, 200, "OK", "application/json", 
                                     json_response, json_len);
                    free(encoded);
                } else {
                    send_http_response(client_fd, 500, "Internal Server Error", 
                                     "application/json", "{\"error\":\"encoding failed\"}", 25);
                }
            } else {
                send_http_response(client_fd, 204, "No Content", "application/json", 
                                 "{\"error\":\"no frame available\"}", 29);
            }
        } else {
            send_http_response(client_fd, 404, "Not Found", "text/plain", "Not Found", 9);
        }
    } else if (strcmp(method, "OPTIONS") == 0) {
        send_http_response(client_fd, 200, "OK", "text/plain", "", 0);
    } else {
        send_http_response(client_fd, 405, "Method Not Allowed", "text/plain", 
                         "Method Not Allowed", 18);
    }
}

VideoSender* video_sender_create(int width, int height, int port) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return NULL;
    }
#endif

    VideoSender *sender = calloc(1, sizeof(VideoSender));
    if (!sender) {
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    sender->width = width;
    sender->height = height;
    sender->port = port;
    sender->frame_count = 0;
    
    // Initialize FFmpeg
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("H264 encoder not found\n");
        free(sender);
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    sender->codec_ctx = avcodec_alloc_context3(codec);
    if (!sender->codec_ctx) {
        printf("Could not allocate codec context\n");
        free(sender);
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    // Set codec parameters
    sender->codec_ctx->bit_rate = 2000000;
    sender->codec_ctx->width = width;
    sender->codec_ctx->height = height;
    sender->codec_ctx->time_base = (AVRational){1, 30};
    sender->codec_ctx->framerate = (AVRational){30, 1};
    sender->codec_ctx->gop_size = 10;
    sender->codec_ctx->max_b_frames = 1;
    sender->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    av_opt_set(sender->codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(sender->codec_ctx->priv_data, "tune", "zerolatency", 0);
    
    if (avcodec_open2(sender->codec_ctx, codec, NULL) < 0) {
        printf("Could not open codec\n");
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    // Allocate frame and packet
    sender->frame = av_frame_alloc();
    sender->packet = av_packet_alloc();
    if (!sender->frame || !sender->packet) {
        printf("Could not allocate frame or packet\n");
        if (sender->frame) av_frame_free(&sender->frame);
        if (sender->packet) av_packet_free(&sender->packet);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    sender->frame->format = sender->codec_ctx->pix_fmt;
    sender->frame->width = width;
    sender->frame->height = height;
    
    if (av_frame_get_buffer(sender->frame, 32) < 0) {
        printf("Could not allocate frame buffer\n");
        av_packet_free(&sender->packet);
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    // Initialize swscale context
    sender->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                   width, height, AV_PIX_FMT_YUV420P,
                                   SWS_BILINEAR, NULL, NULL, NULL);
    if (!sender->sws_ctx) {
        printf("Could not initialize swscale context\n");
        av_packet_free(&sender->packet);
        av_frame_free(&sender->frame);
        avcodec_free_context(&sender->codec_ctx);
        free(sender);
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    
    return sender;
}

int video_sender_start_server(VideoSender *sender) {
    if (!sender) return -1;
    
    sender->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sender->server_fd < 0) {
#ifdef _WIN32
        printf("Error creating socket: %d\n", WSAGetLastError());
#else
        printf("Error creating socket: %s\n", strerror(errno));
#endif
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sender->server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#ifdef _WIN32
        printf("Error setting socket options: %d\n", WSAGetLastError());
#else
        printf("Error setting socket options: %s\n", strerror(errno));
#endif
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(sender->port);
    
    if (bind(sender->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        printf("Error binding socket: %d\n", WSAGetLastError());
#else
        printf("Error binding socket: %s\n", strerror(errno));
#endif
        close(sender->server_fd);
        return -1;
    }
    
    if (listen(sender->server_fd, 10) < 0) {
#ifdef _WIN32
        printf("Error listening on socket: %d\n", WSAGetLastError());
#else
        printf("Error listening on socket: %s\n", strerror(errno));
#endif
        close(sender->server_fd);
        return -1;
    }
    
    printf("HTTP server listening on port %d\n", sender->port);
    printf("Open http://localhost:%d in your browser to view the stream\n", sender->port);
    
    return 0;
}

int video_sender_encode_frame(VideoSender *sender, const uint8_t *bgra_data) {
    if (!sender || !bgra_data) return -1;
    
    // Convert BGRA to YUV420P
    const uint8_t *src_data[1] = { bgra_data };
    int src_linesize[1] = { sender->width * 4 };
    
    sws_scale(sender->sws_ctx, src_data, src_linesize, 0, sender->height,
              sender->frame->data, sender->frame->linesize);
    
    sender->frame->pts = sender->frame_count++;
    
    // Encode frame
    int ret = avcodec_send_frame(sender->codec_ctx, sender->frame);
    if (ret < 0) {
        printf("Error sending frame for encoding: %s\n", av_err2str(ret));
        return -1;
    }
    
    ret = avcodec_receive_packet(sender->codec_ctx, sender->packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0; // Need more frames
    } else if (ret < 0) {
        printf("Error receiving packet: %s\n", av_err2str(ret));
        return -1;
    }
    
    return 1; // Frame encoded successfully
}

void video_sender_handle_requests(VideoSender *sender) {
    if (!sender || sender->server_fd < 0) return;
    
    fd_set read_fds;
    struct timeval timeout;
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(sender->server_fd, &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout
        
        int activity = select(sender->server_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity < 0) {
#ifdef _WIN32
            printf("Select error: %d\n", WSAGetLastError());
#else
            printf("Select error: %s\n", strerror(errno));
#endif
            break;
        }
        
        if (activity > 0 && FD_ISSET(sender->server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            
            int client_fd = accept(sender->server_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd >= 0) {
                char buffer[4096];
                int received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                if (received > 0) {
                    buffer[received] = '\0';
                    handle_http_request(sender, client_fd, buffer);
                }
                close(client_fd);
            }
        }
    }
}

int video_sender_send_frame(VideoSender *sender, const uint8_t *bgra_data) {
    return video_sender_encode_frame(sender, bgra_data);
}

void video_sender_destroy(VideoSender *sender) {
    if (!sender) return;
    
    if (sender->server_fd >= 0) {
        close(sender->server_fd);
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
    
    if (sender->cert_file) {
        free(sender->cert_file);
    }
    
    if (sender->key_file) {
        free(sender->key_file);
    }
    
    free(sender);
}

// Example usage with HTTP server
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <width> <height> <port>\n", argv[0]);
        printf("Example: %s 640 480 8080\n", argv[0]);
        printf("Then open http://localhost:8080 in your browser\n");
        return 1;
    }
    
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);
    int port = atoi(argv[3]);
    
    VideoSender *sender = video_sender_create(width, height, port);
    if (!sender) {
        return 1;
    }
    
    if (video_sender_start_server(sender) < 0) {
        video_sender_destroy(sender);
        return 1;
    }
    
    // Generate test frames in a separate thread or loop
    uint8_t *frame_data = malloc(width * height * 4);
    
    // Start frame generation loop
    for (int frame = 0; frame < 9000; frame++) { // 5 minutes at 30fps
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
        
        video_sender_send_frame(sender, frame_data);
        
        // Handle HTTP requests (non-blocking)
        video_sender_handle_requests(sender);
        
        printf("Generated frame %d\r", frame);
        fflush(stdout);
        
#ifdef _WIN32
        Sleep(33);
#else
        usleep(33333);
#endif
    }
    
    free(frame_data);
    video_sender_destroy(sender);
    
    return 0;
}