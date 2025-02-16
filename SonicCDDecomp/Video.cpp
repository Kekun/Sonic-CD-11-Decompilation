﻿#include "RetroEngine.hpp"

int currentVideoFrame = 0;
int videoFrameCount = 0;
int videoWidth  = 0;
int videoHeight       = 0;

THEORAPLAY_Decoder *videoDecoder;
const THEORAPLAY_VideoFrame *videoVidData;
THEORAPLAY_Io callbacks;

byte videoData = 0;
int videoFilePos = 0;
bool videoPlaying = 0;
int vidFrameMS = 0;
int vidBaseticks = 0;


bool videoSkipped = false;

static long videoRead(THEORAPLAY_Io *io, void *buf, long buflen)
{
    FileIO *file    = (FileIO *)io->userdata;
    const size_t br = fRead(buf, 1, buflen * sizeof(byte), file);
    if (br == 0)
        return -1;
    return (int)br;
} // IoFopenRead

static void videoClose(THEORAPLAY_Io *io)
{
    FileIO *file = (FileIO *)io->userdata;
    fClose(file);
}

void PlayVideoFile(char *filePath) { 
    char filepath[0x100];
    StrCopy(filepath, BASE_PATH"videos/");
    StrAdd(filepath, filePath);
    StrAdd(filepath, ".ogv");

    FileIO *file = fOpen(filepath, "rb");
    if (file) {
        printLog("Loaded File '%s'!", filepath);

        callbacks.read     = videoRead;
        callbacks.close    = videoClose;
        callbacks.userdata = (void *)file;
        videoDecoder       = THEORAPLAY_startDecode(&callbacks, /*FPS*/ 30, THEORAPLAY_VIDFMT_RGBA, GetGlobalVariableByName("Options.Soundtrack") ? 1 : 0);

        if (!videoDecoder) {
            printLog("Video Decoder Error!");
            return;
        }
        while (!videoVidData) {
            if (!videoVidData)
                videoVidData = THEORAPLAY_getVideo(videoDecoder);
        }
        if (!videoVidData) {
            printLog("Video Error!");
            return;
        }

        videoWidth  = videoVidData->width;
        videoHeight = videoVidData->height;
        SetupVideoBuffer(videoWidth, videoHeight);
        vidBaseticks = SDL_GetTicks();
        vidFrameMS     = (videoVidData->fps == 0.0) ? 0 : ((Uint32)(1000.0 / videoVidData->fps));
        videoPlaying = true;
        trackID        = TRACK_COUNT - 1;

        videoSkipped = false;

        Engine.gameMode = ENGINE_VIDEOWAIT;
    }
    else {
        printLog("Couldn't find file '%s'!", filepath);
    }
    
}

void UpdateVideoFrame()
{
    if (videoPlaying) {
        if (videoFrameCount > currentVideoFrame) {
            GFXSurface *surface = &gfxSurface[videoData];
            int fileBuffer               = 0;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer << 24;

            byte clr[3];
            for (int i = 0; i < 0x80; ++i) {
                FileRead(&clr, 3);
                activePalette32[i].r = clr[0];
                activePalette32[i].g = clr[1];
                activePalette32[i].b = clr[2];
                activePalette[i]     = ((ushort)(clr[0] >> 3) << 11) | 32 * (clr[1] >> 2) | (clr[2] >> 3);
            }

            FileRead(&fileBuffer, 1);
            while (fileBuffer != ',') FileRead(&fileBuffer, 1); // gif image start identifier

            FileRead(&fileBuffer, 2); // IMAGE LEFT
            FileRead(&fileBuffer, 2); // IMAGE TOP
            FileRead(&fileBuffer, 2); // IMAGE WIDTH
            FileRead(&fileBuffer, 2); // IMAGE HEIGHT
            FileRead(&fileBuffer, 1); // PaletteType
            bool interlaced = (fileBuffer & 0x40) >> 6;
            if (fileBuffer >> 7 == 1) {
                int c = 0x80;
                do {
                    ++c;
                    FileRead(&fileBuffer, 3);
                } while (c != 0x100);
            }
            ReadGifPictureData(surface->width, surface->height, interlaced,
                                       graphicData, surface->dataPosition);

            SetFilePosition(videoFilePos);
            ++currentVideoFrame;
        }
        else {
            videoPlaying = 0;
            CloseFile();
        }
    }
}

int ProcessVideo()
{
    if (videoPlaying) {
        CheckKeyPress(&keyPress, 0x10);

        if (videoSkipped && fadeMode < 0xFF) {
            fadeMode += 8;
        }

        if (keyPress.A) {
            if (!videoSkipped) 
                fadeMode = 0;

            videoSkipped = true;
        }

        if (!THEORAPLAY_isDecoding(videoDecoder) || (videoSkipped && fadeMode >= 0xFF)) {
            StopVideoPlayback();

            return 1; // video finished
        }

        // Don't pause or it'll go wild
        if (videoPlaying) {
            const Uint32 now = (SDL_GetTicks() - vidBaseticks);

            if (!videoVidData)
                videoVidData = THEORAPLAY_getVideo(videoDecoder);

            // Play video frames when it's time.
            if (videoVidData && (videoVidData->playms <= now)) {
                if (vidFrameMS && ((now - videoVidData->playms) >= vidFrameMS)) {

                    // Skip frames to catch up, but keep track of the last one+
                    //  in case we catch up to a series of dupe frames, which
                    //  means we'd have to draw that final frame and then wait for
                    //  more.

                    const THEORAPLAY_VideoFrame *last = videoVidData;
                    while ((videoVidData = THEORAPLAY_getVideo(videoDecoder)) != NULL) {
                        THEORAPLAY_freeVideo(last);
                        last = videoVidData;
                        if ((now - videoVidData->playms) < vidFrameMS)
                            break;
                    }

                    if (!videoVidData)
                        videoVidData = last;
                }

                // do nothing; we're far behind and out of options.
                if (!videoVidData) {
                    // video lagging uh oh
                }

                memset(Engine.videoFrameBuffer, 0, (videoWidth * videoHeight) * sizeof(uint));
                uint px = 0;
                for (uint i = 0; i < (videoWidth * videoHeight) * sizeof(uint); i += sizeof(uint)) {
                    Engine.videoFrameBuffer[px++] = (videoVidData->pixels[i + 3] << 24 | videoVidData->pixels[i] << 16
                                                     | videoVidData->pixels[i + 1] << 8 | videoVidData->pixels[i + 2] << 0);
                }

                THEORAPLAY_freeVideo(videoVidData);
                videoVidData = NULL;
            }

            return 2; // its playing as expected
        }
    }

    return 0; // its not even initialised
}

void StopVideoPlayback()
{
    if (videoPlaying) {
        // `videoPlaying` and `videoDecoder` are read by
        // the audio thread, so lock it to prevent a race
        // condition that results in invalid memory accesses.
        SDL_LockAudio();

        if (videoSkipped && fadeMode >= 0xFF)
            fadeMode = 0;

        if (videoVidData) {
            THEORAPLAY_freeVideo(videoVidData);
            videoVidData = NULL;
        }
        if (videoDecoder) {
            THEORAPLAY_stopDecode(videoDecoder);
            videoDecoder = NULL;
        }

        CloseVideoBuffer();
        videoPlaying = false;

        SDL_UnlockAudio();
    }
}


void SetupVideoBuffer(int width, int height) {
    Engine.videoBuffer = SDL_CreateTexture(Engine.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);

    if (!Engine.videoBuffer) 
        printLog("Failed to create video buffer!");

    Engine.videoFrameBuffer = new uint[width * height];
}
void CloseVideoBuffer() {
    if (videoPlaying) {
        if (Engine.videoFrameBuffer) {
            delete[] Engine.videoFrameBuffer;
            Engine.videoFrameBuffer = nullptr;
        }

        SDL_DestroyTexture(Engine.videoBuffer);
        Engine.videoBuffer = nullptr;
    }
}
