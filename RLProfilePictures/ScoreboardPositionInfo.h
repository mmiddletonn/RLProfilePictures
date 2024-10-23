#pragma once

#include "bakkesmod/wrappers/wrapperstructs.h"

// Define a struct to hold all offset values that will be set externally.
struct SbPosOffsets {
    float scoreboardLeft;
    float blueBottom;
    float orangeTop;
    float bannerDistance;
    float imageWidth;
    float imageHeight;
    float centerX;
    float centerY;
    float scoreboardHeight;
    float scoreboardWidth;
    float imbalanceShift;
    float mutatorSize;
    float skipTickShift;
    float yOffcenterOffset;
};

// Scoreboard Position Info: 
struct SbPosInfo {
    Vector2F blueLeaderPos;  // Position of the profile box for the top blue player
    Vector2F orangeLeaderPos;
    float playerSeparation;  // Distance between profile pics on each team
    float profileScale;      // Amount to scale each platform icon / profile pic
};

inline SbPosInfo getSbPosInfo(Vector2F canvasSize, float uiScale, bool mutators, int numBlues, int numOranges, const SbPosOffsets& offsets) {
    //-----Black Magic, thanks BenTheDan------------
    float scale;
    if (canvasSize.X / canvasSize.Y > 1.5f) {
        scale = 0.507f * canvasSize.Y / offsets.scoreboardHeight;
    }
    else {
        scale = 0.615f * canvasSize.X / offsets.scoreboardWidth;
    }

    Vector2F center = Vector2F{ canvasSize.X / 2, canvasSize.Y / 2 + offsets.yOffcenterOffset * scale * uiScale };
    float mutators_center = canvasSize.X - offsets.mutatorSize * scale * uiScale;
    if (mutators && mutators_center < center.X) {
        center.X = mutators_center;
    }
    int team_difference = numBlues - numOranges;
    center.Y += offsets.imbalanceShift * (team_difference - ((numBlues == 0) != (numOranges == 0)) * (team_difference >= 0 ? 1 : -1)) * scale * uiScale;

    SbPosInfo output;
    output.profileScale = 0.48f;
    float tier_X = -offsets.scoreboardLeft - (offsets.imageWidth - 100.0f) * output.profileScale + 30.5f;
    tier_X = center.X + tier_X * scale * uiScale;
    float tier_Y_blue = -offsets.blueBottom + (6 * (4 - numBlues)) - offsets.bannerDistance * numBlues + 9.0f;
    tier_Y_blue = center.Y + tier_Y_blue * scale * uiScale;
    float tier_Y_orange = offsets.orangeTop;
    tier_Y_orange = center.Y + tier_Y_orange * scale * uiScale;

    // Adjust positions based on some extra tweaks
    output.blueLeaderPos = { tier_X, tier_Y_blue + 10 * scale * uiScale };
    output.orangeLeaderPos = { tier_X, tier_Y_orange + 9 * scale * uiScale };
    output.playerSeparation = offsets.bannerDistance * scale * uiScale;
    output.profileScale *= scale * uiScale;

    //------End Black Magic---------
    return output;
}
