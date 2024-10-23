// RLProfilePictures.cpp
#include "pch.h"

#include "RLProfilePictures.h"
#include "ScoreboardPositionInfo.h"

#include <functional>
#include <map>
#include <iostream>
#include <unordered_set>

#include <fstream>
#include <Windows.h>
#include <wininet.h>

#include <filesystem>
#pragma comment(lib, "wininet.lib")

#include <ctime>
#include <chrono>
#include <nlohmann/json.hpp>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>

#include <unordered_map>
#include <string>


namespace {

    BAKKESMOD_PLUGIN(RLProfilePictures, "RLProfilePictures", plugin_version, PLUGINTYPE_FREEPLAY)

        struct SSParams {
        uintptr_t PRI_A;
        uintptr_t PRI_B;

        // if hooking post
        int32_t ReturnValue;
    };

    std::string nameAndId(PriWrapper pri) {
        return pri.GetPlayerName().ToString() + "|" + pri.GetUniqueIdWrapper().GetIdString();
    }

    std::string nameAndId(const RLProfilePictures::Pri& p) {
        return p.name + "|" + p.uid;
    }
} // namespace

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

void RLProfilePictures::WriteSettings() {
    // Write the settings to the file
    auto file = std::ofstream(settingsFile);
    nlohmann::json json = settings;
    if (file.is_open()) {
        file << json.dump(4);
    }
    file.close();
}

void RLProfilePictures::LoadSettings() {
	// Load the settings from the file
	if (!std::filesystem::exists(settingsFile)) {
		return;
	}
	auto file = std::ifstream(settingsFile);
	if (file.is_open()) {
		nlohmann::json json;
		file >> json;
		settings = json.get<Settings>();
	}
	file.close();
}

void RLProfilePictures::onLoad()
{
    _globalCvarManager = cvarManager;

    LoadSettings();

    // Initialize cache directory and load cache
    cacheDirectory = gameWrapper->GetDataFolder() / "RLProfilePictures" / "cache";
    if (!std::filesystem::exists(cacheDirectory)) {
        std::filesystem::create_directories(cacheDirectory);
    }
    cacheJsonFile = cacheDirectory / "cache.json";
    LoadCache();

    // Hook event to clear cache when the player leaves the game
    gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed", [this](...) {
        ClearCache();
        });

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GFxData_Scoreboard_TA.UpdateSortedPlayerIDs", [this](ActorWrapper caller, ...) {
        getSortedIds(caller);
        ComputeScoreboardInfo();
        });
    gameWrapper->HookEventWithCaller<ActorWrapper>(
        "Function TAGame.GameEvent_Soccar_TA.ScoreboardSort",
        [this](ActorWrapper gameEvent, void* params, std::string eventName) {
            RecordScoreboardComparison(gameEvent, params, eventName);
        });
    gameWrapper->HookEventWithCaller<ActorWrapper>(
        "Function TAGame.PRI_TA.GetScoreboardStats",
        [this](auto args...) { ComputeScoreboardInfo();
        });
    gameWrapper->HookEvent("Function TAGame.GFxData_GameEvent_TA.OnOpenScoreboard", [this](...) {
        scoreBoardOpen = true;
        });
    gameWrapper->HookEvent("Function TAGame.GFxData_GameEvent_TA.OnCloseScoreboard", [this](...) {
        scoreBoardOpen = false;
        });
    gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed", [this](...) {
        scoreBoardOpen = false;

        comparisons.clear();
        disconnectedPris.clear();
        ComputeScoreboardInfo();
        });

    gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) {
        RenderPlatformLogos(canvas);
        });

    // Start the download thread
    downloadThread = std::thread(&RLProfilePictures::DownloadThreadFunction, this);

	offsetsThread = std::thread(&RLProfilePictures::OffsetsThreadFunction, this);
}

void RLProfilePictures::onUnload()
{
    // Signal the download thread to stop
    {
        std::lock_guard<std::mutex> lock(downloadQueueMutex);
        stopDownloadThread = true;
        downloadCV.notify_one();
    }
    if (downloadThread.joinable()) {
        downloadThread.join();
    }
}

void RLProfilePictures::LoadCache() {
    if (!std::filesystem::exists(cacheJsonFile)) {
        return;
    }
    std::ifstream cacheFile(cacheJsonFile);
    if (cacheFile.is_open()) {
        nlohmann::json cacheJson;
        cacheFile >> cacheJson;
        for (auto& [uid, entry] : cacheJson.items()) {
            time_t timestamp = entry["lastCached"];
            cacheTimestamps[uid] = timestamp;
        }
    }
}

void RLProfilePictures::SaveCache() {
    nlohmann::json cacheJson;
    for (auto& [uid, timestamp] : cacheTimestamps) {
        cacheJson[uid]["lastCached"] = timestamp;
    }
    std::ofstream cacheFile(cacheJsonFile);
    if (cacheFile.is_open()) {
        cacheFile << cacheJson.dump(4);
    }
}

void RLProfilePictures::getSortedIds(ActorWrapper caller) {

    auto* scoreboard = reinterpret_cast<ScoreboardObj*>(caller.memory_address);
    if (scoreboard->sorted_names == nullptr) return;
    auto sorted_names = std::wstring(scoreboard->sorted_names);

    std::string str;
    std::transform(sorted_names.begin(), sorted_names.end(), std::back_inserter(str), [](wchar_t c) {
        return (char)c;
        });
    sortedIds = str;
}

bool RLProfilePictures::sortPris(Pri a, Pri b) {
    std::string id_a = a.uid;
    std::string id_b = b.uid;

    if (a.isBot) id_a = "Bot_" + a.name;
    if (b.isBot) id_b = "Bot_" + b.name;

    size_t index_a = sortedIds.find(id_a);
    size_t index_b = sortedIds.find(id_b);
    if (index_a != std::string::npos && index_b != std::string::npos) {
        return index_a < index_b;
    }
    else {
        return a.score > b.score;
    }
}

void RLProfilePictures::ComputeScoreboardInfo() {
    if (!accumulateComparisons) {
        return;
    }
    accumulateComparisons = false;

    auto hash = [](const Pri& p) { return std::hash<std::string>{}(nameAndId(p)); };
    auto keyEqual = [](const Pri& lhs, const Pri& rhs) { return nameAndId(lhs) == nameAndId(rhs); };
    std::unordered_set<Pri, decltype(hash), decltype(keyEqual)> seenPris{ 10, hash, keyEqual };

    disconnectedPris.clear();
    for (const auto& comparison : comparisons) {
        seenPris.insert(comparison.first);
        seenPris.insert(comparison.second);

        if (comparison.first.ghost_player) {
            disconnectedPris.insert(nameAndId(comparison.first));
        }
        if (comparison.second.ghost_player) {
            disconnectedPris.insert(nameAndId(comparison.second));
        }
    }

    std::vector<Pri> seenPrisVector;
    int numBlues{};
    int numOranges{};
    for (auto pri : seenPris) {
        pri.team = teamHistory[nameAndId(pri)];

        if (pri.team > 1) disconnectedPris.insert(nameAndId(pri));
        if (disconnectedPris.find(nameAndId(pri)) != disconnectedPris.end()) {
            pri.ghost_player = true;
        }

        if (pri.team == 0 && !pri.isSpectator) {
            numBlues++;
        }
        else if (pri.team == 1 && !pri.isSpectator) {
            numOranges++;
        }
        seenPrisVector.push_back(pri);
    }
    std::sort(seenPrisVector.begin(), seenPrisVector.end(), [this](const Pri& a, const Pri& b) { return sortPris(a, b); });
    computedInfo = ComputedScoreboardInfo{ seenPrisVector, numBlues, numOranges };
}

void RLProfilePictures::RecordScoreboardComparison(ActorWrapper gameEvent, void* params, std::string eventName) {
    if (!accumulateComparisons) {
        accumulateComparisons = true;
        comparisons.clear();
    }
    SSParams* p = static_cast<SSParams*>(params);

    if (!p) { LOG("NULL SSParams"); return; }
    PriWrapper a(p->PRI_A);
    PriWrapper b(p->PRI_B);

    comparisons.push_back({ a, b });
    auto teamNumA = a.GetTeamNum2();
    if (teamNumA <= 1) {
        teamHistory[nameAndId(a)] = teamNumA;
    }

    auto teamNumB = b.GetTeamNum2();
    if (teamNumB <= 1) {
        teamHistory[nameAndId(b)] = teamNumB;
    }
}

std::shared_ptr<ImageWrapper> RLProfilePictures::GetImageForPlayer(const RLProfilePictures::Pri pri) {
        std::string uid = pri.uid;
    {
        std::lock_guard<std::mutex> lock(imageCacheMutex);
        auto it = imageCache.find(uid);
        if (it != imageCache.end()) {
            return it->second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(downloadQueueMutex);
        if (queuedDownloads.find(pri) == queuedDownloads.end()) {
            downloadQueue.push(pri);
            queuedDownloads.insert(pri);
            downloadCV.notify_one();
        }
    }
    return nullptr;
}

void RLProfilePictures::DownloadThreadFunction() {
    while (true) {
		RLProfilePictures::Pri pri;
        {
            std::unique_lock<std::mutex> lock(downloadQueueMutex);
            downloadCV.wait(lock, [this] {
                return stopDownloadThread || !downloadQueue.empty();
                });
            if (stopDownloadThread && downloadQueue.empty()) {
                break;
            }
            if (!downloadQueue.empty()) {
                pri = downloadQueue.front();
                downloadQueue.pop();
            }
        }
        if (!pri.name.empty()) {
            DownloadAndCacheImage(pri);
            {
                std::lock_guard<std::mutex> lock(downloadQueueMutex);
                queuedDownloads.erase(pri);
            }
        }
    }
}

void RLProfilePictures::OffsetsThreadFunction() {

	//initialize the image url
	std::string url = "https://rlprofilepictures.matt-middleton.com/offsets";

	// Download the json data
	HINTERNET hInternet = InternetOpen(L"RLProfilePictures", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
	if (hInternet == NULL) {
		LOG("[RLProfilePictures] Failed to open internet connection.");
		return;
	}
	HINTERNET hConnect = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD, 0);
	if (hConnect == NULL) {
		LOG("[RLProfilePictures] Failed to open URL: {}", url);
		InternetCloseHandle(hInternet);
		return;
	}

	std::stringstream offsetsStream;
	char buffer[4096];
	DWORD bytesRead;

	while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
		offsetsStream.write(buffer, bytesRead);
	}

	InternetCloseHandle(hConnect);
	InternetCloseHandle(hInternet);

	std::string offsetsData = offsetsStream.str();
	if (!offsetsData.empty()) {

		LOG("[RLProfilePictures] Downloaded scoreboard offsets: {}", offsetsData);

		nlohmann::json json = nlohmann::json::parse(offsetsData);

		offsets.scoreboardLeft = json["scoreboardLeft"];
		offsets.blueBottom = json["blueBottom"];
		offsets.orangeTop = json["orangeTop"];
		offsets.bannerDistance = json["bannerDistance"];
		offsets.imageWidth = json["imageWidth"];
		offsets.imageHeight = json["imageHeight"];
		offsets.centerX = json["centerX"];
		offsets.centerY = json["centerY"];
		offsets.scoreboardHeight = json["scoreboardHeight"];
		offsets.scoreboardWidth = json["scoreboardWidth"];
		offsets.imbalanceShift = json["imbalanceShift"];
		offsets.mutatorSize = json["mutatorSize"];
		offsets.skipTickShift = json["skipTickShift"];
		offsets.yOffcenterOffset = json["yOffcenterOffset"];

	}

}

void RLProfilePictures::DownloadAndCacheImage(RLProfilePictures::Pri pri) {

	LOG("[RLProfilePictures] Downloading image for {} | {} | {}", pri.name, pri.platform, pri.id);

    // Sanitize UID for file name
    std::string sanitizedUid = pri.uid;
    std::replace(sanitizedUid.begin(), sanitizedUid.end(), '|', '_'); // Replace '|' with '_'

    // Construct the path to the image file
    std::filesystem::path imagePath = cacheDirectory / (sanitizedUid + ".png");

    // Get current time
    time_t currentTime = std::time(nullptr);

	//initialize the image url
	std::string imageUrl = getImageUrl(pri.platform, pri.id, settings, gameWrapper.get());

    // Download the image data
    HINTERNET hInternet = InternetOpen(L"RLProfilePictures", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hInternet == NULL) {
        LOG("[RLProfilePictures] Failed to open internet connection.");
        return;
    }
    HINTERNET hConnect = InternetOpenUrlA(hInternet, imageUrl.c_str(), NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (hConnect == NULL) {
        LOG("[RLProfilePictures] Failed to open URL: {}", imageUrl);
        InternetCloseHandle(hInternet);
        return;
    }

    std::stringstream imageStream;
    char buffer[4096];
    DWORD bytesRead;

    while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        imageStream.write(buffer, bytesRead);
    }

    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    std::string imageData = imageStream.str();
    if (!imageData.empty()) {
        // Save the image data to the image file
        std::ofstream outFile(imagePath.string(), std::ios::binary);
        outFile.write(imageData.c_str(), imageData.size());
        outFile.close();

        // Update cache timestamp
        {
            std::lock_guard<std::mutex> lock(imageCacheMutex);
            cacheTimestamps[pri.uid] = currentTime;
            SaveCache();
        }

        // Now schedule the creation of ImageWrapper on the main thread
        gameWrapper->Execute([this, pri, imagePath](GameWrapper* gw) {
            auto image = std::make_shared<ImageWrapper>(imagePath.string(), false, false);
            if (!image->IsLoadedForCanvas()) {
                image->LoadForCanvas();
            }
            {
                std::lock_guard<std::mutex> lock(imageCacheMutex);
                imageCache[pri.uid] = image;
            }
        });
    }
}

void RLProfilePictures::UpdatePlayerProfilePicture(std::shared_ptr<GameWrapper> gameWrapper, std::string imageFilePath)
{
    if (gameWrapper == nullptr)
    {
        LOG("GameWrapper is null");
        return;
    }

    gameWrapper->Execute([gameWrapper, imageFilePath](GameWrapper* gw)
        {
            std::string playerUID = gw->GetUniqueID().GetIdString();

            size_t firstPipe = playerUID.find('|');
            size_t secondPipe = playerUID.find('|', firstPipe + 1);
            std::string platform = playerUID.substr(0, firstPipe);
            std::string id = playerUID.substr(firstPipe + 1, secondPipe - firstPipe - 1);

            LOG("Current player ID: {}", id);
            LOG("imageFilePath: {}", imageFilePath);

            // Prepare the post request
            HINTERNET hSession = InternetOpen(L"RLProfilePictures/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
            if (!hSession)
            {
                LOG("InternetOpen failed");
                return;
            }

            HINTERNET hConnect = InternetConnect(hSession, L"rlprofilepictures.matt-middleton.com", INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
            if (!hConnect)
            {
                LOG("InternetConnect failed");
                InternetCloseHandle(hSession);
                return;
            }

            HINTERNET hRequest = HttpOpenRequest(hConnect, L"POST", L"/upload", NULL, NULL, NULL, INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
            if (!hRequest)
            {
                LOG("HttpOpenRequest failed");
                InternetCloseHandle(hConnect);
                InternetCloseHandle(hSession);
                return;
            }

            // Prepare the file data to send
            std::ifstream file(imageFilePath, std::ios::binary);
            if (!file)
            {
                LOG("Failed to open image file");
                InternetCloseHandle(hRequest);
                InternetCloseHandle(hConnect);
                InternetCloseHandle(hSession);
                return;
            }
            std::string fileData((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            // Prepare the form data
            std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
            std::string contentType = "multipart/form-data; boundary=" + boundary;
            std::string body = "--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"pfp\"; filename=\"" + imageFilePath + "\"\r\n" +
                "Content-Type: image/jpeg\r\n\r\n" +
                fileData + "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"id\"\r\n\r\n" +
                id + "\r\n" +
                "--" + boundary + "--\r\n";

            // Send the request
            BOOL result = HttpSendRequest(hRequest,
                L"Content-Type: multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxkTrZu0gW\r\n",
                -1L,
                (LPVOID)body.c_str(),
                body.size());

            if (!result)
            {
                LOG("HttpSendRequest failed");
                return;
            }
            else
            {
                LOG("Profile picture uploaded successfully");
            }

            // Clean up
            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hSession);

            return;
        });
}

void RLProfilePictures::RenderPlatformLogos(CanvasWrapper canvas) {

    if (!scoreBoardOpen) { return; }
    if (!gameWrapper->IsInOnlineGame()) { return; }
    ServerWrapper sw = gameWrapper->GetOnlineGame();
    if (!sw) { return; }
    if (sw.GetbMatchEnded()) { return; }

    LinearColor blueColor = teamColors[0];
    LinearColor orangeColor = teamColors[1];

    MMRWrapper mmrWrapper = gameWrapper->GetMMRWrapper();
    Vector2 screenSize = gameWrapper->GetScreenSize();
    Vector2F screenSizeFloat{ screenSize.X, screenSize.Y };
    SbPosInfo sbPosInfo = getSbPosInfo(screenSizeFloat,
        gameWrapper->GetDisplayScale() * gameWrapper->GetInterfaceScale(),
        /* mutators= */ mmrWrapper.GetCurrentPlaylist() == 34,
        computedInfo.bluePlayerCount,
        computedInfo.orangePlayerCount, offsets);

    int blues = -1;
    int oranges = -1;

    Vector2F imageShift = { 0, 0 };

    for (auto pri : computedInfo.sortedPlayers) {

        if (pri.isSpectator) continue;

        Vector2F drawPos{};

        if (pri.team == 0) {
            blues++;
            canvas.SetColor(blueColor);
            if (pri.ghost_player) canvas.SetColor(LinearColor{ blueColor.R, blueColor.G, blueColor.B, 155 / 1.5 });
            drawPos = sbPosInfo.blueLeaderPos + Vector2F{ 0, sbPosInfo.playerSeparation * blues } + imageShift;
        }
        else if (pri.team == 1) {
            oranges++;

            canvas.SetColor(orangeColor);
            if (pri.ghost_player) canvas.SetColor(LinearColor{ orangeColor.R, orangeColor.G, orangeColor.B, 155 / 1.5 });
            drawPos = sbPosInfo.orangeLeaderPos + Vector2F{ 0, sbPosInfo.playerSeparation * oranges } + imageShift;
        }
        else {
            LOG("[RLProfilePictures] Unexpected team value {} for pri {}", pri.team, nameAndId(pri));
            continue;
        }
        if (pri.isBot) { continue; }

        canvas.SetPosition(drawPos);

        // Use the UID to get the appropriate image
        std::shared_ptr<ImageWrapper> image = GetImageForPlayer(pri);
        if (image && image->IsLoadedForCanvas()) {
            canvas.DrawTexture(image.get(), 100.0f / 48.0f * sbPosInfo.profileScale); // Scale images accordingly
        }
        // Else, do not draw anything
    }
}

void RLProfilePictures::ClearCache() {
    try {
        // Step 1: Ensure the cache directory is valid
        // Get the paths to the relevant directories
        std::filesystem::path cachePath = cacheDirectory;
        std::filesystem::path rlProfilePicturesDir = cachePath.parent_path();   // Parent directory of the cache
        std::filesystem::path dataDir = rlProfilePicturesDir.parent_path();     // Parent directory of 'RLProfilePictures'
        std::filesystem::path bakkesmodDir = dataDir.parent_path();             // Parent directory of 'data'

        // Validate that the cache directory is within the expected directory structure
        if (rlProfilePicturesDir.filename() != "RLProfilePictures") {
            LOG("[RLProfilePictures] Cache directory is not within 'RLProfilePictures'. Aborting cache clear.");
            return; // Abort if not under 'RLProfilePictures'
        }
        if (dataDir.filename() != "data") {
            LOG("[RLProfilePictures] Cache directory is not within 'data'. Aborting cache clear.");
            return; // Abort if not under 'data'
        }
        if (bakkesmodDir.filename() != "bakkesmod") {
            LOG("[RLProfilePictures] Cache directory is not within 'bakkesmod'. Aborting cache clear.");
            return; // Abort if not under 'bakkesmod'
        }

        // Step 2: Remove all files in the cache directory
        std::filesystem::remove_all(cacheDirectory);

        // Step 3: Recreate the cache directory
        std::filesystem::create_directories(cacheDirectory);

        // Step 4: Clear in-memory caches
        {
            std::lock_guard<std::mutex> lock(imageCacheMutex);
            imageCache.clear();
            {
                std::lock_guard<std::mutex> lock(downloadQueueMutex);
                queuedDownloads.clear();
                std::queue<RLProfilePictures::Pri> emptyQueue;
                std::swap(downloadQueue, emptyQueue);
            }
            cacheTimestamps.clear();
        }

        // Step 5: Save empty cache
        SaveCache();

        // Log the successful clearing of the cache
        LOG("[RLProfilePictures] Cache cleared");
    }
    catch (const std::exception& e) {
        // Log any exceptions that occur during the process
        LOG("[RLProfilePictures] Exception in ClearCache: {}", e.what());
    }
}
