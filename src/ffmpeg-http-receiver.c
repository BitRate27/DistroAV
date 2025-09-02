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
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

// Base64 decoding for receiving binary data
static int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t* base64_decode(const char *input, size_t *output_length) {
    size_t input_length = strlen(input);
    if (input_length % 4 != 0) return NULL;
    
    *output_length = input_length / 4 * 3;
    if (input[input_length - 1] == '=') (*output_length)--;
    if (input[input_length - 2] == '=') (*output_length)--;
    
    uint8_t *decoded_data = malloc(*output_length);
    if (!decoded_data) return NULL;
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t sextet_a = input[i] == '=' ? 0 & i++ : base64_decode_char(input[i++]);
        uint32_t sextet_b = input[i] == '=' ? 0 & i++ : base64_decode_char(input[i++]);
        uint32_t sextet_c = input[i] == '=' ? 0 & i++ : base64_decode_char(input[i++]);
        uint32_t sextet_d = input[i] == '=' ? 0 & i++ : base64_decode_char(input[i++]);
        
        uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);
        
        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    
    return decoded_data;
}

typedef struct {
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    AVFrame *bgra_frame;
    AVPacket *packet;
    struct SwsContext *sws_ctx;
    char *server_url;
    int width;
    int height;
    int frame_count;
} VideoReceiver;

static int http_request(const char *host, int port, const char *path, 
                       char **response_body, size_t *body_length) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(socket_fd);
        return -1;
    }
    
    if (connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(socket_fd);
        return -1;
    }
    
    // Send HTTP GET request
    char request[1024];
    int request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: FFmpeg-Receiver/1.0\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);
    
    if (send(socket_fd, request, request_len, 0) < 0) {
        close(socket_fd);
        return -1;
    }
    
    // Receive response
    char *buffer = malloc(1024 * 1024); // 1MB buffer
    if (!buffer) {
        close(socket_fd);
        return -1;
    }
    
    int total_received = 0;
    int received;
    while ((received = recv(socket_fd, buffer + total_received, 
                          1024 * 1024 - total_received - 1, 0)) > 0) {
        total_received += received;
    }
    
    close(socket_fd);
    
    if (total_received <= 0) {
        free(buffer);
        return -1;
    }
    
    buffer[total_received] = '\0';
    
    // Parse HTTP response
    char *header_end = strstr(buffer, "\r\n\r\n");
    if (!header_end) {
        free(buffer);
        return -1;
    }
    
    // Check status code
    int status_code;
    if (sscanf(buffer, "HTTP/%*s %d", &status_code) != 1 || status_code != 200) {
        free(buffer);
        return status_code;
    }
    
    // Extract body
    char *body_start = header_end + 4;
    *body_length = total_received - (body_start - buffer);
    
    *response_body = malloc(*body_length + 1);
    if (!*response_body) {
        free(buffer);
        return -1;
    }
    
    memcpy(*response_body, body_start, *body_length);
    (*response_body)[*body_length] = '\0';
    
    free(buffer);
    return 200;
}

static char* extract_json_string(const char *json, const char *key) {
    char search_key[256];
    snprintf(search_key, sizeof(search_key), "\"%s\":\"", key);
    
    char *start = strstr(json, search_key);
    if (!start) return NULL;
    
    start += strlen(search_key);
    char *end = strchr(start, '"');
    if (!end) return NULL;
    
    size_t len = end - start;
    char *result = malloc(len + 1);
    if (!result) return NULL;
    
    memcpy(result, start, len);
    result[len] = '\0';
    
    return result;
}

static int extract_json_int(const char *json, const char *key) {
    char search_key[256];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    char *start = strstr(json, search_key);
    if (!start) return -1;
    
    start += strlen(search_key);
    return atoi(start);
}

VideoReceiver* video_receiver_create(const char *host, int port) {
#ifdef _WIN