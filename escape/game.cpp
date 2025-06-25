extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>

    #define WIN32_LEAN_AND_MEAN

    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    
}

#include <iostream>
#include <utility>
#include <format>
#include <cstdint>
#include "VHInclude.h"
#include "VEInclude.h"
#include <vector>
#include <atomic>
#include <string>
#include <chrono>
#include <thread>
#include <map> 
#include <unordered_map>
#include <algorithm> 
#include <SDL.h>
#include <glm/gtc/type_ptr.hpp>

#include "stb_image_write.h"

#pragma comment(lib, "ws2_32.lib")

constexpr char HOST[] = "::1";

// sending FFMPEG
typedef struct RTHeader {
    double time;
    unsigned long packetnum;
} RTHeader_t;

#pragma pack(push, 1)
typedef struct FragmentHeader {
    uint32_t frame_id;
    uint16_t total_fragments;
    uint16_t fragment_index;
} FragmentHeader_t;
#pragma pack(pop)

// sending IMGUI
#pragma pack(push, 1)
// to initialise ImGui Window #1
struct Object {
    // this would include camera, player, cubes/walls, cyphers 
    char name[32];
    float pos[3];
};

struct SceneData { 
    uint16_t obj_count; // total number of Objects tagged as Retrievables
    Object objects[100];
};

// to intiailise ImGui Window #2
struct PlayerData {
    float pos[3];
    bool moving;
};

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

// to initialise ImGui Window #4 - collectibles
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
struct LightData {
    SpoLightData spot_light;
    PoiLightData point_light;
    DirLightData directional_light;
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

// asset paths setup - ObjectConfig
struct ObjectConfig {
    std::string modelPath;
    std::string namePrefix;
    int counter = 0;
};

// asset paths setup - CollectibleConfig
struct CollectibleConfig {
    std::string namePrefix;
    std::string modelPath;
    int count;
};

// collectible packet data
// #pragma pack(push, 1)
// struct CollectibleInfo {
//     char name[32];
//     int remaining;
// };
// struct CollectiblePacket {
//     uint16_t collectible_count;
//     CollectibleInfo collectibles[32];
// };
// #pragma pack(pop)



// === GLOBAL CONSTANTS ===
// receiver: keydown
constexpr uint16_t KEYDOWN_PORT = 8888;
std::atomic<bool> runInputThread{true};

// receiver: light updates
constexpr uint16_t LIGHTUPDATE_PORT = 8887;

// sender: FFMPEG sender
constexpr uint16_t FFMPEG_PORT = 9999;

// sender: light sender
constexpr uint16_t LIGHTDATA_PORT = 9998;
std::atomic<bool> runLightThread{true};

// sender: collectibles
constexpr uint16_t COLLECTIBLE_PORT = 9997;

// sender: player data sender
constexpr uint16_t PLAYERDATA_PORT = 9996;

// sender: game state sender
constexpr uint16_t GAMESTATE_PORT = 9995;

std::mutex m_spotLightMutex;
std::optional<SpoLightData> m_receivedSpotUpdate;

std::mutex m_pointLightMutex;
std::optional<PoiLightData> m_receivedPointUpdate;

std::mutex m_directionalLightMutex;
std::optional<DirLightData> m_receivedDirectionalUpdate;

// others
constexpr uint32_t SCROLL_UP_CODE = 1000;
constexpr uint32_t SCROLL_DOWN_CODE = 1001;

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

        void init(int port) {
            sock = socket( AF_INET6, SOCK_DGRAM, 0);
            struct addrinfo hints;

            memset(&addr, 0, sizeof(addr));
            memset(&hints, 0, sizeof(hints));

            hints.ai_family = AF_INET6;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags = 0;

            struct addrinfo *result = NULL;
            auto dwRetval = getaddrinfo(HOST, nullptr, &hints, &result);
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

        char* getBuffer() const {
            return recbuffer;
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
            // memcpy(&latestImGuiPacket, buffer, sizeof(ImGuiPacket));

            return ret;
        }

        void closeSock() {
            closesocket(sock);
            sock = 0;
        }
};

class FrameLimiter {
    public:
    FrameLimiter(float targetFPS)
    : m_frameDuration
        (
        std::chrono::duration_cast<std::chrono::steady_clock::duration>
            (
            std::chrono::duration<float>(1.0f / targetFPS)
            )
        ),
    m_nextFrameTime
        (
        std::chrono::steady_clock::now()
        ) {}

    
        void Wait() {
            auto now = std::chrono::steady_clock::now();
    
            if (now < m_nextFrameTime) {
                std::this_thread::sleep_until(m_nextFrameTime);
            }
    
            m_nextFrameTime += m_frameDuration;
        }
    
    private:
        std::chrono::steady_clock::duration m_frameDuration;
        std::chrono::steady_clock::time_point m_nextFrameTime;
};
    
class FFmpegWriter {
    public:
        FFmpegWriter(int width, int height, const std::string& outputFile)
            : m_width(width), m_height(height)
        {
            std::string cmd = std::format(
                R"(ffmpeg -y -f rawvideo -pixel_format rgba -video_size {}x{} -framerate 60 -i - -c:v libx264 -preset ultrafast -pix_fmt yuv420p -f h264 "{}")",
                width, height, outputFile
            );
    
            m_pipe = _popen(cmd.c_str(), "wb");
            if (!m_pipe) {
                std::cerr << "Failed to start FFmpeg." << std::endl;
            }
        }
    
        void WriteFrame(const uint8_t* frameData) {
            if (m_pipe) {
                fwrite(frameData, 1, m_width * m_height * 4, m_pipe);
            }
        }
    
        ~FFmpegWriter() {
            if (m_pipe) {
                _pclose(m_pipe);
            }
        }
    
    private:
        int m_width;
        int m_height;
        FILE* m_pipe = nullptr;
};

class FFmpegEncoder {
    public:
        FFmpegEncoder(int width, int height) 
            : m_width(width), m_height(height)
        {
    
            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) {
                std::cerr << "Codec not found\n";
                return;
            }
            m_codecCtx = avcodec_alloc_context3(codec);
            if (!m_codecCtx) {
                std::cerr << "Could not allocate codec context\n";
                return;
            }

            m_codecCtx->bit_rate = 400000;
            m_codecCtx->width = m_width;
            m_codecCtx->height = m_height;
            m_codecCtx->time_base = {1, 60};
            m_codecCtx->framerate = {60, 1};
            m_codecCtx->gop_size = 10;
            m_codecCtx->max_b_frames = 1;
            m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    
            av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);

            if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
                std::cerr << "Could not open codec\n";
                return;
            }
    
            m_frame = av_frame_alloc();
            if (!m_frame) {
                std::cerr << "Could not allocate frame\n";
                return;
            }

            m_frame->format = m_codecCtx->pix_fmt;
            m_frame->width = m_codecCtx->width;
            m_frame->height = m_codecCtx->height;
            av_frame_get_buffer(m_frame, 32);
    
            m_packet = av_packet_alloc();
            if (!m_packet) {
                std::cerr << "Could not allocate packet\n";
                return;
            }

    
            // SwsContext to convert from RGBA to YUV420P
            m_swsCtx = sws_getContext(
                width, height, AV_PIX_FMT_RGBA,
                width, height, AV_PIX_FMT_YUV420P,
                SWS_BICUBIC, nullptr, nullptr, nullptr
            );
            if (!m_swsCtx) {
                std::cerr << "Could not allocate SwsContext\n";
                return;
            }
        }
    
        ~FFmpegEncoder() {
            sws_freeContext(m_swsCtx);
            av_packet_free(&m_packet);
            av_frame_free(&m_frame);
            avcodec_free_context(&m_codecCtx);
        }
    
        // Returns encoded H.264 buffer (in packet), and size
        std::pair<uint8_t*, int> EncodeFrame(const uint8_t* rgbaData) {
            const uint8_t* srcSlice[] = { rgbaData };
            int srcStride[] = { 4 * m_width };
    
            // Convert RGBA → YUV420P
            sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_height, m_frame->data, m_frame->linesize);
    
            m_frame->pts = m_pts++;
    
            // Send frame to encoder
            int ret = avcodec_send_frame(m_codecCtx, m_frame);
            if (ret < 0) return {nullptr, 0};
    
            ret = avcodec_receive_packet(m_codecCtx, m_packet);
            if (ret < 0) return {nullptr, 0};
    
            // Return pointer and size (packet data is owned by FFmpeg)
            return { m_packet->data, m_packet->size };
        }
    
        void FreePacket() {
            av_packet_unref(m_packet);
        }
    
    private:
        int m_width, m_height;
        int m_pts = 0;
        AVCodecContext* m_codecCtx = nullptr;
        AVFrame* m_frame = nullptr;
        AVPacket* m_packet = nullptr;
        SwsContext* m_swsCtx = nullptr;
    };


// === GLOBAL VARIABLES ===
std::unique_ptr<FFmpegWriter> m_ffmpegWriter;
std::unique_ptr<FFmpegEncoder> m_ffmpegEncoder;


int startWinsock(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 0), &wsa);
}

enum Tags : size_t {
    Tag_Retrievable = 1,
    Tag_Collectible = 2,
    Tag_Exit = 3
};

class MyGame : public vve::System {
    public:
        MyGame(vve::Engine& engine) 
            : vve::System("MyGame", engine) 
        {
            m_engine.RegisterCallbacks({
                {this, 5000, "LOAD_LEVEL", [this](Message& message){ return OnLoadLevel(message); }},
                {this, 0, "UPDATE", [this](Message& message){ return OnUpdate(message); }},
                {this, 0, "RECORD_NEXT_FRAME", [this](Message& message){ return OnRecordNextFrame(message); }},
                {this, 0, "FRAME_END", [this](Message& message){ return OnFrameEnd(message); } },
                {this, 0, "SDL_KEY_DOWN", [this](Message& message){ return OnKeyDown(message);} },
                {this, 0, "SDL_KEY_REPEAT", [this](Message& message){ return OnKeyDown(message);} }
            });
        }
    
        ~MyGame() = default;
    
    private:
        // --- Constants ---
        enum class PlayerState { STATIONARY, MOVING };
    
        // --- Inline asset paths ---
        inline static const std::string plane_obj  { "assets/test/plane/plane_t_n_s.obj" };
        inline static const std::string plane_mesh { "assets/test/plane/plane_t_n_s.obj/plane" };
        inline static const std::string plane_txt  { "assets/test/plane/grass.jpg" };

        // wall
        inline static const std::string cube_obj   { "assets/test/crate0/cube.obj" };

        // grave
        inline static const std::string g1_obj   { "../escape/assets/graveyard/Models/OBJ format/gravestone-cross.obj" };
        inline static const std::string g2_obj   { "../escape/assets/graveyard/Models/OBJ format/gravestone-bevel.obj" };
        
        // furniture
        inline static const std::string b_obj   { "../escape/assets/graveyard/Models/OBJ format/bench.obj" };
        inline static const std::string l_obj   { "../escape/assets/graveyard/Models/OBJ format/lightpost-double.obj" };
        inline static const std::string t_obj   { "../escape/assets/graveyard/Models/OBJ format/pine.obj" };
        inline static const std::string r1_obj   { "../escape/assets/graveyard/Models/OBJ format/rocks.obj" };
        inline static const std::string r2_obj   { "../escape/assets/graveyard/Models/OBJ format/rocks-tall.obj" };
        inline static const std::string p_obj   { "../escape/assets/graveyard/Models/OBJ format/column-large.obj" };
        
        // collectibles
        inline static const std::string candle_obj   { "../escape/assets/graveyard/Models/OBJ format/candle.obj" };
        inline static const std::string wood_obj   { "../escape/assets/graveyard/Models/OBJ format/debris-wood.obj" };
        inline static const std::string stone_obj   { "../escape/assets/graveyard/Models/OBJ format/debris.obj" };
        inline static const std::string shovel_obj   { "../escape/assets/graveyard/Models/OBJ format/shovel.obj" };
        inline static const std::string cross_obj   { "../escape/assets/graveyard/Models/OBJ format/cross.obj" };
        
        // door!
        inline static const std::string door_obj          { "../escape/assets/graveyard/Models/OBJ format/fence.obj" };
        inline static const std::string broken_door_obj   { "../escape/assets/graveyard/Models/OBJ format/fence-damaged.obj" };
        // others
        inline static const std::string cypher_obj   { "../escape/assets/furniture/Models/computerScreen.obj" };

        // characters
        inline static std::string player_obj{ "../escape/assets/mini_characters/Models/OBJ format/character-female-a.obj" };
        inline static std::string zombie_obj{ "../escape/assets/graveyard/Models/OBJ format/character-zombie.obj" };
        inline static std::string ghost_obj{ "../escape/assets/graveyard/Models/OBJ format/character-ghost.obj" };
        
        // --- Game State ---
        PlayerState m_playerState = PlayerState::STATIONARY;
        int m_cubeCollected = 0;
        float m_volume = 100.0f;
        bool m_lastCollisionState = false;
        bool m_allCollected = false;
        glm::vec3 m_cameraOffsetLocal = glm::vec3(0.0f, -2.0f, 2.0f);  
    
        // --- Handles ---
        vecs::Handle m_cameraHandle{};
        vecs::Handle m_cameraNodeHandle{};
        vecs::Handle m_playerHandle{};
        vecs::Handle m_cypherHandle{};
        vecs::Handle m_buffHandle{};
        // vecs::Handle m_handlePlane{};
        // std::map<vecs::Handle, std::string> m_objects;
        std::vector<std::string> m_mapGrid;
        std::vector<vecs::Handle> m_objectHandles;
        std::vector<vecs::Handle> m_collectibleHandles;
        std::vector<vecs::Handle> m_collectedHandles;
        std::vector<CollectibleConfig> m_collectibleData = {
            { "Candle", candle_obj, 1 },
            { "Wood", wood_obj, 3 },
            { "Stone", stone_obj, 1 },
            { "Shovel", shovel_obj, 1 },
            { "Cross", cross_obj, 2 }
        };
        std::unordered_map<char, ObjectConfig> m_objectTypesData = {
                { 'a', { cube_obj, "Fence",   0 } },
                { 'b', { g1_obj, "Grave (Type 1)", 0 } },
                { 'c', { g2_obj, "Grave (Type 2)", 0 } },
                { 'd', { b_obj, "Bench", 0 } },
                { 'e', { l_obj, "Light Post", 0 } },
                { 'f', { t_obj, "Tree", 0 } },
                { 'g', { r1_obj, "Rock (Type 1)", 0 } },
                { 'h', { r2_obj, "Rock (Type 2)", 0 } },
                { 'i', { zombie_obj, "Zombie", 0 } },
                { 'j', { ghost_obj, "Ghost", 0 } },
                { 'k', { door_obj       , "Exit", 0 } },
                { 'l', { broken_door_obj, "Exit", 0 } }
            };



        // // --- Streaming Variables ---
        UDPsend m_UDPsender;
        UDPsend m_LightSender;
        UDPsend m_CollectibleSender;
        UDPsend m_PlayerDataSender;
        UDPsend m_GameStateSender;
        FrameLimiter m_frameLimiter{25.0f};

        UDPreceive m_keyboardEventReceiver;
        UDPreceive m_lightEventReceiver;

        // --- Helper ---
        vec3_t RandomPosition() {
            return vec3_t{ float(rand() % 40 - 20), float(rand() % 40 - 20), 0.0f };
        }
    
        void GetCamera() {
            if (!m_cameraHandle.IsValid()) {
                auto [handle, camera, parent] = *m_registry.GetView<vecs::Handle, vve::Camera&, vve::ParentHandle>().begin();
                m_cameraHandle = handle; 
                m_cameraNodeHandle = parent; 
            }
        }

        vec3_t RandomWalkablePosition() {
            std::vector<std::pair<int, int>> walkableTiles;
        
            for (int row = 0; row < m_mapGrid.size(); ++row) {
                const auto& line = m_mapGrid[row];
                for (int col = 0; col < line.length(); ++col) {
                    if (line[col] == ' ') {
                        walkableTiles.emplace_back(row, col);
                    }
                }
            }
        
            if (walkableTiles.empty()) return {0.0f, 0.0f, 0.1f}; // fallback
        
            auto [row, col] = walkableTiles[rand() % walkableTiles.size()];
        
            float spacing = 1.0f;
            float x = (float)(col - 5) * spacing;
            float y = (float)(row - 1) * spacing;
        
            return { x, y, 0.1f };
        }
        
        CollectibleData SpawnCollectibles() {
            CollectibleData ret = {};
            size_t collect_idx = 0; 

            std::set<std::pair<int, int>> usedTiles; // to avoid duplicates

            for (const auto& item : m_collectibleData) {
                
                Collectible collect;
                std::string nameStr = static_cast<std::string>(item.namePrefix);
                strncpy(collect.name, nameStr.c_str(), sizeof(collect.name));
                collect.count = static_cast<uint16_t>(item.count);
                ret.collectible[collect_idx] = collect;
                collect_idx++;
                for (int i = 0; i < item.count; ++i) {
                    vec3_t pos;

                    // Prevent spawning multiple items on the same tile
                    int attempts = 0;
                    while (true) {
                        pos = RandomWalkablePosition();

                        // Convert back to grid coordinate
                        int col = static_cast<int>(std::round(pos.x + 5));
                        int row = static_cast<int>(std::round(pos.y + 1));

                        if (usedTiles.count({row, col}) == 0 || attempts > 50) {
                            usedTiles.insert({row, col});
                            break;
                        }
                        attempts++;
                    }

                    std::string name = item.namePrefix + " " + std::to_string(i + 1);
                    vecs::Handle handle = m_registry.Insert(
                        vve::Position{ pos },
                        vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0))) },
                        vve::Scale{ vec3_t{0.5f} },
                        vve::Name{ name },
                        aiProcess_FlipWindingOrder
                    );
                    m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Collectible));
                    m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));
                    m_collectibleHandles.push_back(handle); // reuse for all collectibles

                    m_engine.SendMsg(MsgSceneCreate{
                        vve::ObjectHandle(handle),
                        vve::ParentHandle{},
                        vve::Filename{ item.modelPath },
                        aiProcess_FlipWindingOrder
                    });
                }
                ret.collectible_count = static_cast<uint16_t>(collect_idx);
            }
            return ret;
        }

        // void SpawnCyphers(int count) {
        //     for (int i = 0; i < count; ++i) {
        //         vec3_t pos = RandomWalkablePosition();
        //         std::string cypher_name = "Cypher " + std::to_string(i + 1);
        //         vecs::Handle handle = m_registry.Insert(
        //             vve::Position{ pos },
        //             vve::Rotation{ glm::mat3( glm::rotate( glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0,0,1)) * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0) ) ) },
        //             vve::Scale{ vec3_t{0.1f} },
        //             vve::Name{ cypher_name }
        //         );
        //         m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));

        //         m_collectibleHandles.push_back(handle);
        
        //         m_engine.SendMsg(MsgSceneCreate{
        //             vve::ObjectHandle(handle), vve::ParentHandle{}, vve::Filename{cypher_obj}, aiProcess_FlipWindingOrder
        //         });
        //     }
        // }

        void LoadMapAndSpawnWalls(const std::string& mapFilePath) {
            std::ifstream mapFile(mapFilePath);
            if (!mapFile.is_open()) {
                std::cerr << "Failed to open map file: " << mapFilePath << std::endl;
                return;
            }
        
            std::string line;
            int row = 0;
            int wall_num = 0;
            m_mapGrid.clear();

            while (std::getline(mapFile, line)) {
                m_mapGrid.push_back(line);
                for (int col = 0; col < line.length(); ++col) {
                    char symbol = line[col];
                    if (m_objectTypesData.find(symbol) != m_objectTypesData.end()) {
                        float spacing = 1.0f;
                        float x = (float)(col - 5) * spacing;
                        float y = (float)(row - 1) * spacing;

                        ObjectConfig& config = m_objectTypesData[symbol];
                        config.counter++;
                        std::string objName = config.namePrefix + " " + std::to_string(config.counter);

                        vecs::Handle handle;
                        if (config.namePrefix == "Fence") {
                            handle = m_registry.Insert(
                                vve::Position{ {x, y, 0.5f} },
                                vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0))) },
                                vve::Scale{ vec3_t{1.0f} },
                                vve::Name{ objName }
                            );
                            m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));
                        } else if (config.namePrefix == "Exit") {
                            handle = m_registry.Insert(
                                vve::Position{ {x, y, 0.0f} },
                                vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0))) },
                                vve::Scale{ vec3_t{1.0f} },
                                vve::Name{ objName }
                            );
                            m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Exit));
                        } else {
                            handle = m_registry.Insert(
                                vve::Position{ {x, y, 0.01f} },
                                vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0))) },
                                vve::Scale{ vec3_t{1.0f} },
                                vve::Name{ objName },
                                aiProcess_FlipWindingOrder
                            );
                            m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));

                        }
                        

                        m_objectHandles.push_back(handle);

                        m_engine.SendMsg(MsgSceneCreate{
                            vve::ObjectHandle(handle),
                            vve::ParentHandle{},
                            vve::Filename{config.modelPath},
                            aiProcess_FlipWindingOrder
                        });
                    }
                }
                row++;
            }

        
            // while (std::getline(mapFile, line)) {
            //     m_mapGrid.push_back(line);
            //     for (int col = 0; col < line.length(); ++col) {
            //         if (line[col] == '#') { // '#' means place a wall cube
            //             // Each cube is at grid (col, row), but we might want to scale it (spacing between cubes)
            //             float spacing = 1.0f; // How far apart cubes are
            //             float x = (float)(col-5) * spacing;
            //             float y = (float)(row-1) * spacing; // -row so Y axis goes downward
            //             // std::cout << x << std::endl;
            //             // std::cout << y << std::endl;
            //             // std::cout << "spawned cube" << std::endl;

        
            //             // Insert cube
            //             wall_num ++;
            //             std::string wall_name = "Fence " + std::to_string(wall_num);
            //             vecs::Handle handle = m_registry.Insert(
            //                 vve::Position{ {x, y, 0.5f} },
            //                 vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), 3.14159f / 2.0f, glm::vec3(1.0f,0.0f,0.0f))) },
            //                 vve::Scale{ vec3_t{1.0f} },
            //                 vve::Name{ wall_name }
            //             );
            //             m_registry.AddTags(handle, static_cast<size_t>(Tags::Tag_Retrievable));
        
            //             m_objectHandles.push_back(handle);
        
            //             m_engine.SendMsg(MsgSceneCreate{
            //                 vve::ObjectHandle(handle),
            //                 vve::ParentHandle{},
            //                 vve::Filename{cube_obj},
            //                 aiProcess_FlipWindingOrder
            //             });
            //         }
            //     }
            //     row++;
            // }
        
            mapFile.close();
        }

        // method for sending collectible data to receiver
        // CollectiblePacket BuildCollectiblePacket() {
        //     CollectiblePacket packet = {};
        //     std::unordered_map<std::string, int> collectibleRemaining;

        //     // Count remaining based on tags and name prefixes
        //     for (const auto& [handle, name] : m_registry.GetView<vecs::Handle, vve::Name>(std::vector<size_t>{Tags::Tag_Collectible})) {
        //         std::string n = static_cast<std::string>(name);
        //         std::string prefix = n.substr(0, n.find(' '));  // e.g., "Candle 1" → "Candle"
        //         collectibleRemaining[prefix]++;
        //     }

        //     int i = 0;
        //     for (const auto& [prefix, count] : collectibleRemaining) {
        //         if (i >= 32) break;
        //         strncpy(packet.collectibles[i].name, prefix.c_str(), sizeof(packet.collectibles[i].name));
        //         packet.collectibles[i].name[sizeof(packet.collectibles[i].name) - 1] = '\0';
        //         packet.collectibles[i].remaining = count;
        //         i++;
        //     }

        //     packet.collectible_count = i;
        //     return packet;
        // }

        bool CheckCollision(const glm::vec3& proposedPos) {
            float playerRadius = 0.3f; // adjust if you want tighter/looser collision
            glm::vec3 cubeHalfExtents(0.5f, 0.5f, 0.5f); // assuming 1x1x1 cubes
            glm::vec3 cypherHalfExtents(0.05f, 0.05f, 0.05f); // assuming 1x1x1 cubes
        
            for (auto& cubeHandle : m_objectHandles) {
                if (!cubeHandle.IsValid()) continue;
        
                auto& cubePos = m_registry.Get<vve::Position&>(cubeHandle)();
                auto& cubeName = m_registry.Get<vve::Name&>(cubeHandle)();
                std::string name = cubeName.substr(0, cubeName.find(' ')); 

                float minX = cubePos.x - cubeHalfExtents.x;
                float maxX = cubePos.x + cubeHalfExtents.x;
                float minY = cubePos.y - cubeHalfExtents.y;
                float maxY = cubePos.y + cubeHalfExtents.y;
        
                float closestX = glm::clamp(proposedPos.x, minX, maxX);
                float closestY = glm::clamp(proposedPos.y, minY, maxY);
        
                float dx = proposedPos.x - closestX;
                float dy = proposedPos.y - closestY;
        
                if (dx * dx + dy * dy < playerRadius * playerRadius) {
                    if (name == "Zombie" || name == "Ghost") {
                        m_engine.SendMsg(vve::System::MsgPlaySound{ vve::Filename{"../escape/assets/sounds/error.wav"}, 1, 60 });
                        bool gameState = true;
                        m_GameStateSender.send_raw(reinterpret_cast<const char*>(&gameState), sizeof(gameState));
                    }
                    return true; // collision detected
                }
            }
            
            for (auto it = m_collectibleHandles.begin(); it != m_collectibleHandles.end(); ) {
                auto& handle = *it;

                if (!handle.IsValid()) {
                    ++it;
                    continue;
                }

                auto pos = m_registry.Get<vve::Position&>(handle)(); // COPY the position

                float minX = pos.x - 0.05f;
                float maxX = pos.x + 0.05f;
                float minY = pos.y - 0.05f;
                float maxY = pos.y + 0.05f;

                float closestX = glm::clamp(proposedPos.x, minX, maxX);
                float closestY = glm::clamp(proposedPos.y, minY, maxY);

                float dx = proposedPos.x - closestX;
                float dy = proposedPos.y - closestY;

                if (dx * dx + dy * dy < playerRadius * playerRadius) {
                    std::cout << "collision with collectible!" << std::endl;
                    m_engine.SendMsg(vve::System::MsgPlaySound{ vve::Filename{"../escape/assets/sounds/pickup.wav"}, 1, 100 });
                    m_collectedHandles.push_back(handle); 
                    m_engine.SendMsg(MsgObjectDestroy{ vve::ObjectHandle(handle) });
                    it = m_collectibleHandles.erase(it);
                    if (m_collectibleHandles.empty()) {
                        m_allCollected = true;
                    }

                    return true;
                } else {
                    ++it;
                }
            }

        
            return false; // no collision
        }
    
        // RELEVANT DETAILS:
        // constexpr uint16_t KEYDOWN_PORT = 8888;
        // std::atomic<bool> runInputThread{true};
        void StartInputListener() {
            char buffer[64];
            while (runInputThread.load()) {
                int recvLen = m_keyboardEventReceiver.receive_raw(buffer, sizeof(buffer) - 1);
                if (recvLen > 0) {
                    buffer[recvLen] = '\0';
                    uint32_t code;
                    float dt;

                    if (sscanf(buffer, "%u:%f", &code, &dt) == 2) {  
                        // std::cout << "[Receiver Input] Key code: " << code << " | dt: " << dt << "s\n";

                        SDL_Event e{};
                        e.type = SDL_KEYDOWN;
                        e.key.type = SDL_KEYDOWN;
                        e.key.timestamp = SDL_GetTicks();
                        e.key.windowID = 0; // 0 = default window
                        e.key.state = SDL_PRESSED;
                        e.key.repeat = 0;
                        e.key.keysym.scancode = static_cast<SDL_Scancode>(code); // set this too
                        e.key.keysym.sym = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(code));
                        e.key.keysym.mod = KMOD_NONE;
                        SDL_Scancode key = static_cast<SDL_Scancode>(code);
                        this->HandleRemoteKey(key, dt);

                        // SDL_PushEvent(&e);
                        // std::cout << "Pushed SDL_KEYDOWN for keycode: " << code << "\n";
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            // m_keyboardEventReceiver.closeSock();
        }

        // part of StartInputListener()
        void HandleRemoteKey(SDL_Scancode key, float dt) {
            PlayerData playerData = {};
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            auto& playerRot = m_registry.Get<vve::Rotation&>(m_playerHandle)();

            float moveSpeed = dt * 5.0f;
            float rotSpeed = glm::radians(90.0f) * dt;

            glm::vec3 moveDir{0.0f};
            glm::vec3 forward = playerRot * glm::vec3(0.0f, 0.0f, 1.0f);
            glm::vec3 right   = playerRot * glm::vec3(1.0f, 0.0f, 0.0f);

            switch (key) {
                case SDL_SCANCODE_W: moveDir += forward; m_playerState = PlayerState::MOVING; break;
                case SDL_SCANCODE_S: moveDir -= forward; m_playerState = PlayerState::MOVING; break;
                case SDL_SCANCODE_A: moveDir += right; m_playerState = PlayerState::MOVING; break;
                case SDL_SCANCODE_D: moveDir -= right; m_playerState = PlayerState::MOVING; break;
                case SDL_SCANCODE_LEFT:
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), rotSpeed, glm::vec3(0.0f, 1.0f, 0.0f)));
                    break;
                case SDL_SCANCODE_RIGHT:
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), -rotSpeed, glm::vec3(0.0f, 1.0f, 0.0f)));
                    break;
                case SDL_SCANCODE_ESCAPE:
                    m_engine.Stop();
                    break;
            }

            if (glm::length(moveDir) > 0.0f) {
                glm::vec3 proposedPos = playerPos + glm::normalize(moveDir) * moveSpeed;
                if (!CheckCollision(proposedPos)) {
                    playerPos = proposedPos;
                    // m_engine.SendMsg(vve::System::MsgPlaySound{ vve::Filename{"../escape/assets/sounds/bump.wav"}, 1, 100 });
                }
            } else {
                m_playerState = PlayerState::STATIONARY;
            }
        }

        // RELEVANT DETAILS:
        // constexpr uint16_t LIGHTDATA_PORT = 9998;
        // std::atomic<bool> runLightThread{true};
        void StartLightUpdateListener() {
            while (runLightThread.load()) {
                char buffer[64];  
                char type[17] = {};  
                memcpy(type, buffer, 16);  
                type[16] = '\0';
                std::string typeStr(type);

                int recvLen = m_lightEventReceiver.receive_raw(buffer, sizeof(buffer));

                if (recvLen > 0) {
                    if (typeStr == "Spot" && recvLen == sizeof(SpoLightData)) {
                        const SpoLightData* data = reinterpret_cast<const SpoLightData*>(buffer);
                        {
                            std::lock_guard<std::mutex> lock(m_spotLightMutex);
                            m_receivedSpotUpdate = *data;  // store the update for next frame
                            // std::cout << "[light event receiver] Spot Light Data Position: " << data->pos[0] << data->pos[1] << data->pos[2] << std::endl;
                            // std::cout << "[light event receiver] Spot Light Data Rotation: " << data->rot[0] << data->rot[1] << data->rot[2] << std::endl;
                            // std::cout << "[light event receiver] Spot Light Data Rotation: " << data->color[0] << data->color[1] << data->color[2] << data->color[3] << std::endl;
                        }

                    } else if (typeStr == "Point" && recvLen == sizeof(PoiLightData)) {
                        const PoiLightData* data = reinterpret_cast<const PoiLightData*>(buffer);

                        {
                            std::lock_guard<std::mutex> lock(m_pointLightMutex);
                            m_receivedPointUpdate = *data;  // store the update for next frame
                            std::cout << "[light event receiver] Point Light Data Position: " << data->pos[0] << data->pos[1] << data->pos[2] << std::endl;
                            std::cout << "[light event receiver] Point Light Data Rotation: " << data->color[0] << data->color[1] << data->color[2] << data->color[3] << std::endl;
                        }

                    } else if (typeStr == "Directional" && recvLen == sizeof(DirLightData)) {
                        const DirLightData* data = reinterpret_cast<const DirLightData*>(buffer);

                        {
                            std::lock_guard<std::mutex> lock(m_directionalLightMutex);
                            m_receivedDirectionalUpdate = *data;  // store the update for next frame
                            std::cout << "[light event receiver] Directional Light Data Rotation: " << data->color[0] << data->color[1] << data->color[2] << data->color[3] << std::endl;
                        }

                    } else {
                        std::cerr << "[LightReceiver] Unknown light type or wrong size: " << recvLen << "\n";
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            m_lightEventReceiver.closeSock();  // Optional
        }

        ImGuiPacket latestImGuiPacket = {}; 

        // --- Callbacks ---
        bool OnLoadLevel(Message message) {
            std::srand(static_cast<unsigned int>(std::time(nullptr))); 
            auto msg = message.template GetData<vve::System::MsgLoadLevel>();
            // std::cout << "Loading level: " << msg.m_level << std::endl;
    
            // initialise plane
            m_engine.SendMsg(vve::System::MsgSceneLoad{ vve::Filename{plane_obj}, aiProcess_FlipWindingOrder });
    
            auto m_handlePlane = m_registry.Insert(
                vve::Position{ {0.0f,0.0f,0.0f } },
                vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), 3.14159f / 2.0f, glm::vec3(1.0f,0.0f,0.0f))) },
                vve::Scale{ vec3_t{1000.0f,1000.0f,1000.0f} },
                vve::MeshName{ plane_mesh },
                vve::TextureName{ plane_txt },
                vve::UVScale{ { 1000.0f, 1000.0f } }
            );
    
            m_engine.SendMsg(MsgObjectCreate{ vve::ObjectHandle(m_handlePlane), vve::ParentHandle{} });
    
            // initialise player
            glm::mat3 p_rotation = glm::mat3( glm::rotate( glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0,0,1)) * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0) ) );
            glm::vec3 p_position = RandomWalkablePosition();
            m_playerHandle = m_registry.Insert(
                vve::Position{ p_position },
                vve::Rotation{ p_rotation },
                vve::Scale{ vec3_t{1.0f} },
                vve::Name{ "Player" }
            );
            // m_registry.AddTags(m_playerHandle, static_cast<size_t>(Tags::Tag_Retrievable));
            
            m_engine.SendMsg(MsgSceneCreate{
                vve::ObjectHandle(m_playerHandle), vve::ParentHandle{}, vve::Filename{player_obj}
            });

            // initialise camera
            GetCamera();
            m_engine.SendMsg(MsgObjectSetParent{
                vve::ObjectHandle{m_cameraNodeHandle},
                vve::ParentHandle{m_playerHandle}
            });
            m_registry.AddTags(m_cameraNodeHandle, static_cast<size_t>(Tags::Tag_Retrievable));
            
            m_registry.Get<vve::Position&>(m_cameraNodeHandle)() = glm::vec3(0.0f, 1.0f, -1.0f);
            
            auto& camPos = m_registry.Get<vve::Position&>(m_cameraNodeHandle)();
            glm::vec3 target = m_registry.Get<vve::Position&>(m_playerHandle)();
            glm::vec3 offsetTarget = target + glm::vec3(0.0f, 0.5f, 1.0f); 
            glm::mat4 viewMatrix = glm::lookAt(camPos, offsetTarget, glm::vec3(0.0f, 1.0f, 1.0f));
            m_registry.Get<vve::Rotation&>(m_cameraHandle)() = glm::mat3(glm::inverse(viewMatrix));

            // initialise map
            LoadMapAndSpawnWalls("../escape/assets/maps/map1.txt");

            // initialise collectibles + preparing data for imgui window #4
            CollectibleData collectible_data = SpawnCollectibles();

            // initialise bgm
            m_engine.SendMsg(vve::System::MsgPlaySound{ vve::Filename{"../escape/assets/sounds/anxious.wav"}, 2, 100 });
            m_engine.SendMsg(vve::System::MsgSetVolume{ static_cast<int>(m_volume) });
            
            // initialise UDP sender
            m_UDPsender.init(FFMPEG_PORT);
            m_LightSender.init(LIGHTDATA_PORT);
            m_PlayerDataSender.init(PLAYERDATA_PORT);
            m_CollectibleSender.init(COLLECTIBLE_PORT);
            m_GameStateSender.init(GAMESTATE_PORT);
            // m_registry.Print();

            // initialise UDP receiver
            m_lightEventReceiver.init(LIGHTUPDATE_PORT);
            m_keyboardEventReceiver.init(KEYDOWN_PORT);
            std::thread listener(&MyGame::StartInputListener, this);
            listener.detach();
            std::thread lightListener(&MyGame::StartLightUpdateListener, this);
            lightListener.detach();
            // CollectiblePacket colPacket = BuildCollectiblePacket();
            // m_CollectibleSender.send_raw(reinterpret_cast<const char*>(&colPacket), sizeof(colPacket));
            // std::cout << "[Sender] Sent CollectiblePacket of size: " << sizeof(colPacket) << "\n";

            // data for initialisation of GUI window #1
            SceneData scene_data = {};
            size_t obj_idx = 0;

            auto retrievables = m_registry.GetView<vve::Name, vve::Position>(std::vector<size_t>{Tags::Tag_Retrievable});
            for (auto [name, pos] : retrievables) {
                if (obj_idx >= 100) break;
                Object obj;

                // assigning obj.name
                // Step 1: Convert name (e.g., strong type) into std::string
                std::string nameStr = static_cast<std::string>(name);  // Adjust if your strong type differs

                // std::cout << "Item retrieved: " << nameStr << std::endl;
                // Step 2: Copy safely into fixed-size C-style array
                strncpy(obj.name, nameStr.c_str(), sizeof(obj.name));
                obj.name[sizeof(obj.name) - 1] = '\0';  // Ensure null termination
                
                // assigning obj.pos
                obj.pos[0] = pos().x;
                obj.pos[1] = pos().y;
                obj.pos[2] = pos().z;

                // adding obj to scene_data
                if (obj_idx < 100) {
                    scene_data.objects[obj_idx] = obj;
                    obj_idx++;
                }
            }

            scene_data.obj_count = static_cast<uint16_t>(obj_idx);
            // scene_data ready to be exported - imgui window #1

            // data for initialisation of GUI window #2
            PlayerData player_data;
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            player_data.pos[0] = playerPos.x;
            player_data.pos[1] = playerPos.y;
            player_data.pos[2] = playerPos.z;

            player_data.moving = false;
            // player_data ready to be exported - imgui window #2

            // data for initialisation of GUI window #3 
            LightData light_data;
            // 1. retrieve Spot Light 
            SpoLightData SpoLight;
            for (auto [handle, name, position, rotation, spotLight] : m_registry.GetView<vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::SpotLight&>()) {
                strcpy(SpoLight.type, "Spot"); 

                // spot light position
                auto& rawPos = position();
                SpoLight.pos[0] = rawPos.x;
                SpoLight.pos[1] = rawPos.y;
                SpoLight.pos[2] = rawPos.z;
                
                // spot light rotation
                auto& rawRot = rotation();
                glm::vec3 eulerAngles = glm::eulerAngles(glm::quat_cast(rawRot)); // radians

                SpoLight.rot[0] = glm::degrees(eulerAngles.x);
                SpoLight.rot[1] = glm::degrees(eulerAngles.y);
                SpoLight.rot[2] = glm::degrees(eulerAngles.z);

                // spot light color
                auto& p = spotLight();
                SpoLight.color[0] = p.color.r;
                SpoLight.color[1] = p.color.g;
                SpoLight.color[2] = p.color.b;
                SpoLight.color[3] = 1.0f;
            }
            // 2. assign spot light to light data
            light_data.spot_light = SpoLight;
            // spot light ready

            // 1. retrieve Point Light 
            PoiLightData PoiLight;
            for (auto [handle, name, position, rotation, pointLight] : m_registry.GetView<vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::PointLight&>()) {
                strcpy(PoiLight.type, "Point"); 
                // point light position
                auto& rawPos = position();
                PoiLight.pos[0] = rawPos.x;
                PoiLight.pos[1] = rawPos.y;
                PoiLight.pos[2] = rawPos.z;

                // point light color
                auto& s = pointLight();
                PoiLight.color[0] = s.color.r;
                PoiLight.color[1] = s.color.g;
                PoiLight.color[2] = s.color.b;
                PoiLight.color[3] = 1.0f;
            }
            // 2. assign point light to light data
            light_data.point_light = PoiLight;
            // point light ready
            
            // 1. retrieve Directional Light 
            DirLightData DirLight;
            for (auto [handle, name, dirLight] : m_registry.GetView<vecs::Handle, vve::Name, vve::DirectionalLight&>()) {
                strcpy(DirLight.type, "Directional"); 
                // directional light color
                auto& d = dirLight();
                DirLight.color[0] = d.color.r;
                DirLight.color[1] = d.color.g;
                DirLight.color[2] = d.color.b;
                DirLight.color[3] = 1.0f;
            }
            // 2. assign directional light to light data
            light_data.directional_light = DirLight;
            // directional light ready
            // light_data ready to be exported - imgui window #3

            
            // auto collectibles = m_registry.GetView<vve::Name>

            ImGuiPacket packet = {};
            packet.scene = scene_data;
            packet.player = player_data;
            packet.lights = light_data;
            packet.collectibles = collectible_data;
            // std::cout << "[line 1246] number of types of collectible data: " << packet.collectibles.collectible_count << std::endl;
            
            m_LightSender.send_raw(reinterpret_cast<const char*>(&packet), sizeof(packet));
            // std::cout << "[Sender] Sent ImGuiPacket of size: " << sizeof(packet) << "\n";

            return false;
        }
    
        bool OnUpdate(Message& message) {
            auto msg = message.GetData<vve::System::MsgUpdate>();
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            auto& playerRot = m_registry.Get<vve::Rotation&>(m_playerHandle)();
            auto& playerState = m_playerState;
            vecs::Handle exit_handle{};
            if (m_allCollected) {
                std::cout << "CONGRATS ON COMPLETION~" << std::endl;
                vec3_t position;
                for (const auto& [handle, name, pos] : m_registry.GetView<vecs::Handle, vve::Name, vve::Position>(std::vector<size_t>{Tags::Tag_Exit})) {
                    std::cout << name() << std::endl;
                    exit_handle = handle;
                    position = pos();
                    std::cout << "position x: " << position.x << std::endl;
                }
                m_allCollected = false;
                m_engine.SendMsg(MsgObjectDestroy{ vve::ObjectHandle(exit_handle) });
                auto it = std::find(m_objectHandles.begin(), m_objectHandles.end(), exit_handle);
                if (it != m_objectHandles.end()) {
                    m_objectHandles.erase(it);
                }
                std::cout << "exit deleted!" << std::endl;
                exit_handle = m_registry.Insert(
                    vve::Position{ position },
                    vve::Rotation{ glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0))) },
                    vve::Scale{ vec3_t{1.0f} },
                    vve::Name{ "Exit Broken" }
                );
                m_engine.SendMsg(MsgSceneCreate{
                        vve::ObjectHandle(exit_handle),
                        vve::ParentHandle{},
                        vve::Filename{ broken_door_obj },
                        aiProcess_FlipWindingOrder
                    });
            }
            
            CollectibleData updatedCollectibleData = {};
            uint16_t idx = 0;

            for (const auto& [handle, nameComp] : m_registry.GetView<vecs::Handle, vve::Name>(std::vector<size_t>{Tags::Tag_Collectible})) {
                std::string fullName = static_cast<std::string>(nameComp);
                std::string name = fullName.substr(0, fullName.find(' '));  
                bool found = false;

                // Check if this baseName already exists
                for (uint16_t i = 0; i < idx; ++i) {
                    if (name == updatedCollectibleData.collectible[i].name) {
                        updatedCollectibleData.collectible[i].count++;
                        found = true;
                        break;
                    }
                }

                // If not found, create new entry (if there's still room)
                if (!found && idx < 10) {
                    strncpy(updatedCollectibleData.collectible[idx].name, name.c_str(), sizeof(updatedCollectibleData.collectible[idx].name));
                    updatedCollectibleData.collectible[idx].name[sizeof(updatedCollectibleData.collectible[idx].name) - 1] = '\0'; // ensure null termination
                    updatedCollectibleData.collectible[idx].count = 1;
                    idx++;
                }
            }

            updatedCollectibleData.collectible_count = idx;
            // std::cout << "======================== NEW ON UPDATE ========================" << std::endl;
            // for (int i = 0; i < updatedCollectibleData.collectible_count; ++i) {
            //     std::cout << "Collectible Name: " << updatedCollectibleData.collectible[i].name
            //             << ", Count: " << updatedCollectibleData.collectible[i].count << std::endl;
            // }
            // std::cout << "this is the number of types of data: " << updatedCollectibleData.collectible_count << std::endl;

            // std::cout << "======================== end ========================" << std::endl;
            m_CollectibleSender.send_raw(reinterpret_cast<const char*>(&updatedCollectibleData), sizeof(updatedCollectibleData));
            return false;
        }
    
        bool OnRecordNextFrame(Message message) {
            if (m_receivedSpotUpdate.has_value()) {
                std::lock_guard<std::mutex> lock(m_spotLightMutex);

                const auto& data = m_receivedSpotUpdate.value();
                for (auto [handle, name, position, rotation, spotLight] :
                    m_registry.GetView<vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::SpotLight&>()) {

                    auto& currentPos = m_registry.Get<vve::Position&>(handle)();
                    currentPos = glm::make_vec3(data.pos);

                    auto& currentRot = m_registry.Get<vve::Rotation&>(handle)();
                    glm::vec3 newAngles = glm::radians(glm::vec3(data.rot[0], data.rot[1], data.rot[2]));
                    glm::quat q = glm::quat(newAngles);
                    currentRot = glm::mat3_cast(q);

                    auto& p = spotLight();
                    p.color.r = data.color[0];
                    p.color.g = data.color[1];
                    p.color.b = data.color[2];

                    // std::cout << "[OnRecordNextFrame] Applied Spot Light update.\n";
                    // std::cout << "                    Position: " << data.pos[0] << data.pos[1] << data.pos[2] << std::endl;
                    // std::cout << "                    Rotation: " << data.rot[0] << data.rot[1] << data.pos[2] << std::endl;
                    break;
                }

                m_receivedSpotUpdate.reset();  // clear after use
            }
            if (m_receivedPointUpdate.has_value()) {
                std::lock_guard<std::mutex> lock(m_pointLightMutex);

                const auto& data = m_receivedPointUpdate.value();
                for (auto [handle, name, position, rotation, pointLight] :
                    m_registry.GetView<vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::PointLight&>()) {

                    auto& currentPos = m_registry.Get<vve::Position&>(handle)();
                    currentPos = glm::make_vec3(data.pos);

                    auto& p = pointLight();
                    p.color.r = data.color[0];
                    p.color.g = data.color[1];
                    p.color.b = data.color[2];

                    // std::cout << "[OnRecordNextFrame] Applied Point Light update.\n";
                    // std::cout << "                    Position: " << data.pos[0] << data.pos[1] << data.pos[2] << std::endl;
                    break;
                }

                m_receivedPointUpdate.reset();  // clear after use
            }
            if (m_receivedDirectionalUpdate.has_value()) {
                std::lock_guard<std::mutex> lock(m_directionalLightMutex);

                const auto& data = m_receivedDirectionalUpdate.value();
                for (auto [handle, name, position, rotation, dirLight] :
                    m_registry.GetView<vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::DirectionalLight&>()) {

                    auto& p = dirLight();
                    p.color.r = data.color[0];
                    p.color.g = data.color[1];
                    p.color.b = data.color[2];

                    // std::cout << "[OnRecordNextFrame] Applied Point Light update.\n";
                    break;
                }

                m_receivedDirectionalUpdate.reset();  // clear after use
            }

            PlayerData updatedPlayerData = {};
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            if (m_playerState == PlayerState::MOVING) {
                updatedPlayerData.moving = true;
            } else {
                updatedPlayerData.moving = false;
            }
            updatedPlayerData.pos[0] = playerPos.x;
            updatedPlayerData.pos[1] = playerPos.y;
            updatedPlayerData.pos[2] = playerPos.z;

            m_PlayerDataSender.send_raw(reinterpret_cast<const char*>(&updatedPlayerData), sizeof(updatedPlayerData));
            // std::cout << "updatedPlayerData.moving" << updatedPlayerData.moving << std::endl;
            // std::cout << "updatedPlayerData.pos[0]" << updatedPlayerData.pos[0] << std::endl;
            // std::cout << "updatedPlayerData.pos[1]" << updatedPlayerData.pos[1] << std::endl;
            // std::cout << "updatedPlayerData.pos[2]" << updatedPlayerData.pos[2] << std::endl;
            // static bool showSceneObjects = false;
            // static bool showSettings = true;

            // if (showSceneObjects && showSettings) {
            //     showSettings = false;
            // }
            // if (showSettings && showSceneObjects) {
            //     showSceneObjects = false;
            // }


            // ImGui::SetNextWindowPos(ImVec2(10, 10));
            // ImGui::SetNextWindowSize(ImVec2(300, 540));
            // ImGui::SetNextWindowCollapsed(!showSceneObjects, ImGuiCond_Once);
            // ImGui::Begin("Scene Objects");
            // if (ImGui::IsWindowCollapsed()) showSceneObjects = false;
            // else showSceneObjects = true;
        
            // auto retrievables = m_registry.GetView<vve::Name, vve::Position>(std::vector<size_t>{Tags::Tag_Retrievable});
            // for (auto [name, pos] : retrievables) {
            //     std::string objName = name;
            //     auto& objPos = pos;
            //     ImGui::Text("%s", objName.c_str());
            //     ImGui::Text("  Position: (%.2f, %.2f, %.2f)", objPos().x, objPos().y, objPos().z);
            //     ImGui::Separator();
            // }
            
            // ImGui::End();

            // ImGui::SetNextWindowPos(ImVec2(10, 30));
            // ImGui::SetNextWindowSize(ImVec2(300, 520));
            // ImGui::SetNextWindowCollapsed(!showSettings, ImGuiCond_Once);
            // ImGui::Begin("Scene Settings");
            // if (ImGui::IsWindowCollapsed()) showSettings = false;
            // else showSettings = true;

            // ImGui::SeparatorText("Player Status");
            // const char* stateText = (m_playerState == PlayerState::MOVING) ? "MOVING" : "STATIONARY";
            // ImGui::Text("State: %s", stateText);
            // auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            // ImGui::Text("player z coordinate: %.2f", playerPos.y);
            // ImGui::Text("player x coordinate: %.2f", playerPos.x);

            // ImGui::SeparatorText("Light Settings");
            // if (ImGui::BeginTabBar("Light Settings")) {
            //     if (ImGui::BeginTabItem("Point Light")) {
            //         for (auto [handle, name, position, rotation, pointLight] : m_registry.GetView<
            //         vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::PointLight&>()) {
            //             auto& p = pointLight();

            //             auto& rawPos = position();
            //             float pos[3] = { rawPos.x, rawPos.y, rawPos.z };

            //             if (ImGui::SliderFloat3(("Position##" + std::string(name)).c_str(), pos, -50.0f, 50.0f)) {
            //                 rawPos.x = pos[0];
            //                 rawPos.y = pos[1];
            //                 rawPos.z = pos[2];
            //             }
                        
            //             ImGui::Text("");

            //             auto& rawRot = rotation();

            //             glm::vec3 eulerAngles = glm::eulerAngles(glm::quat_cast(rawRot)); // radians

            //             float degrees[3] = {
            //                 glm::degrees(eulerAngles.x),
            //                 glm::degrees(eulerAngles.y),
            //                 glm::degrees(eulerAngles.z)
            //             };

            //             std::string rotLabel = "Rotation##" + static_cast<std::string>(name);
            //             if (ImGui::SliderFloat3(rotLabel.c_str(), degrees, -180.0f, 180.0f)) {
            //                 glm::vec3 newAngles = glm::radians(glm::vec3(degrees[0], degrees[1], degrees[2]));
            //                 glm::quat q = glm::quat(newAngles);
            //                 rawRot = glm::mat3_cast(q);  // update spotlight rotation
            //             }
                        
            //             ImGui::Text("");

            //             float color[4] = { p.color.r, p.color.g, p.color.b, 1.0f };
            //             std::string label = "Color##" + static_cast<std::string>(name);
            //             if (ImGui::ColorPicker4(label.c_str(), color)) {
            //                 p.color.r = color[0]; p.color.g = color[1]; p.color.b = color[2];
            //             }
            //         }
            //         ImGui::EndTabItem();
            //     }

            //     if (ImGui::BeginTabItem("Spot Light")) {
            //         for (auto [handle, name, position, rotation, spotLight] : m_registry.GetView<
            //         vecs::Handle, vve::Name, vve::Position, vve::Rotation, vve::SpotLight&>()) {

            //             auto& d = spotLight();

            //             auto& rawPos = position();
            //             float pos[3] = { rawPos.x, rawPos.y, rawPos.z };

            //             if (ImGui::SliderFloat3(("Position##" + std::string(name)).c_str(), pos, -50.0f, 50.0f)) {
            //                 rawPos.x = pos[0];
            //                 rawPos.y = pos[1];
            //                 rawPos.z = pos[2];
            //             }
                        
            //             ImGui::Text("");

            //             auto& rawRot = rotation();

            //             glm::vec3 eulerAngles = glm::eulerAngles(glm::quat_cast(rawRot)); // radians

            //             float degrees[3] = {
            //                 glm::degrees(eulerAngles.x),
            //                 glm::degrees(eulerAngles.y),
            //                 glm::degrees(eulerAngles.z)
            //             };

            //             std::string rotLabel = "Rotation##" + static_cast<std::string>(name);
            //             if (ImGui::SliderFloat3(rotLabel.c_str(), degrees, -180.0f, 180.0f)) {
            //                 glm::vec3 newAngles = glm::radians(glm::vec3(degrees[0], degrees[1], degrees[2]));
            //                 glm::quat q = glm::quat(newAngles);
            //                 rawRot = glm::mat3_cast(q);  // update spotlight rotation
            //             }
                        
            //             ImGui::Text("");

            //             float color[4] = { d.color.r, d.color.g, d.color.b, 1.0f };
            //             std::string label = "Color##" + static_cast<std::string>(name);
            //             if (ImGui::ColorPicker4(label.c_str(), color)) {
            //                 d.color.r = color[0]; d.color.g = color[1]; d.color.b = color[2];
            //             }
            //         }
            //         ImGui::EndTabItem();
            //     }

            //     if (ImGui::BeginTabItem("Directional Light")) {
            //         for (auto [handle, name, position, dirLight] : m_registry.GetView<
            //         vecs::Handle, vve::Name, vve::Position, vve::DirectionalLight&>()) {
            //             auto& d = dirLight();
                        
            //             float color[4] = { d.color.r, d.color.g, d.color.b, 1.0f };
            //             std::string label = "Color##" + static_cast<std::string>(name);
            //             if (ImGui::ColorPicker4(label.c_str(), color)) {
            //                 d.color.r = color[0]; d.color.g = color[1]; d.color.b = color[2];
            //             }
            //         }
            //         ImGui::EndTabItem();
            //     }

            //     ImGui::EndTabBar();
            // }

            // ImGui::End();
            return false;
        }

        bool OnFrameEnd(Message& message) {
            auto [rhandle, renderer] = vve::Renderer::GetState(m_registry);
            auto [whandle, window] = vve::Window::GetState(m_registry, "");

            if (!renderer.IsValid() || !window.IsValid()) return false;

            VkExtent2D extent = {
                static_cast<uint32_t>(window().m_width),
                static_cast<uint32_t>(window().m_height)
            };
            
            uint32_t imageSize = extent.width * extent.height * 4;
            VkImage image = renderer().m_swapChain.m_swapChainImages[renderer().m_imageIndex];

            uint8_t* dataImage = new uint8_t[imageSize];

            vh::ImgCopyImageToHost(
                renderer().m_device,
                renderer().m_vmaAllocator,
                renderer().m_graphicsQueue,
                renderer().m_commandPool,
                image,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                dataImage,
                extent.width,
                extent.height,
                imageSize,
                2, 1, 0, 3
            );

            if (!m_ffmpegWriter) {
                m_ffmpegWriter = std::make_unique<FFmpegWriter>(extent.width, extent.height, "C:/Users/aanny/source/repos/fork/escape/videos/recording.h264");
            }

            m_ffmpegWriter->WriteFrame(dataImage);

            // After copying image to dataImage
            if (!m_ffmpegEncoder) {
                m_ffmpegEncoder = std::make_unique<FFmpegEncoder>(extent.width, extent.height);
            }
            
            auto [encodedData, encodedSize] = m_ffmpegEncoder->EncodeFrame(dataImage);
            if (encodedData && encodedSize > 0) {
                // std::cout << "Sending H264 frame: " << encodedSize << " bytes\n";
                m_UDPsender.send_fragmented((char*)encodedData, encodedSize);
                m_ffmpegEncoder->FreePacket();
            }
            
            // std::vector<uint8_t> encoded = m_udpSender.compress(dataImage, extent.width, extent.height);

            // m_udpSender.send((char*)encoded.data(), encoded.size());
            delete[] dataImage;

            m_frameLimiter.Wait();

            return true;

        }
        
        bool OnKeyDown(Message& message) {
            GetCamera();
        
            // Get component references
            auto [pn, rn, sn, LtoPn] = m_registry.template Get<vve::Position&, vve::Rotation&, vve::Scale&, vve::LocalToParentMatrix>(m_cameraNodeHandle);
            auto [pc, rc, sc, LtoPc] = m_registry.template Get<vve::Position&, vve::Rotation&, vve::Scale&, vve::LocalToParentMatrix>(m_cameraHandle);
    
            auto& playerPos = m_registry.Get<vve::Position&>(m_playerHandle)();
            auto& playerRot = m_registry.Get<vve::Rotation&>(m_playerHandle)();
        
            int key; 
            float dt;
            if (message.HasType<MsgKeyDown>()) {
                auto msg = message.template GetData<MsgKeyDown>();
                key = msg.m_key;
                dt = msg.m_dt;
                
            } else {
                auto msg = message.template GetData<MsgKeyRepeat>();
                key = msg.m_key;
                dt = msg.m_dt;
            }
        
            float moveSpeed = dt * 5.0f;
            float rotSpeed = glm::radians(90.0f) * dt; // 90 degrees/sec
        
            glm::vec3 moveDir{0.0f};
        
            // Define local movement directions
            glm::vec3 localForward = glm::vec3(0.0f, 0.0f, 1.0f); // +Z
            glm::vec3 localRight   = glm::vec3(1.0f, 0.0f, 0.0f); // +X
        
            // Convert local directions into world directions
            glm::vec3 forward = playerRot * localForward;
            glm::vec3 right   = playerRot * localRight;
            // glm::vec3 c_forward = camNodeRot * localForward;
            // glm::vec3 c_right   = camNodeRot * localRight;
    
            // for Camera
            glm::vec3 translate(0.0f);
            glm::vec3 axis1(1.0f), axis2(1.0f);
            float angle1 = 0.0f, angle2 = 0.0f;
            int dx = 0, dy = 0;
            
    
            switch (key) {
                case SDL_SCANCODE_W:
                    moveDir += forward;
                    break;
                case SDL_SCANCODE_S:
                    moveDir -= forward;
                    break;
                case SDL_SCANCODE_A:
                    moveDir += right;
                    break;
                case SDL_SCANCODE_D:
                    moveDir -= right;
                    break;
            
                case SDL_SCANCODE_LEFT: {
                    glm::vec3 rotAxis = glm::vec3(0.0f, 1.0f, 0.0f); // rotate around Z
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), rotSpeed, rotAxis));
                    dx = -1;
                    break;
                }
                case SDL_SCANCODE_RIGHT: {
                    glm::vec3 rotAxis = glm::vec3(0.0f, 1.0f, 0.0f); // rotate around Z
                    playerRot = glm::mat3(glm::rotate(glm::mat4(playerRot), -rotSpeed, rotAxis));
                    dx = 1;
                    break;
                }
                case SDL_SCANCODE_SPACE: {
                    if (message.HasType<MsgKeyRepeat>()) break;
    
                    // if (m_cubeCollected > 0) {
                    //     glm::vec3 forward = playerRot * glm::vec3(0.0f, 0.0f, 1.0f);
                    //     glm::vec3 placePos = playerPos + forward * 1.5f + glm::vec3(0.0f, 0.0f, 0.5f); // 1.5 units ahead of player
    
                    //     vecs::Handle placedCube = m_registry.Insert(
                    //         vve::Position{ placePos },
                    //         vve::Rotation{ mat3_t{1.0f} },
                    //         vve::Scale{ vec3_t{1.0f} }
                    //     );
    
                    //     m_objectHandles.push_back(placedCube);
    
                    //     m_engine.SendMsg(MsgSceneCreate{
                    //         vve::ObjectHandle(placedCube), vve::ParentHandle{}, vve::Filename{cube_obj}, aiProcess_FlipWindingOrder
                    //     });
                    //     m_engine.SendMsg(MsgPlaySound{ vve::Filename{"assets/sounds/putdown.wav"}, 1, 50 });
    
                    //     m_cubeCollected--; 
                    // }
                    break;
                }
    
                case SDL_SCANCODE_ESCAPE: {
                    const char* shutdownMsg = "__SHUTDOWN__";
                    m_UDPsender.send_fragmented((char*)shutdownMsg, strlen(shutdownMsg));
                    m_engine.Stop();
                    break;
                }                
            }
    
            if (glm::length(moveDir) > 0.0f) {
                moveDir = glm::normalize(moveDir) * moveSpeed;
                glm::vec3 proposedPos = playerPos + moveDir;
                m_playerState = PlayerState::MOVING;
                bool collision = CheckCollision(proposedPos);
                if (!collision) {
                    playerPos = proposedPos;
                    m_lastCollisionState = false; // no collision now
                } else {
                    
                    m_playerState = PlayerState::STATIONARY;
                    if (!m_lastCollisionState) { // only if new collision!
                        m_engine.SendMsg(MsgPlaySound{ vve::Filename{"assets/sounds/bump.wav"}, 1, 100 });
                        std::cout << "boink" << std::endl;
                    }
                    m_lastCollisionState = true; // remember that we are colliding now
                }
            }
            
            return true;
        }
    };

int main() {
    startWinsock();
    vve::Engine engine("My Engine", VK_MAKE_VERSION(1, 3, 0)) ;
    MyGame mygui{engine};  
    engine.Run();
    WSACleanup();
    return 0;
}
    
    