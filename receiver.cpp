#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <iomanip>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#pragma comment(lib, "ws2_32.lib")

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
}

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

// sender ports
constexpr char HOST[] = "::1"; // IPv6 loopback address
constexpr uint16_t GAME_PORT = 8888;
constexpr uint16_t LIGHTDATA_PORT = 8887;

// receiver ports
constexpr uint16_t FFMPEGDATA_PORT = 9999;
constexpr uint16_t IMGUIDATA_PORT = 9998;
constexpr uint16_t COLLECTIBLE_PORT = 9997;
constexpr uint16_t PLAYERDATA_PORT = 9996;
constexpr uint16_t GAMESTATE_PORT = 9995;

SOCKET lightDataSocket;
sockaddr_in6 lightSenderAddr;

// === Structures and Types ===
struct RTHeader_t {
    double time;
    unsigned long packetnum;
};

#pragma pack(push, 1)

struct FragmentHeader_t {
    uint32_t frame_id;
    uint16_t total_fragments;
    uint16_t fragment_index;
};

#pragma pack(pop)

struct FrameBuffer {
    uint16_t total_fragments = 0;
    std::map<uint16_t, std::vector<uint8_t>> fragments;
    size_t total_size = 0;
};

struct DecodedFrame {
    int width;
    int height;
    std::vector<uint8_t> rgba;
};

#pragma pack(push, 1)
struct ReceiverReport {
    double timestamp;
    uint32_t bytes_received;
    uint32_t expected_packets;
    uint32_t received_packets;
    float    frame_rate;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct NAKPacket {
    uint32_t frame_id;
    uint16_t missing_index;
};
#pragma pack(pop)

#pragma pack(push, 1)
// to initialise ImGui Window #1
struct Object {
    // this would include camera, player, cubes/walls, cyphers 
    char name[32];
    float pos[3];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct SceneData { 
    uint16_t obj_count; // total number of Objects tagged as Retrievables
    Object objects[100];
};
#pragma pack(pop)

#pragma pack(push, 1)
// to intiailise ImGui Window #2
struct PlayerData {
    float pos[3];
    bool moving;
};
#pragma pack(pop)

// to initialise ImGui Window #3
#pragma pack(push, 1)
struct SpoLightData {
    char type[16];
    float pos[3];
    float rot[3];
    float color[4];
};
struct PoiLightData {
    char type[16];
    float pos[3];
    float color[4];
};
struct DirLightData {
    char type[16];
    float color[4];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct LightData {
    SpoLightData spot_light;
    PoiLightData point_light;
    DirLightData directional_light;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct Collectible {
    char name[16];
    uint16_t count;
};

struct CollectibleData {
    uint16_t collectible_count;
    Collectible collectible[10];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct ImGuiPacket {
    SceneData scene;
    PlayerData player;
    LightData lights;
    CollectibleData collectibles;
};
#pragma pack(pop)


// === Globals ===
static constexpr int MAX_UDP_PACKET_SIZE = 65536;
static constexpr int BUFFER_THRESHOLD = 5;
constexpr uint32_t SCROLL_UP_CODE = 1000;
constexpr uint32_t SCROLL_DOWN_CODE = 1001;
bool isSDLInitialized = false;
bool fromImguiPacket = true;
bool gameEnd = false;
ImGuiPacket latestImGuiPacket = {}; 
PlayerData latestPlayerPacket = {}; 
CollectibleData latestCollectableDataPacket = {};

std::mutex frameQueueMutex;
std::condition_variable frameQueueCondVar;
std::queue<DecodedFrame> frameQueue;
std::atomic<bool> running{true};
std::atomic<uint32_t> total_bytes{0};
std::atomic<uint32_t> expected_packet_count{0};
std::atomic<uint32_t> received_packet_count{0};
std::atomic<uint32_t> decoded_frame_count{0};

// === Classes ===
class UDPsend {
    public:
        int sock = 0;
        struct sockaddr_in6 addr;
        unsigned int packetnum = 0;

        static constexpr int MTU = 1400;
        static constexpr int HEADER_SIZE = sizeof(RTHeader_t) + sizeof(FragmentHeader_t);
        static constexpr int MAX_PAYLOAD = MTU - HEADER_SIZE;

        UDPsend() {};

        ~UDPsend() {};

        void init(const char *address, int port) {
            sock = socket( AF_INET6, SOCK_DGRAM, 0);
            struct addrinfo hints;

            memset(&addr, 0, sizeof(addr));
            memset(&hints, 0, sizeof(hints));

            hints.ai_family = AF_INET6;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags = 0;

            struct addrinfo *result = NULL;
            auto dwRetval = getaddrinfo(address, nullptr, &hints, &result);
            if ( dwRetval != 0 ) {
                printf("getaddrinfo failed with error: %d\n", dwRetval);
                return;
            }
            for (addrinfo* ptr = result; ptr != NULL; ptr = ptr->ai_next) {
                if (ptr->ai_family == AF_INET6) {
                    memcpy(&addr, ptr->ai_addr, ptr->ai_addrlen);
                    addr.sin6_port = htons(port);
                    addr.sin6_family = AF_INET6;
                }
            }
            freeaddrinfo(result);
        };
        
        // for the sending of frames
        int send_fragmented(char* buffer, int len) {
            packetnum++; // new frame ID
            
            int total_fragments = (len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
        
            for (int i = 0; i < total_fragments; ++i) {
                int payload_size = std::min(MAX_PAYLOAD, len - i * MAX_PAYLOAD);
        
                char sendbuffer[MTU];

                RTHeader_t rt_header;
                auto now = std::chrono::steady_clock::now();
                rt_header.time = std::chrono::duration<double>(now.time_since_epoch()).count();
                rt_header.packetnum = packetnum;
                // std::cout << "rt_header.packetnum: " << rt_header.packetnum << std::endl;


                FragmentHeader_t frag_header;
                frag_header.frame_id = packetnum;
                frag_header.total_fragments = total_fragments;
                frag_header.fragment_index = i;
                // std::cout << "frag_header.frame_id: " << frag_header.frame_id << std::endl;

                memcpy(sendbuffer, &rt_header, sizeof(rt_header));
                memcpy(sendbuffer + sizeof(rt_header), &frag_header, sizeof(frag_header));
                memcpy(sendbuffer + HEADER_SIZE, buffer + i * MAX_PAYLOAD, payload_size);
        
                int ret = sendto(sock, sendbuffer, HEADER_SIZE + payload_size, 0,
                                 (const sockaddr*)&addr, sizeof(addr));
                    // std::cout << "HEADER_SIZE: " << HEADER_SIZE << std::endl;
                if (ret < 0) {
                    std::cerr << "Failed to send fragment " << i << "\n";
                    return ret;
                }
            }
        
            return total_fragments;
        }
        
        // for the sending of imgui data
        int send_raw(const char* buffer, int len) {
            int ret = sendto(sock, buffer, len, 0, (const sockaddr*)&addr, sizeof(addr));
            if (ret < 0) {
                std::cerr << "Failed to send raw data\n";
            }
            return ret;
        }

        void closeSock() {
            closesocket(sock);
            sock=0;
        };
};

class UDPreceive {
    private:
        SOCKET sock;
        sockaddr_in6 addr;
        char* recbuffer;

    public:
        UDPreceive() {
            recbuffer = new char[65000];
        }

        ~UDPreceive() {
            delete[] recbuffer;
            closesocket(sock);
        }

        void init(int port) {
            sock = socket(AF_INET6, SOCK_DGRAM, 0);
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            addr.sin6_addr = in6addr_any;
            int ret = bind(sock, (const sockaddr*)&addr, sizeof(addr));
            printf("Binding port %d return %d\n", port, ret);
        }

        SOCKET getSock() const {
            return sock;
        }
        
        sockaddr_in6 getAddr() const {
            return addr;
        }

        int receive(char* buffer, int len) {
            sockaddr_in6 addr;
            socklen_t slen = sizeof(addr);

            int ret = recvfrom(sock, buffer, len, 0, (sockaddr*)&addr, &slen);
            if (ret == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) return 0;  // No data
                std::cerr << "recvfrom failed: " << err << "\n";
                return -1;
            }
            return ret;
        }
        int receive_raw(char* buffer, int len) {
            sockaddr_in6 addr;
            socklen_t slen = sizeof(addr);

            int ret = recvfrom(sock, buffer, len, 0, (sockaddr*)&addr, &slen);
            if (ret == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) return 0;  // No data
                std::cerr << "recvfrom failed: " << err << "\n";
                return -1;
            }
            memcpy(&latestImGuiPacket, buffer, sizeof(ImGuiPacket));
            return ret;
        }


        // int receive_raw(char* buffer, int len, double* ptime) {
        //     return receive_raw(buffer, len, "", ptime);
        // }

        int receive_fragment(uint32_t& out_frame_id, uint16_t& out_total, uint16_t& out_index, std::vector<uint8_t>& out_payload) {
            sockaddr_in6 addr;
            socklen_t slen = sizeof(addr);
            char recbuffer[MAX_UDP_PACKET_SIZE];

            int ret = recvfrom(sock, recbuffer, sizeof(recbuffer), 0, (sockaddr*)&addr, &slen);
            if (ret == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) return 0;
                std::cerr << "recvfrom failed: " << err << "\n";
                return -1;
            }

            RTHeader_t* rt_header = (RTHeader_t*)recbuffer;
            FragmentHeader_t* frag_header = (FragmentHeader_t*)(recbuffer + sizeof(RTHeader_t));
            char* payload = recbuffer + sizeof(RTHeader_t) + sizeof(FragmentHeader_t);
            int payloadSize = ret - (sizeof(RTHeader_t) + sizeof(FragmentHeader_t));

            out_frame_id = frag_header->frame_id;
            out_total = frag_header->total_fragments;
            out_index = frag_header->fragment_index;
            out_payload.assign(payload, payload + payloadSize);

            total_bytes += payloadSize;
            expected_packet_count += out_total;
            received_packet_count++;

            return payloadSize;
        }

        void decode_thread_func(AVCodecContext* codecCtx) {
            std::unordered_map<uint32_t, FrameBuffer> frame_buffer_map;
            AVPacket* packet = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();

            while (running.load()) {
                uint32_t frame_id;
                uint16_t total_fragments, fragment_index;
                std::vector<uint8_t> payload;

                int recv_ret = receive_fragment(frame_id, total_fragments, fragment_index, payload);
                if (recv_ret <= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                auto& buffer = frame_buffer_map[frame_id];
                buffer.total_fragments = total_fragments;
                buffer.fragments[fragment_index] = payload;
                buffer.total_size += payload.size();

                if (buffer.fragments.size() == total_fragments) {
                    std::vector<uint8_t> full_frame;
                    full_frame.reserve(buffer.total_size);
                    for (int i = 0; i < total_fragments; ++i) {
                        if (buffer.fragments.count(i) == 0) {
                            std::cerr << "Missing fragment index " << i << " for frame " << frame_id << "\n";
                            frame_buffer_map.erase(frame_id);
                            break;
                        }
                        auto& frag = buffer.fragments[i];
                        full_frame.insert(full_frame.end(), frag.begin(), frag.end());
                    }
                    frame_buffer_map.erase(frame_id);
                    decoded_frame_count++;

                    av_packet_unref(packet);
                    av_new_packet(packet, full_frame.size());
                    memcpy(packet->data, full_frame.data(), full_frame.size());
                    if (avcodec_send_packet(codecCtx, packet) == 0) {
                        while (avcodec_receive_frame(codecCtx, frame) == 0) {
                            int w = frame->width, h = frame->height;
                            SwsContext* sws = sws_getContext(w, h, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                            std::vector<uint8_t> rgba(w * h * 4);
                            uint8_t* dst[1] = { rgba.data() };
                            int linesize[1] = { w * 4 };
                            sws_scale(sws, frame->data, frame->linesize, 0, h, dst, linesize);
                            sws_freeContext(sws);

                            std::unique_lock<std::mutex> lock(frameQueueMutex);
                            frameQueue.push({w, h, std::move(rgba)});
                            frameQueueCondVar.notify_one();
                        }
                    }
                }
            }

            av_frame_free(&frame);
            av_packet_free(&packet);
        }

        void closeSock() {
            closesocket(sock);
            sock = 0;
        }
};

// === Network Setup ===
int startWinsock() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

void sendKeyToGame(uint8_t scancode, float dt, UDPsend& sender) {
    std::cout << "[KeyPress] scancode: " << static_cast<int>(scancode) << std::endl;
    char buffer[16];
    sprintf(buffer, "%u:%.3f", scancode, dt);
    sender.send_raw(buffer, strlen(buffer));
}

void send_nak(SOCKET sock, const sockaddr_in6& sender_addr, uint32_t frame_id, uint16_t missing_index) {
    NAKPacket nak{frame_id, missing_index};
    sendto(sock, (char*)&nak, sizeof(nak), 0, (sockaddr*)&sender_addr, sizeof(sender_addr));
    std::cout << "[NAK] Requested resend for frame " << frame_id << ", fragment " << missing_index << "\n";
}

void reportLoop(SOCKET reportSock, sockaddr_in6 senderAddr) {
    const int interval_seconds = 10;
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));

        ReceiverReport report;
        report.timestamp = static_cast<double>(SDL_GetTicks()) / 1000.0;
        report.bytes_received = total_bytes.exchange(0);
        report.expected_packets = expected_packet_count.exchange(0);
        report.received_packets = received_packet_count.exchange(0);
        report.frame_rate = decoded_frame_count.exchange(0) / (float)interval_seconds;

        sendto(reportSock, reinterpret_cast<char*>(&report), sizeof(report), 0,
               reinterpret_cast<sockaddr*>(&senderAddr), sizeof(senderAddr));
    }
}


int main() {
    if (startWinsock() != 0) return -1;

    // Setup FFmpeg decoding
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_open2(codecCtx, codec, nullptr);

    // Initialize UDP receivers
    UDPreceive ffmpegReceiver;
    UDPreceive imguiReceiver;
    UDPreceive collectibleReceiver;
    UDPreceive playerReceiver;
    UDPreceive gameStateReceiver;
    ffmpegReceiver.init(FFMPEGDATA_PORT);
    imguiReceiver.init(IMGUIDATA_PORT);
    collectibleReceiver.init(COLLECTIBLE_PORT);
    playerReceiver.init(PLAYERDATA_PORT);
    gameStateReceiver.init(GAMESTATE_PORT);

    u_long mode = 1;
    ioctlsocket(ffmpegReceiver.getSock(), FIONBIO, &mode);
    ioctlsocket(imguiReceiver.getSock(), FIONBIO, &mode);  
    ioctlsocket(collectibleReceiver.getSock(), FIONBIO, &mode);  
    ioctlsocket(playerReceiver.getSock(), FIONBIO, &mode);  
    ioctlsocket(gameStateReceiver.getSock(), FIONBIO, &mode);  

    // Start FFmpeg decoding thread
    std::thread(reportLoop, ffmpegReceiver.getSock(), ffmpegReceiver.getAddr()).detach();
    std::thread decoderThread(&UDPreceive::decode_thread_func, &ffmpegReceiver, codecCtx);

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Init(SDL_INIT_VIDEO);

    UDPsend keyEventSender;
    keyEventSender.init(HOST, GAME_PORT);
    UDPsend lightEventSender;
    lightEventSender.init(HOST, LIGHTDATA_PORT);
    
    SDL_Event e;
    while (running.load()) {
        if (gameEnd) {
            keyEventSender.closeSock();
        }
        // ImGui packet receive
        char buffer[sizeof(ImGuiPacket)];
        
        int len = imguiReceiver.receive_raw(buffer, sizeof(buffer));
        if (len == sizeof(ImGuiPacket)) {
            std::cout << "[Receiver] ImGuiPacket received!\n";
            
        } else if (len > 0) {
            std::cerr << "[Receiver] Incorrect ImGuiPacket size: " << len << "\n";
        }

        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE)) {
                running.store(false);
                frameQueueCondVar.notify_all();
                break;
            }
            if (e.type == SDL_EVENT_KEY_DOWN) {
                sendKeyToGame(e.key.scancode, 0.053f, keyEventSender);
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                // SDL3 uses `e.wheel.x` and `e.wheel.y` for horizontal and vertical scroll
                std::cout << "[Mouse Scroll] x: " << e.wheel.x << ", y: " << e.wheel.y << "\n";

                float dt = 0.033f; // or calculate dynamically
                if (e.wheel.y > 0) {
                    std::string msg = std::to_string(SCROLL_UP_CODE) + ":" + std::to_string(dt);
                    keyEventSender.send_raw(msg.c_str(), msg.length());
                } else if (e.wheel.y < 0) {
                    std::string msg = std::to_string(SCROLL_DOWN_CODE) + ":" + std::to_string(dt);
                    keyEventSender.send_raw(msg.c_str(), msg.length());
                }

            }
        }

        std::unique_lock<std::mutex> lock(frameQueueMutex);
        if (!frameQueue.empty()) {
            auto frame = std::move(frameQueue.front());
            frameQueue.pop();
            lock.unlock();

            if (!isSDLInitialized) {
                window = SDL_CreateWindow("Receiver", frame.width, frame.height, 0);
                renderer = SDL_CreateRenderer(window, nullptr);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGui::StyleColorsDark();
                ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
                ImGui_ImplSDLRenderer3_Init(renderer);
                isSDLInitialized = true;
            }

            SDL_UpdateTexture(texture, nullptr, frame.rgba.data(), frame.width * 4);
            ImGui_ImplSDL3_NewFrame();
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui::NewFrame();

            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowSize(ImVec2(300, 200));
            ImGui::Begin("Scene");
            ImGui::SeparatorText("Player Status");
            
            char playerDataBuffer[sizeof(PlayerData)];
            if (playerReceiver.receive(playerDataBuffer, sizeof(playerDataBuffer))) {
                memcpy(&latestPlayerPacket, playerDataBuffer, sizeof(PlayerData));
                ImGui::Text("Position: %.2f %.2f", latestPlayerPacket.pos[0], latestPlayerPacket.pos[1]);
                ImGui::Text("State: %s", latestPlayerPacket.moving ? "MOVING" : "STATIONARY");
            } else {
                ImGui::Text("Position: %.2f %.2f", latestImGuiPacket.player.pos[0], latestImGuiPacket.player.pos[1]);
                ImGui::Text("State: %s", latestImGuiPacket.player.moving ? "MOVING" : "STATIONARY");
            }
            ImGui::SeparatorText("Objects");
            for (int i = 0; i < latestImGuiPacket.scene.obj_count; ++i) {
                std::string objName = latestImGuiPacket.scene.objects[i].name;
                float* pos = latestImGuiPacket.scene.objects[i].pos;

                ImGui::Text("%s", objName.c_str());
                ImGui::Text("  Position: (%.2f, %.2f, %.2f)", pos[0], pos[1], pos[2]);
                ImGui::Separator();
            }
            ImGui::End();

            ImGui::SetNextWindowPos(ImVec2(10, 220));
            ImGui::SetNextWindowSize(ImVec2(300, 350));
            ImGui::Begin("Light Settings");
            if (ImGui::BeginTabBar("Light Settings")) {
                if (ImGui::BeginTabItem("Spot Light")) {
                    auto& l = latestImGuiPacket.lights.spot_light;
                    
                    bool posUpdate = false;
                    if (ImGui::SliderFloat3("Position", l.pos, -50.0f, 50.0f)) {
                        posUpdate = true;
                    } else {
                        posUpdate = false;
                    }
                    
                    bool rotUpdate = false;
                    if (ImGui::SliderFloat3("Rotation", l.rot, -50.0f, 50.0f)) {
                        rotUpdate = true;
                    } else {
                        rotUpdate = false;
                    }
                    
                    bool colUpdate = false;
                    std::string label = l.type;
                    float color[4] = { l.color[0], l.color[1], l.color[2], l.color[3] };
                    if (ImGui::ColorPicker4(label.c_str(), color)) {
                        colUpdate = true;
                    } else {
                        colUpdate = false;
                    }
                    ImGui::EndTabItem();

                    if ( posUpdate || rotUpdate || colUpdate ) {
                        memcpy(l.color, color, sizeof(color));

                        SpoLightData packet;
                        strcpy(packet.type, "Spot");
                        memcpy(packet.pos, l.pos, sizeof(l.pos));
                        memcpy(packet.rot, l.rot, sizeof(l.rot));
                        memcpy(packet.color, l.color, sizeof(l.color));

                        std::cout << "  Position: (" 
                                << l.pos[0] << ", " 
                                << l.pos[1] << ", " 
                                << l.pos[2] << ")\n";

                        std::cout << "  Rotation (deg): (" 
                                << l.rot[0] << ", " 
                                << l.rot[1] << ", " 
                                << l.rot[2] << ")\n";

                        std::cout << "spot light buffer being sent!" << std::endl;
                        lightEventSender.send_raw(reinterpret_cast<const char*>(&packet), sizeof(packet));
                    }
                }
                
                if (ImGui::BeginTabItem("Point Light")) {
                    auto& l = latestImGuiPacket.lights.point_light;

                    bool posUpdate = false;
                    if (ImGui::SliderFloat3("Position", l.pos, -50.0f, 50.0f)) {
                        posUpdate = true;
                    } else {
                        posUpdate = false;
                    }
                    
                    bool colUpdate = false;
                    std::string label = l.type;
                    float color[4] = { l.color[0], l.color[1], l.color[2], l.color[3] };
                    if (ImGui::ColorPicker4(label.c_str(), color)) {
                        colUpdate = true;
                    } else {
                        colUpdate = false;
                    }
                    ImGui::EndTabItem();

                    if ( posUpdate || colUpdate ) {
                        memcpy(l.color, color, sizeof(color));

                        PoiLightData packet{};
                        strcpy(packet.type, "Point");
                        memcpy(packet.pos, l.pos, sizeof(l.pos));
                        memcpy(packet.color, l.color, sizeof(l.color));

                        std::cout << "  Position: (" 
                                << l.pos[0] << ", " 
                                << l.pos[1] << ", " 
                                << l.pos[2] << ")\n";

                        std::cout << "point light buffer being sent!" << std::endl;
                        lightEventSender.send_raw(reinterpret_cast<const char*>(&packet), sizeof(packet));
                    }
                }

                

                if (ImGui::BeginTabItem("Directional Light")) {
                    auto& l = latestImGuiPacket.lights.directional_light;

                    bool colUpdate = false;
                    std::string label = l.type;
                    float color[4] = { l.color[0], l.color[1], l.color[2], l.color[3] };
                    if (ImGui::ColorPicker4(label.c_str(), color)) {
                        colUpdate = true;
                    } else {
                        colUpdate = false;
                    }
                    ImGui::EndTabItem();

                    if ( colUpdate ) {
                        memcpy(l.color, color, sizeof(color));

                        SpoLightData packet{};
                        strcpy(packet.type, "Directional");
                        memcpy(packet.color, l.color, sizeof(l.color));

                        std::cout << "directional light buffer being sent!" << std::endl;
                        lightEventSender.send_raw(reinterpret_cast<const char*>(&packet), sizeof(packet));
                    }
                }

                ImGui::EndTabBar();
            }
            ImGui::End();

                
            ImGui::SetNextWindowPos(ImVec2(890, 10));
            ImGui::SetNextWindowSize(ImVec2(300, 150));
            
            if (ImGui::Begin("Mission")) {
                char buffer[1];  // buffer to hold 1 byte (enough for a bool)

                int received = gameStateReceiver.receive(buffer, sizeof(buffer));
                if (received == sizeof(bool)) {
                    gameEnd = buffer[0];  // buffer[0] will be 0 (false) or 1 (true)
                }

                char collectibleDataBuffer[sizeof(CollectibleData)];
                if (collectibleReceiver.receive(collectibleDataBuffer, sizeof(CollectibleData))) {
                    bool allZero = true;
                    // std::cout << "collectibleReceiver received the packet!" << std::endl;
                    memcpy(&latestCollectableDataPacket, collectibleDataBuffer, sizeof(CollectibleData));
                    auto& collectible_data = latestCollectableDataPacket.collectible;
                    for (int i = 0; i < latestCollectableDataPacket.collectible_count; ++i) {
                        if (latestCollectableDataPacket.collectible[i].count > 0) {
                            allZero = false;
                            break;
                        }
                    }
                    if (gameEnd) {
                        ImGui::Text("GAME OVER!");
                    } else if (allZero) {
                        ImGui::Text("Gate is open!");
                    } else {
                        ImGui::Text("Find the missing items:");
                        for (int i = 0; i < latestCollectableDataPacket.collectible_count; ++i) { 
                            const Collectible& collectible = latestCollectableDataPacket.collectible[i];
                            // std::cout << "collectible.name" << collectible.name << std::endl;
                            // std::cout << "collectible.count" << collectible.count << std::endl;
                            ImGui::BulletText("%s: %d", collectible.name, collectible.count);
                        }
                    }
                }
            }
            ImGui::End();

            ImGui::Render();
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);

            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        } else {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    decoderThread.join();
    avcodec_free_context(&codecCtx);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    keyEventSender.closeSock();
    WSACleanup();
    return 0;
}
