// swrObjJdge_SetupTrackEnvironment with the fly-by state cleared on both early returns. Retail
// returns without touching cam_spline / camSweepCursor / camSweepTransform when the track has no
// camera path (or no start node), so they keep pointing into the previous track's asset-buffer
// data. Stock tracks never take those returns; custom tracks do (PR #307). Everything else is
// the src/ body unchanged, including the fog setup the early returns skip.
#include "track_lifecycle.h"

#include <Primitives/rdMatrix.h>
#include <Swr/swrAssetBuffer.h>
#include <Swr/swrCam.h>
#include <Swr/swrModel.h>
#include <Swr/swrObj.h>
#include <Swr/swrRender.h>
#include <Swr/swrSpline.h>
#include <globals.h>

#include <string.h>

static void flyby_clear(swrObjJdge* judge)
{
    judge->cam_spline = NULL;
    judge->camSweepAnim = NULL;
    memset(&judge->camSweepCursor, 0, sizeof(judge->camSweepCursor));
    rdMatrix_SetIdentity44(&judge->camSweepTransform);
}

// 0x00464b90
void refined_swrObjJdge_SetupTrackEnvironment(swrObjJdge* judge, int* anims, int model)
{
    static const SPLINEID camSplines[8][4] = {
        { SPLINEID_tatooine_mini_track6came, SPLINEID_tatooine_track7camm, -1, -1 }, { SPLINEID_planeta_track6came, SPLINEID_planeta_track7camm, SPLINEID_planeta_track8camh, SPLINEID_planeth_track6came }, { SPLINEID_planetb1_track6came, SPLINEID_planetb2_track7camm, SPLINEID_planetb3_track8camh, -1 }, { SPLINEID_planetc_track6came, SPLINEID_planetc_track7camm, SPLINEID_planetc_track8camh, -1 }, { SPLINEID_planetd1_track6came, SPLINEID_planetd2_track7camm, SPLINEID_planetd3_track8camh, SPLINEID_planeti_track6came }, { SPLINEID_planete1_track6came, SPLINEID_planete2_track7camm, SPLINEID_planete3_track8camh, -1 }, { SPLINEID_planetf_track6came, SPLINEID_planetf_track7camm, SPLINEID_planetf_track8camh, -1 }, { SPLINEID_planetj1_track6came, SPLINEID_planetj2_track7camm, SPLINEID_planetj3_track8camh, -1 },
    };
    swrModel_Header* header = (swrModel_Header*)model;
    swrModel_Node* startNode = NULL;
    swrModel_Animation* camAnim;
    char* bufferBefore;
    int fogStartDist;
    int fogR = 0;
    int fogG = 0;
    int fogB = 0;

    // the judge lives for the whole process (allocated at world init, below checkpoint 1)
    track_register_cursor(&judge->camSweepCursor);
    judge->cam_splineId = -1;
    if (judge->planetId >= 0 && judge->planetId < 8 && judge->planet_track_number >= 0 && judge->planet_track_number < 4)
        judge->cam_splineId = camSplines[judge->planetId][judge->planet_track_number];
    judge->camSweepState = NULL;
    if ((int)judge->cam_splineId < 0) {
        flyby_clear(judge);
        return;
    }

    bufferBefore = swrAssetBuffer_GetBuffer();
    judge->cam_spline = swrSpline_LoadSplineById((char*)judge->cam_splineId);
    swrObjJdge_splineBufferBytes += swrAssetBuffer_GetBuffer() - bufferBefore;

    if (judge->planet_track_number == 0)
        startNode = header->entries[3].node;
    if (judge->planet_track_number == 1)
        startNode = header->entries[4].node;
    if (judge->planet_track_number == 2)
        startNode = header->entries[5].node;
    if (judge->planet_track_number == 3)
        startNode = header->entries[3].node;
    if (judge->cam_spline == NULL || startNode == NULL) {
        flyby_clear(judge);
        return;
    }

    judge->camSweepState = NULL;
    judge->camSweepCursor.spline = judge->cam_spline;
    judge->camSweepCursor.velocity = 0.0f;
    judge->camSweepCursor.segmentT = 0.0f;
    judge->camSweepCursor.tangentLength = 0.0f;
    judge->camSweepCursor.endFlag = 0;
    judge->camSweepCursor.startFlag = 0;
    judge->camSweepCursor.branchSelector = 0;
    judge->camSweepCursor.branchFlags = 0;
    if (judge->camSweepCursor.spline != NULL)
        swrSpline_CursorSeek(&judge->camSweepCursor, 0);
    swrSpline_EvaluateAtOffset(&judge->camSweepCursor, &judge->camSweepTransform, 0.0f);

    camAnim = NULL;
    if (anims != NULL) {
        for (; *anims != 0; anims++) {
            if (((swrModel_Animation*)*anims)->node_ptr == (swrModel_NodeTransformed*)startNode) {
                swrModel_AnimationClearFlags((swrModel_Animation*)*anims, 0x30);
                camAnim = (swrModel_Animation*)*anims;
            }
        }
    }
    // the sweep walks the whole camera spline over the start node's animation
    if (camAnim == NULL)
        judge->camSweepCursor.velocity = 1.0f;
    else
        judge->camSweepCursor.velocity = ((float)(int)((swrSpline*)judge->cam_spline)->num_control_points - 1.0f) / camAnim->duration4;
    swrCam_CamState_InitMainMat4(5, 2, &judge->camSweepTransform, 0);
    swrCam_CamState_SetOffsetSource(5, startNode, 2);
    judge->camSweepAnim = camAnim;

    nearClipDistance = 5.0f;
    drawDistance = 5000.0f;
    fogStartDist = 0;
    if (judge->planetId == 0) {
        drawDistance = 4500.0f;
        fogStartDist = 996;
        fogR = 0xdc;
        fogG = 0xea;
        fogB = 0xe5;
    }
    if (judge->planetId == 1) {
        drawDistance = 11000.0f;
        fogStartDist = 996;
        if (judge->planet_track_number == 3) {
            fogR = 0xad;
            fogG = 0xb9;
            fogB = 0xc6;
        } else {
            fogR = 0xd2;
            fogG = 0xd7;
            fogB = 0xe1;
        }
    }
    if (judge->planetId == 2) {
        if (judge->planet_track_number == 0) {
            drawDistance = 3500.0f;
        } else if (judge->planet_track_number == 1) {
            drawDistance = 3500.0f;
            fogStartDist = 996;
            fogR = 0x78;
            fogG = 0x64;
            fogB = 0x78;
        } else if (judge->planet_track_number == 2) {
            drawDistance = 11000.0f;
        }
    }
    if (judge->planetId == 3) {
        fogStartDist = 994;
        fogR = 0x9a;
        fogG = 0x81;
        fogB = 0x81;
        drawDistance = 11000.0f;
    }
    if (judge->planetId == 4) {
        if (judge->planet_track_number == 3) {
            drawDistance = 5000.0f;
        } else if (judge->planet_track_number == 0) {
            drawDistance = 5000.0f;
            fogStartDist = 996;
            fogR = 0xca;
            fogG = 0xb7;
            fogB = 0xc3;
        } else if (judge->planet_track_number == 1 || judge->planet_track_number == 2) {
            if (judge->planet_track_number == 1) {
                drawDistance = 1200.0f;
                fogStartDist = 992;
                fogR = 0x9b;
                fogG = 0x9a;
                fogB = 0xa7;
            } else {
                drawDistance = 1500.0f;
                fogStartDist = 990;
                fogR = 0x5a;
                fogG = 0x36;
                fogB = 0x28;
            }
            if (someRootNodeChildNodes[0] != NULL)
                swrModel_NodeModifyFlags(someRootNodeChildNodes[0], 2, -4, 0x10, 3);
        }
    }
    if (judge->planetId == 5) {
        fogR = 0xbe;
        fogG = 0x37;
        fogB = 0x0a;
        if (judge->planet_track_number == 0) {
            drawDistance = 8000.0f;
            fogStartDist = 990;
        } else if (judge->planet_track_number == 1) {
            drawDistance = 2500.0f;
            fogStartDist = 996;
        } else if (judge->planet_track_number == 2) {
            drawDistance = 4000.0f;
            fogStartDist = 996;
        }
    }
    if (judge->planetId == 6)
        drawDistance = 11000.0f;
    if (judge->planetId == 7) {
        if (judge->planet_track_number == 1)
            drawDistance = 2500.0f;
        else if (judge->planet_track_number == 0 || judge->planet_track_number == 2)
            drawDistance = 6000.0f;
    }

    if (fogStartDist == 0) {
        rdModel_SetFogEnabled(0);
    } else {
        rdModel_SetFogEnabled(1);
        swrRender_SetFogParameters(fogStartDist, 1000, fogR, fogG, fogB, 0xff);
        swrRender_SetClearColor(fogR, fogG, fogB);
    }
    swrRender_frameClearFlags |= swrRender_FRAME_CLEAR_COLOR;
}
