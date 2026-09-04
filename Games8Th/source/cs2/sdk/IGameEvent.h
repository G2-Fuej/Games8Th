#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#define STRINGTOKEN_MURMURHASH_SEED 0x31415926

inline uint32_t MurmurHash2(const void* key, int len, uint32_t seed) {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = seed ^ len;
    const unsigned char* data = (const unsigned char*)
key;

    while (len >= 4) {
        uint32_t k = *(uint32_t*)
data;
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        data += 4; len -= 4;
    }

    switch (len) {
    case 3: h ^= data[2] << 16; [[fallthrough]];
    case 2: h ^= data[1] << 8;  [[fallthrough]];
    case 1: h ^= data[0]; h *= m;
    }

    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

inline uint32_t MurmurHash2LowerCaseA(const char* pString, int len, uint32_t nSeed) {
    if (!pString || len <= 0) return 0;
    char stackBuf[256];
    char* p = (len < 256) ? stackBuf : new char[len + 1];
    for (int i = 0; i < len; i++) {
        const unsigned char c = static_cast<unsigned char>(pString[i]);
        p[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
    }
    uint32_t res = MurmurHash2(p, len, nSeed);
    if (p != stackBuf) delete[] p;
    return res;
}

class CUtlStringToken {
public:
    uint32_t m_nHashCode;
    uint32_t m_nUnknown = 0xFFFFFFFFu;
    const char* m_szDebugName;

    CUtlStringToken() : m_nHashCode(0), m_szDebugName(nullptr) {}

    CUtlStringToken(const char* szString) {
        m_nHashCode = szString ? MurmurHash2LowerCaseA(szString, static_cast<int>(std::
strlen(szString)), STRINGTOKEN_MURMURHASH_SEED) : 0;
        m_szDebugName = szString;
    }
};

class CCSPlayerController;

class IGameEvent {
public:
    const char* GetName();
    int64_t GetInt64(const std::
string_view name, int64_t defVal = 0);
    CCSPlayerController* GetPlayerController(const std::
string_view name);
    const char* GetString(const std::
string_view name, const char* defVal = "");
    float GetFloat(const std::
string_view name, float defVal = 0.0f);
    void SetString(const std::
string_view name, const std::
string_view value);

    static void InitPatterns();
};

