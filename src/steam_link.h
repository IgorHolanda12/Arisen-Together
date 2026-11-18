#pragma once
#include <cstdint>

#pragma pack(push, 8)
struct FriendGameInfo {
    uint64_t game_id;
    uint32_t ip;
    uint16_t port, query_port;
    uint64_t lobby;
};
struct P2PSessionState {
    uint8_t active, connecting, error, using_relay;
    int32_t bytes_queued, packets_queued;
    uint32_t remote_ip;
    uint16_t remote_port;
};
#pragma pack(pop)

struct SteamFns {
    void *net = nullptr, *user = nullptr, *friends = nullptr;
    uint64_t (*GetSteamID)(void *user) = nullptr;
    bool (*BLoggedOn)(void *user) = nullptr;
    bool (*SendP2PPacket)(void *net, uint64_t to, const void *data, uint32_t size, int type, int channel) = nullptr;
    bool (*IsP2PPacketAvailable)(void *net, uint32_t *size, int channel) = nullptr;
    bool (*ReadP2PPacket)(void *net, void *dest, uint32_t cap, uint32_t *size, uint64_t *from, int channel) = nullptr;
    bool (*AcceptP2PSessionWithUser)(void *net, uint64_t user) = nullptr;
    int (*GetFriendCount)(void *friends, int flags) = nullptr;
    uint64_t (*GetFriendByIndex)(void *friends, int index, int flags) = nullptr;
    bool (*GetFriendGamePlayed)(void *friends, uint64_t user, FriendGameInfo *info) = nullptr;
    const char *(*GetFriendRichPresence)(void *friends, uint64_t user, const char *key) = nullptr;
    bool (*SetRichPresence)(void *friends, const char *key, const char *value) = nullptr;
    void (*RequestFriendRichPresence)(void *friends, uint64_t user) = nullptr;
    bool (*CloseP2PSessionWithUser)(void *net, uint64_t user) = nullptr;
    bool (*GetP2PSessionState)(void *net, uint64_t user, P2PSessionState *state) = nullptr;
    void *utils = nullptr;
    const char *(*GetPersonaName)(void *friends) = nullptr;
    const char *(*GetFriendPersonaName)(void *friends, uint64_t user) = nullptr;
    int (*GetMediumFriendAvatar)(void *friends, uint64_t user) = nullptr;
    bool (*GetImageSize)(void *utils, int image, uint32_t *w, uint32_t *h) = nullptr;
    bool (*GetImageRGBA)(void *utils, int image, uint8_t *dest, int size) = nullptr;
};

bool steam_mock_fill(SteamFns &f);
