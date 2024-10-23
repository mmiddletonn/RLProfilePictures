// RLProfilePictures.h
#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include <fstream>

#include "version.h"
#include <set>
#include <map>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <ctime>
#include <chrono>
#include <nlohmann/json.hpp>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>

#include "Settings.h"
#include "ScoreboardPositionInfo.h"

constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

class RLProfilePictures : public BakkesMod::Plugin::BakkesModPlugin,
    public BakkesMod::Plugin::PluginSettingsWindow,
    public BakkesMod::Plugin::PluginWindow {
public:
    struct Pri {
		std::string uid;
        std::string id;
        int score{};
        unsigned char team{};
        bool isBot{};
        std::string name;
        std::string platform;
        bool ghost_player;
        bool isSpectator;

        // Default constructor
        Pri() {}

        // Constructor using PriWrapper
        Pri(PriWrapper p) {
            if (!p) { return; }
            uid = p.GetUniqueIdWrapper().GetIdString();
            score = p.GetMatchScore();
            team = p.GetTeamNum2();
            isBot = p.GetbBot();
            name = p.GetPlayerName().ToString();
            platform = p.GetPlatform();
            isSpectator = p.IsSpectator();
            ghost_player = team > 1;

            size_t firstPipe = uid.find('|');
            size_t secondPipe = uid.find('|', firstPipe + 1);
            platform = uid.substr(0, firstPipe);

            // Extract the substring between the first and second '|' characters to get the id
            id = uid.substr(firstPipe + 1, secondPipe - firstPipe - 1);

        }

        // Overload the < operator
        bool operator<(const Pri& other) const {
            return name < other.name;
        }
    };

    struct SbPosOffsets offsets;

    struct ScoreboardObj
    {
        unsigned char pad[0xB0];
        wchar_t* sorted_names;
    };

private:
    /**
     * Stores data derived from each scoreboard sort cycle (happens once every second).
     */
    struct ComputedScoreboardInfo {
        std::vector<Pri> sortedPlayers;
        int bluePlayerCount{};
        int orangePlayerCount{};
    };

    Settings settings;

    virtual void onLoad();
    virtual void onUnload();
    /**
     * Pre-compute scoreboard info after the sorting algorithm finishes. The hook
     * "TAGame.PRI_TA.GetScoreboardStats" runs at least once immediately after Rocket League sorts
     * the scoreboard. This happens every second, so computing the info like this saves us some
     * performance.
     */
    void ComputeScoreboardInfo();
    void RecordScoreboardComparison(ActorWrapper gameEvent, void* params, std::string eventName);
    void RenderPlatformLogos(CanvasWrapper canvas);
    void RenderDebugInfo(CanvasWrapper canvas);
    void RenderDebugPri(CanvasWrapper canvas, const RLProfilePictures::Pri& pri, Vector2F iconPos);
    std::set<std::string> disconnectedPris;
    void RenderSettings() override;
    std::string GetPluginName() override;
    virtual void Render() override;
    virtual std::string GetMenuName() override;
    virtual std::string GetMenuTitle() override;
    virtual void SetImGuiContext(uintptr_t ctx) override;
    virtual bool ShouldBlockInput() override;
    virtual bool IsActiveOverlay() override;
    virtual void OnOpen() override;
    virtual void OnClose() override;
    void SaveSettings();
    void getSortedIds(ActorWrapper caller);
    bool sortPris(Pri a, Pri b);
    bool hotfix = false;

    // Members for scoreboard tracking logic.
    std::string sortedIds = "";
    std::vector<std::pair<Pri, Pri>> comparisons;

    void WriteSettings();
	void LoadSettings();
    bool getPlatformVisibility(const std::string& platform, const Settings& settings, GameWrapper* gameWrapper) {
        if (platform == "Epic") return settings.showEpic;
        if (platform == "Steam") return settings.showSteam && !gameWrapper->IsUsingSteamVersion();  // Hide if Steam version is being used
        if (platform == "XboxOne") return settings.showXbox;
        if (platform == "PS4") return settings.showPSN;
        if (platform == "Switch") return settings.showSwitch;
        return false;
    }

    std::string getImageUrl(const std::string& platform, const std::string& id, const Settings& settings, GameWrapper* gameWrapper) {
        // Base URL for the platform
        std::string baseUrl = "https://rlprofilepictures.matt-middleton.com/platform/" + platform + "/id/" + id + "/info/";

        // Check if the user wants to show the platform, and return the correct URL
        return baseUrl + (getPlatformVisibility(platform, settings, gameWrapper) ? "show" : "hide");
    }

    void UpdatePlayerProfilePicture(std::shared_ptr<GameWrapper>, std::string);

    std::filesystem::path settingsFile = gameWrapper->GetDataFolder() / "RLProfilePictures" / "settings.json";
    /**
     * teamHistory records the last team that a player (represented by the
     * string combining their name and uid) was seen on. This is necessary,
     * because during ScoreboardSort any disconnected players will have a team
     * number different from Blue or Orange, but we still need to know which team
     * they show up on in the scoreboard display.
     */
    std::unordered_map<std::string, int> teamHistory;
    ComputedScoreboardInfo computedInfo{};  // Derived from comparisons and teamHistory.
    bool accumulateComparisons{};

    // Members for scoreboard rendering.
    bool scoreBoardOpen{};
    LinearColor teamColors[2]{ {255, 255, 255, 255}, {255, 255, 255, 255} };
    const static int LOGO_COUNT = 6;

    std::shared_ptr<ImageWrapper> UserIcon;

    // Cache for storing images based on UID
    std::unordered_map<std::string, std::shared_ptr<ImageWrapper>> imageCache;
    std::mutex imageCacheMutex; // For thread-safe access to imageCache

    // Cache management
    std::filesystem::path cacheDirectory;
    std::filesystem::path cacheJsonFile;
    std::unordered_map<std::string, time_t> cacheTimestamps;
    void LoadCache();
    void SaveCache();
    const int CACHE_EXPIRATION_SECONDS = 7 * 24 * 60 * 60; // One week

    // Function to get the image for a specific player based on their UID
    std::shared_ptr<ImageWrapper> GetImageForPlayer(const RLProfilePictures::Pri);

    // For managing background image downloads
    std::queue<RLProfilePictures::Pri> downloadQueue;
    std::set<RLProfilePictures::Pri> queuedDownloads;
    std::mutex downloadQueueMutex;
    std::condition_variable downloadCV;
    bool stopDownloadThread = false;
    std::thread downloadThread;
	std::thread offsetsThread;
    void DownloadThreadFunction();
	void OffsetsThreadFunction();

    // Function to download and cache the image
    void DownloadAndCacheImage(const RLProfilePictures::Pri pri);

    // Function to clear cache when player leaves the game
    void ClearCache();

    // ImGui settings
    ImGuiStyle UserStyle;

    // Members for menus.
    bool windowOpen{};
    std::string menuTitle{ "RLProfilePictures" };

};
