#include "pch.h"
#include "RLProfilePictures.h"
#include "IMGUI/imgui_internal.h"
#include <windows.h> // For Windows API
#include <commdlg.h> // For the file dialog
#include <string>    // For std::string
#include <vector>    // For std::vector
#include <algorithm> // For std::search

std::string RLProfilePictures::GetPluginName() {
    return "RLProfilePictures";
}

void RLProfilePictures::SetImGuiContext(uintptr_t ctx) {
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

// Helper function to find a window with a title containing "Rocket League"
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    char windowTitle[256];
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));

    std::string titleStr(windowTitle);

    // Check if the window title contains "Rocket League"
    if (titleStr.find("Rocket League") != std::string::npos) {
        // If a match is found, store the window handle in the vector
        std::vector<HWND>* windows = reinterpret_cast<std::vector<HWND>*>(lParam);
        windows->push_back(hwnd);
    }

    return TRUE; // Continue enumeration
}

HWND FindRocketLeagueWindow() {
    std::vector<HWND> matchingWindows;

    // Enumerate through all top-level windows
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&matchingWindows));

    // If we found any matching windows, return the first one
    if (!matchingWindows.empty()) {
        return matchingWindows[0];
    }

    // If no windows matched, return NULL
    return NULL;
}

// Helper function to open Windows File Dialog (narrow character version)
std::string OpenFileDialog() {
    char fileName[MAX_PATH] = ""; // Buffer to store the file name (narrow char)

    // Find the game window with title containing "Rocket League"
    HWND gameWindow = FindRocketLeagueWindow();

    // Minimize the game window before opening the file dialog
    if (gameWindow) {
        ShowWindow(gameWindow, SW_MINIMIZE);
    }

    // Initialize OPENFILENAME structure (narrow version)
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL; // No owner window
    ofn.lpstrFilter = "Image Files\0*.jpg;*.jpeg;*.png\0All Files\0*.*\0";
    ofn.lpstrFile = fileName; // The path the user selects (narrow string)
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    // Display the Open File Dialog (narrow version)
    if (GetOpenFileNameA(&ofn)) {
        return std::string(fileName); // Return the selected file path
    }

    return ""; // If user cancels or error occurs
}

void RLProfilePictures::RenderSettings() {
    ImGui::Text("");
    ImGui::Text("This plugin allows you to see all players' profile pictures on the scoreboard in-game.");
    ImGui::Text("Special thanks to SoulDaMeep and BenTheDan");
    ImGui::Text("This would not have been possible without their work on PlatformDisplay and InGameRank.");
    ImGui::Text("");

    ImGui::Separator();

	ImGui::Text("Set your own profile picture:");

    static std::string filePath = "";

	bool isDisabled = !gameWrapper->IsUsingEpicVersion(); // Disable the button if not using Epic Games version

    // Assuming 'isDisabled' is your boolean
    if (isDisabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true); // Disable the button
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f); // Make the button appear visually disabled
    }

    if (ImGui::Button("Browse...")) {
        std::string selectedFile = OpenFileDialog();
        if (!selectedFile.empty()) {
            filePath = selectedFile; // Update the file path if a file was selected

            // Automatically upload the file after selection
            UpdatePlayerProfilePicture(gameWrapper, filePath);
        }
    }

    if (isDisabled) {
        ImGui::PopStyleVar(); // Revert the button visual style
        ImGui::PopItemFlag(); // Re-enable the button
    }

    ImGui::Text("");

    ImGui::Separator();
    ImGui::Text("Render profile pictures for the following platforms:");

    // Checkboxes for platforms
    if (ImGui::Checkbox("Epic Games", &settings.showEpic)) {
        WriteSettings();
    }
    if (isDisabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true); // Disable the checkbox
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f); // Make the checkbox appear visually disabled
    }

    if (ImGui::Checkbox("Steam", &settings.showSteam)) {
        WriteSettings();
    }

    if (isDisabled) {
        ImGui::PopStyleVar(); // Revert the checkbox visual style
        ImGui::PopItemFlag(); // Re-enable the checkbox
    }
    if (ImGui::Checkbox("PSN", &settings.showPSN)) {
        WriteSettings();
    }
    if (ImGui::Checkbox("Xbox", &settings.showXbox)) {
        WriteSettings();
    }
    if (ImGui::Checkbox("Switch", &settings.showSwitch)) {
        WriteSettings();
    }

    ImGui::Text("");
    ImGui::Separator();

    ImGui::Text("Credits:");
	ImGui::BulletText("hamter_rl | Development");
    ImGui::BulletText("SoulDaMeep | Overlay logic");
    ImGui::BulletText("BenTheDan | Scoreboard math");
}

void RLProfilePictures::Render() {
    if (!ImGui::Begin(menuTitle.c_str(), &windowOpen, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    RenderSettings();
    ImGui::End();

    if (!windowOpen) {
        cvarManager->executeCommand("togglemenu " + GetMenuName());
    }
}

std::string RLProfilePictures::GetMenuName() {
    return "RLProfilePictures";
}

std::string RLProfilePictures::GetMenuTitle() {
    return menuTitle;
}

bool RLProfilePictures::ShouldBlockInput() {
    return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

bool RLProfilePictures::IsActiveOverlay() {
    return true;
}

void RLProfilePictures::OnOpen() {
    windowOpen = true;
}

void RLProfilePictures::OnClose() {
    windowOpen = false;
}
