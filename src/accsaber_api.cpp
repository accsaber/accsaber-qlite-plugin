#include "qlite/accsaber_api.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "main.hpp"
#include "qlite/mod_config.hpp"

#include "libcurl/shared/curl.h"
#include "libcurl/shared/easy.h"

#include "beatsaber-hook/shared/rapidjson/include/rapidjson/document.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/stringbuffer.h"
#include "beatsaber-hook/shared/rapidjson/include/rapidjson/writer.h"

#include "UnityEngine/Application.hpp"

namespace AccSaberQLite::API
{

    namespace
    {

        constexpr std::string_view BASE_URL = "https://api.accsaber.com/v1";
        constexpr char const *SESSION_FILE =
            "/sdcard/ModData/com.beatgames.beatsaber/Mods/AccSaberQLite/accsaber_session_DO_NOT_SHARE.txt";

        std::mutex sessionMutex;
        std::string sessionAccessToken;
        std::string sessionRefreshToken;
        long long sessionExpiresAtUnix = 0;
        std::atomic<bool> loginInFlight = false;
        std::atomic<long long> loginStartedAt = 0;
        std::atomic<bool> needsRelogin = false;
        std::string cachedGameVersion;

        constexpr long long LOGIN_STALL_SECONDS = 120;

        long long NowUnix()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        std::string Base64(std::vector<uint8_t> const &bytes)
        {
            static constexpr char alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((bytes.size() + 2) / 3 * 4);
            std::size_t i = 0;
            while (i + 2 < bytes.size())
            {
                uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
                out.push_back(alphabet[(n >> 18) & 63]);
                out.push_back(alphabet[(n >> 12) & 63]);
                out.push_back(alphabet[(n >> 6) & 63]);
                out.push_back(alphabet[n & 63]);
                i += 3;
            }
            if (i + 1 == bytes.size())
            {
                uint32_t n = bytes[i] << 16;
                out.push_back(alphabet[(n >> 18) & 63]);
                out.push_back(alphabet[(n >> 12) & 63]);
                out.append("==");
            }
            else if (i + 2 == bytes.size())
            {
                uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8);
                out.push_back(alphabet[(n >> 18) & 63]);
                out.push_back(alphabet[(n >> 12) & 63]);
                out.push_back(alphabet[(n >> 6) & 63]);
                out.push_back('=');
            }
            return out;
        }

        std::vector<uint8_t> RandomBytes(std::size_t count)
        {
            std::vector<uint8_t> bytes(count);
            std::random_device rd;
            for (std::size_t i = 0; i < count; i += 4)
            {
                uint32_t r = rd();
                for (std::size_t j = 0; j < 4 && i + j < count; j++)
                {
                    bytes[i + j] = static_cast<uint8_t>(r >> (8 * j));
                }
            }
            return bytes;
        }

        std::string GenerateNonce()
        {
            return Base64(RandomBytes(64));
        }

        std::string const &InstallationId()
        {
            static std::string const id = []
            {
                std::string existing = getModConfig().InstallationId.GetValue();
                if (!existing.empty())
                    return existing;
                auto bytes = RandomBytes(16);
                static constexpr char hex[] = "0123456789abcdef";
                std::string generated;
                generated.reserve(32);
                for (uint8_t b : bytes)
                {
                    generated.push_back(hex[b >> 4]);
                    generated.push_back(hex[b & 15]);
                }
                getModConfig().InstallationId.SetValue(generated);
                return generated;
            }();
            return id;
        }

        std::string GameVersion()
        {
            return cachedGameVersion.empty() ? "unknown" : cachedGameVersion;
        }

        bool ParseJson(std::string const &body, rapidjson::Document &doc)
        {
            doc.Parse(body.c_str(), body.size());
            return !doc.HasParseError();
        }

        struct HttpResponse
        {
            long httpCode = 0;
            int curlCode = -1;
            std::string body;

            bool Ok() const { return curlCode == CURLE_OK && httpCode >= 200 && httpCode < 300; }
        };

        std::vector<std::string> ProtocolHeaders(bool withAuth)
        {
            std::vector<std::string> headers{
                "Content-Type: application/json",
                "Accept: application/json",
                "X-AccSaber-Plugin-Version: " VERSION,
                "X-AccSaber-Game-Version: " + GameVersion(),
                "X-AccSaber-Platform: quest",
                "X-AccSaber-Installation: " + InstallationId(),
            };
            if (withAuth)
            {
                std::lock_guard lock(sessionMutex);
                headers.push_back("Authorization: Bearer " + sessionAccessToken);
            }
            return headers;
        }

        std::size_t WriteToString(void *contents, std::size_t size, std::size_t nmemb, void *out)
        {
            std::size_t length = size * nmemb;
            static_cast<std::string *>(out)->append(static_cast<char *>(contents), length);
            return length;
        }

        HttpResponse HttpRequest(char const *method, std::string const &path, bool withAuth,
                                 std::string const *body)
        {
            HttpResponse response;
            CURL *curl = curl_easy_init();
            if (!curl)
                return response;

            std::string url = std::string(BASE_URL) + path;
            std::string userAgent = "AccSaberQLite/" VERSION " (BeatSaber " + GameVersion() + "; Quest)";
            curl_slist *headers = nullptr;
            for (auto const &line : ProtocolHeaders(withAuth))
            {
                headers = curl_slist_append(headers, line.c_str());
            }

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
            if (body)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

            response.curlCode = static_cast<int>(curl_easy_perform(curl));
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.httpCode);

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return response;
        }

        void PostJson(std::string path, bool withAuth, std::string body,
                    std::function<void(HttpResponse)> onFinished)
        {
            std::thread([path = std::move(path), withAuth, body = std::move(body),
                        onFinished = std::move(onFinished)]
                        { onFinished(HttpRequest("POST", path, withAuth, &body)); })
                .detach();
        }

        void GetJson(std::string path, bool withAuth, std::function<void(HttpResponse)> onFinished)
        {
            std::thread([path = std::move(path), withAuth, onFinished = std::move(onFinished)]
                        { onFinished(HttpRequest("GET", path, withAuth, nullptr)); })
                .detach();
        }

        std::mutex modifierMutex;
        std::unordered_map<std::string, double> modifierMultipliers;
        bool modifiersLoaded = false;

        bool EqualsIgnoreCase(std::string const &a, std::string const &b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); i++)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                {
                    return false;
                }
            }
            return true;
        }

        double MultiplierFromCache(std::vector<std::string> const &codes)
        {
            std::lock_guard lock(modifierMutex);
            double multiplier = 1.0;
            for (auto const &code : codes)
            {
                auto it = modifierMultipliers.find(code);
                if (it == modifierMultipliers.end())
                {
                    PaperLogger.warn("No multiplier known for modifier code {}", code);
                    continue;
                }
                multiplier += it->second - 1.0;
            }
            return multiplier;
        }

        std::string JsonEscape(std::string const &value)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.String(value.c_str(), value.size());
            return buffer.GetString();
        }

        bool StoreAuthResponse(std::string const &body)
        {
            rapidjson::Document doc;
            if (!ParseJson(body, doc) || !doc.IsObject())
                return false;
            if (!doc.HasMember("accessToken") || !doc["accessToken"].IsString())
                return false;
            if (!doc.HasMember("refreshToken") || !doc["refreshToken"].IsString())
                return false;

            std::lock_guard lock(sessionMutex);
            sessionAccessToken = doc["accessToken"].GetString();
            sessionRefreshToken = doc["refreshToken"].GetString();
            long long expiresIn = doc.HasMember("expiresIn") && doc["expiresIn"].IsInt64()
                                    ? doc["expiresIn"].GetInt64()
                                    : 0;
            sessionExpiresAtUnix = expiresIn > 0 ? NowUnix() + expiresIn - 60 : 0;

            getModConfig().RefreshToken.SetValue(sessionRefreshToken);
            needsRelogin = false;
            return true;
        }

        void AuthRequest(std::string path, std::string body, char const *context,
                        std::function<void(bool)> onDone)
        {
            PostJson(std::move(path), false, std::move(body),
                    [context, onDone = std::move(onDone)](HttpResponse response)
                    {
                        bool ok = response.Ok() && StoreAuthResponse(response.body);
                        if (ok)
                        {
                            PaperLogger.info("{} succeeded", context);
                        }
                        else
                        {
                            PaperLogger.warn("{} failed (http {})", context, response.httpCode);
                        }
                        onDone(ok);
                    });
        }

        void RefreshSession(std::function<void(bool)> onDone)
        {
            std::string refreshToken;
            {
                std::lock_guard lock(sessionMutex);
                refreshToken = sessionRefreshToken;
            }
            if (refreshToken.empty())
                refreshToken = getModConfig().RefreshToken.GetValue();
            if (refreshToken.empty())
            {
                onDone(false);
                return;
            }

            AuthRequest("/auth/refresh", "{\"refreshToken\":" + JsonEscape(refreshToken) + "}",
                        "Session refresh", [onDone = std::move(onDone)](bool ok)
                        {
                if (!ok) needsRelogin = true;
                onDone(ok); });
        }

        std::string TrimCopy(std::string value)
        {
            auto notSpace = [](unsigned char c)
            { return !std::isspace(c); };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
            value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
            return value;
        }

        void ImportSessionFileIfPresent()
        {
            std::ifstream file(SESSION_FILE);
            if (!file.is_open())
                return;
            std::string token((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
            std::remove(SESSION_FILE);

            token = TrimCopy(std::move(token));
            if (token.empty())
                return;

            getModConfig().RefreshToken.SetValue(token);
            {
                std::lock_guard lock(sessionMutex);
                sessionRefreshToken = token;
            }
            needsRelogin = false;
            PaperLogger.info("Imported baked session credential from installer");
        }

        std::string SerializePayload(ScorePayload const &payload, std::string const &nonce)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.StartObject();
            writer.Key("nonce");
            writer.String(nonce.c_str(), nonce.size());
            writer.Key("mapDifficultyId");
            writer.String(payload.mapDifficultyId.c_str(), payload.mapDifficultyId.size());
            writer.Key("score");
            writer.Uint(payload.score);
            writer.Key("scoreNoMods");
            writer.Uint(payload.scoreNoMods);
            auto writeOptional = [&writer](char const *key, std::optional<int> const &value)
            {
                if (!value.has_value())
                    return;
                writer.Key(key);
                writer.Int(*value);
            };
            writeOptional("maxCombo", payload.maxCombo);
            writeOptional("badCuts", payload.badCuts);
            writeOptional("misses", payload.misses);
            writeOptional("wallHits", payload.wallHits);
            writeOptional("bombHits", payload.bombHits);
            writeOptional("pauses", payload.pauses);
            writeOptional("streak115", payload.streak115);
            if (!payload.hmd.empty())
            {
                writer.Key("hmd");
                writer.String(payload.hmd.c_str(), payload.hmd.size());
            }
            writer.Key("modifierCodes");
            writer.StartArray();
            for (auto const &code : payload.modifierCodes)
            {
                writer.String(code.c_str(), code.size());
            }
            writer.EndArray();
            writer.Key("partial");
            writer.Bool(payload.partial);
            writer.EndObject();
            return buffer.GetString();
        }

        void AttemptSubmit(std::shared_ptr<ScorePayload> payload, int attemptsLeft, bool refreshed,
                        std::function<void(bool)> onDone)
        {
            std::string body = SerializePayload(*payload, GenerateNonce());
            PostJson("/submit", true, std::move(body),
                    [payload, attemptsLeft, refreshed, onDone = std::move(onDone)](
                        HttpResponse response)
                    {
                        if (response.Ok())
                        {
                            PaperLogger.info("Score submitted for difficulty {}", payload->mapDifficultyId);
                            onDone(true);
                            return;
                        }
                        if (response.httpCode == 401 && !refreshed)
                        {
                            RefreshSession([payload, attemptsLeft, onDone](bool ok)
                                            {
                        if (ok) {
                            AttemptSubmit(payload, attemptsLeft, true, onDone);
                        } else {
                            PaperLogger.error("Submit aborted: no valid session, re-login queued");
                            onDone(false);
                        } });
                            return;
                        }
                        if (response.httpCode == 409)
                        {
                            PaperLogger.warn("Submit rejected as duplicate nonce");
                            onDone(false);
                            return;
                        }
                        if (response.httpCode == 429 && attemptsLeft > 0)
                        {
                            PaperLogger.warn("Rate limited, retrying in 61s ({} attempts left)", attemptsLeft);
                            std::this_thread::sleep_for(std::chrono::seconds(61));
                            AttemptSubmit(payload, attemptsLeft - 1, refreshed, onDone);
                            return;
                        }
                        if (response.curlCode != CURLE_OK && attemptsLeft > 0)
                        {
                            PaperLogger.warn("Submit network error (curl {}), retrying ({} attempts left)",
                                            response.curlCode, attemptsLeft);
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            AttemptSubmit(payload, attemptsLeft - 1, refreshed, onDone);
                            return;
                        }
                        PaperLogger.error("Submit failed (http {} curl {})", response.httpCode,
                                        response.curlCode);
                        onDone(false);
                    });
        }

    }

    bool HasSession()
    {
        std::lock_guard lock(sessionMutex);
        return !sessionAccessToken.empty() && !needsRelogin &&
            (sessionExpiresAtUnix == 0 || sessionExpiresAtUnix > NowUnix());
    }

    void EnsureSessionFromMainThread()
    {
        if (cachedGameVersion.empty())
        {
            cachedGameVersion = static_cast<std::string>(UnityEngine::Application::get_version());
        }
        if (HasSession())
            return;
        if (loginInFlight && NowUnix() - loginStartedAt < LOGIN_STALL_SECONDS)
            return;

        ImportSessionFileIfPresent();

        std::string storedRefresh = getModConfig().RefreshToken.GetValue();
        if (!storedRefresh.empty() && !needsRelogin)
        {
            {
                std::lock_guard lock(sessionMutex);
                sessionRefreshToken = storedRefresh;
            }
            loginInFlight = true;
            loginStartedAt = NowUnix();
            RefreshSession([](bool ok)
                           {
                loginInFlight = false;
                if (!ok) PaperLogger.warn("Stored session invalid; download a fresh personalized mod from accsaber.com/quest to relink"); });
            return;
        }

        PaperLogger.warn("Not linked to an account in AccSaber. Download a personalized mod from accsaber.com/quest.");
    }

    void SubmitScore(ScorePayload payload, std::function<void(bool)> onDone)
    {
        auto payloadPtr = std::make_shared<ScorePayload>(std::move(payload));
        AttemptSubmit(std::move(payloadPtr), 3, false, std::move(onDone));
    }

    void ComputeModifierMultiplier(std::vector<std::string> codes, std::function<void(std::optional<double>)> onDone)
    {
        if (codes.empty())
        {
            onDone(1.0);
            return;
        }
        bool loaded;
        {
            std::lock_guard lock(modifierMutex);
            loaded = modifiersLoaded;
        }
        if (loaded)
        {
            onDone(MultiplierFromCache(codes));
            return;
        }
        GetJson("/modifiers", false,
                [codes = std::move(codes), onDone = std::move(onDone)](HttpResponse response)
                {
                    if (!response.Ok())
                    {
                        PaperLogger.error("Failed to load modifiers (http {})", response.httpCode);
                        onDone(std::nullopt);
                        return;
                    }
                    rapidjson::Document doc;
                    if (!ParseJson(response.body, doc) || !doc.IsArray())
                    {
                        onDone(std::nullopt);
                        return;
                    }
                    {
                        std::lock_guard lock(modifierMutex);
                        for (auto const &entry : doc.GetArray())
                        {
                            if (!entry.IsObject())
                                continue;
                            if (!entry.HasMember("code") || !entry["code"].IsString())
                                continue;
                            if (!entry.HasMember("multiplier") || !entry["multiplier"].IsNumber())
                                continue;
                            modifierMultipliers[entry["code"].GetString()] = entry["multiplier"].GetDouble();
                        }
                        modifiersLoaded = true;
                    }
                    onDone(MultiplierFromCache(codes));
                });
    }

    void ResolveMapDifficulty(std::string songHash, std::string difficulty, std::string characteristic,
                                std::function<void(std::optional<std::string>)> onDone)
    {
        GetJson("/maps/hash/" + songHash, false,
                [difficulty = std::move(difficulty), characteristic = std::move(characteristic),
                    onDone = std::move(onDone)](HttpResponse response)
                {
                    if (response.httpCode == 404)
                    {
                        onDone(std::nullopt);
                        return;
                    }
                    if (!response.Ok())
                    {
                        PaperLogger.error("Map lookup failed (http {})", response.httpCode);
                        onDone(std::nullopt);
                        return;
                    }
                    rapidjson::Document doc;
                    if (!ParseJson(response.body, doc) || !doc.IsObject() ||
                        !doc.HasMember("difficulties") || !doc["difficulties"].IsArray())
                    {
                        onDone(std::nullopt);
                        return;
                    }
                    for (auto const &entry : doc["difficulties"].GetArray())
                    {
                        if (!entry.IsObject())
                            continue;
                        if (!entry.HasMember("id") || !entry["id"].IsString())
                            continue;
                        if (!entry.HasMember("difficulty") || !entry["difficulty"].IsString())
                            continue;
                        std::string entryCharacteristic = entry.HasMember("characteristic") &&
                                                                    entry["characteristic"].IsString()
                                                                ? entry["characteristic"].GetString()
                                                                : "Standard";
                        if (difficulty == entry["difficulty"].GetString() &&
                            EqualsIgnoreCase(characteristic, entryCharacteristic))
                        {
                            onDone(std::string(entry["id"].GetString()));
                            return;
                        }
                    }
                    onDone(std::nullopt);
                });
    }

}
