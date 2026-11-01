// play screen - resolve a video, then stream it with the simple-lib-av engine.
//
// Both stream urls are handed straight to the player: simple-lib-av opens each through
// openHttpStream (the http module), so each demuxer reads the moov and then each sample
// on demand by HTTP range - nothing is downloaded in full. Adaptive picks are split: a
// video-only stream plus an audio-only stream, each its own independent http stream.
//
// Follows file-manager's video overlay: the open work runs on a worker
// (createVideoPlayer does blocking network I/O and must stay off the UI thread),
// the UI thread adopts the built player and pulls frames each render frame.

#include "screens/play.h"
#include "extractor.h"
#include "stream-select.h"   // pickBestVideo / pickBestAudio (shared with download)

#include "gfx.h"
#include "theme.h"
#include "colors.h"   // COLOR_BLACK: the letterbox bars are black whatever the theme
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/volume-meter.h"
#include "button-repeat.h"     // left/right scrub auto-repeat
#include "thread.h"
#include "screen-manager.h"
#include "string-utilities.h"   // strCopy

#include "video-player.h"
#include "http.h"               // cancel network I/O before joining playback workers
#include "image-loader.h"       // decode the autoplay thumbnail without blocking the render thread
#include "storage.h"            // resume position (get/setWatchedPosition)
#include "settings.h"           // sponsorblock-mode (which segment categories auto-skip)
#include "sponsorblock.h"       // skip segments (auto-skip + seek-bar marks)
#include "subtitles.h"          // fetch + parse a caption track; cue lookup during playback
#include "chapters.h"           // description timestamp lines -> chapter list
#include "dbg.h"                // logInfo/logError (bridge)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>              // snprintf (stats overlay)
#include <sys/sys_time.h>       // sys_time_get_system_time (fps measure)
#include <math.h>                // autoplay countdown ring

// volume: up/down step the shared pill meter (ui/volume-meter.h), pills in the theme accent.
#define VOLUME_DEFAULT    12

// seek controls (bar + time) show on activity / pause, then auto-hide. left/right scrub the target
// instantly; the engine seek (a decoder flush + fragment reload each time) fires once input goes quiet.
#define SEEK_STEP_SECONDS   5.0f
#define SEEK_APPLY_IDLE_US  400000ULL
#define CONTROLS_VISIBLE_US 3000000ULL
#define TOAST_VISIBLE_US    2000000ULL   // how long a toast ("Skipped ...", "Subtitles: ...") stays up
#define AUTOPLAY_NOTICE_US  3000000ULL
#define AUTOPLAY_THUMB_CAP  (128 * 1024)

// subtitles: triangle cycles off -> track 1 -> track 2 -> ... -> off; the active cue draws above the
// seek bar, and the HUD shows the chosen language top-right while the controls are up.
#define SUBTITLE_TEXT_SIZE 30
#define MAX_AUDIO_TRACKS 128

// description overlay: SELECT toggles the description full-screen; up/down scroll it.
#define DESCRIPTION_MARGIN_X    120
#define DESCRIPTION_MARGIN_Y    80
#define DESCRIPTION_TEXT_SIZE   24
#define DESCRIPTION_SCROLL_STEP 40

// chapter picker: START toggles a centred list of the video's chapters; up/down select
// (the window follows the selection), cross jumps there, circle/START close without jumping.
#define CHAPTER_TEXT_SIZE    24
#define CHAPTER_ROW_HEIGHT   46
#define CHAPTER_VISIBLE_ROWS 12
#define CHAPTER_TIME_GAP     24   // between the right-aligned time column and the title column

// the video to play, set by playVideo() before the screen is pushed. defaults to
// a short public clip so pushing the screen directly still does something.
static char requestedInput[256] = "jNQXAC9IVRw";   // "Me at the zoo"

// Playback navigation: the current video plus the two most recent predecessors.
// The cursor allows L1 to move backward; a new video after going back discards the forward branch.
#define PLAYBACK_NAV_CAP 3
static char playbackNavIds[PLAYBACK_NAV_CAP][12];
static int playbackNavCount;
static int playbackNavCursor = -1;

static int playbackIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int extractPlaybackId(const char *input, char out[12])
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   if (!input || !input[0]) return 0;
   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id && strlen(input) == 11) id = input;
   if (!id) return 0;
   for (int i = 0; i < 11; i++) {
      if (!id[i] || !playbackIdChar(id[i])) return 0;
      out[i] = id[i];
   }
   out[11] = 0;
   return 1;
}

static void appendPlaybackNavigation(const char *input)
{
   char id[12];
   if (!extractPlaybackId(input, id)) return;

   // If the user went back, a newly selected video starts a new forward branch.
   if (playbackNavCursor >= 0 && playbackNavCursor < playbackNavCount - 1)
      playbackNavCount = playbackNavCursor + 1;
   if (playbackNavCount == PLAYBACK_NAV_CAP) {
      memmove(playbackNavIds, playbackNavIds + 1,
              (PLAYBACK_NAV_CAP - 1) * sizeof playbackNavIds[0]);
      playbackNavCount--;
      if (playbackNavCursor > 0) playbackNavCursor--;
   }
   if (playbackNavCount > 0 && strcmp(playbackNavIds[playbackNavCount - 1], id) == 0) {
      playbackNavCursor = playbackNavCount - 1;
      return;
   }
   strCopy(playbackNavIds[playbackNavCount++], sizeof playbackNavIds[0], id);
   playbackNavCursor = playbackNavCount - 1;
}

static int previousPlaybackId(char out[12])
{
   if (playbackNavCursor <= 0) return 0;
   playbackNavCursor--;
   strCopy(out, 12, playbackNavIds[playbackNavCursor]);
   return 1;
}

static int nextPlaybackId(char out[12])
{
   if (playbackNavCursor < 0 || playbackNavCursor >= playbackNavCount - 1) return 0;
   playbackNavCursor++;
   strCopy(out, 12, playbackNavIds[playbackNavCursor]);
   return 1;
}

// Session-only autoplay history: 32 YouTube IDs (11 chars + terminator), reset on app restart.
#define AUTOPLAY_HISTORY_CAP 32
static char autoplayWatchedIds[AUTOPLAY_HISTORY_CAP][12];
static int autoplayWatchedCount;

static int autoplayIsIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void rememberAutoplayVideo(const char *input)
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   if (!input || !input[0]) return;

   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id && strlen(input) == 11) id = input;
   if (!id) return;

   char videoId[12];
   for (int i = 0; i < 11; i++) {
      if (!id[i] || !autoplayIsIdChar(id[i])) return;
      videoId[i] = id[i];
   }
   videoId[11] = 0;

   for (int i = 0; i < autoplayWatchedCount; i++)
      if (strcmp(autoplayWatchedIds[i], videoId) == 0) return;

   if (autoplayWatchedCount == AUTOPLAY_HISTORY_CAP) {
      memmove(autoplayWatchedIds, autoplayWatchedIds + 1,
              (AUTOPLAY_HISTORY_CAP - 1) * sizeof autoplayWatchedIds[0]);
      autoplayWatchedCount--;
   }
   strCopy(autoplayWatchedIds[autoplayWatchedCount++], sizeof autoplayWatchedIds[0], videoId);
}

typedef enum { STAGE_LOADING, STAGE_FAILED, STAGE_PLAYING } Stage;

static struct {
   volatile Stage   stage;
   char             message[128];     // failure reason (worker writes, UI reads)
   VideoPlayer     *pendingPlayer;    // built by the worker, adopted by the UI thread
   volatile int     workerDone;
   int              pauseWhenReady; // preserve Cross presses made before the first frame/player adoption
   int              threadActive;
   sys_ppu_thread_t workerTid;
   volatile int     nextWorkerDone;
   int              nextThreadActive, nextAttempted;
   sys_ppu_thread_t nextWorkerTid;
   char             nextVideoId[16];
   char             nextVideoTitle[256];
   uint8_t         *nextThumbJpeg;
   int              nextThumbJpegLen;
   GfxTexture       nextThumbTexture;
   int              nextNoticeVisible, nextImageRequested;
   uint64_t         nextNoticeStartedUs;
   VideoPlayer     *player;
   int              screenW, screenH;
   int              firstFrameSeen;   // a real frame has been presented (loading status ends, save is safe)
   int              showStats;        // R3 toggles a debug overlay
   char             title[256];        // resolved video title (worker sets, UI reads after adoption)
   char             description[2048]; // resolved video description (worker sets, UI reads after adoption)

   // description overlay: the text is rasterised once, on first open, into one tall texture that
   // up/down scroll. playback carries on untouched underneath.
   int              descriptionVisible;
   int              descriptionRendered;
   int              descriptionScroll;   // pixels scrolled down into the text

   // chapter picker: chapters come out of the description at resolve time; when that finds none, a
   // side worker asks the /next endpoint (YouTube's own chapter list). the row labels are rasterised
   // on first open, and the widest row sizes the panel.
   ChapterList      chapters;            // parsed by the worker (UI reads after adoption)
   ChapterList      fetchedChapters;     // /next result (worker writes, copied over on reap)
   volatile int     chWorkerDone;
   int              chThreadActive, chFetchTried;
   sys_ppu_thread_t chWorkerTid;
   int              chaptersVisible;
   int              chaptersRendered;
   int              chapterSelected;
   int              chapterTimeWidth;    // widest time label (the right-aligned time column)
   int              chapterPanelWidth;
   int              vidItag, vidW, vidH, vidFps, audItag;   // picked formats (worker sets, UI reads)
   char             videoUrls[3][MAX_STREAM_URL];
   char             fallbackAudioUrl[MAX_STREAM_URL];
   int              videoItags[3], videoWidths[3], videoHeights[3], videoFps[3];
   int              videoHasAudio[3];
   int              fallbackAudioItag;
   int              videoAvailable[3], resolutionIndex;
   VideoPlayer     *pendingResolutionPlayer;
   VideoPlayer     *retiringResolutionPlayer;
   volatile int     resolutionSwitchDone;
   int              resolutionSwitchThreadActive, resolutionSwitchWasPaused, resolutionSwitchWasFirstFrameSeen;
   int              resolutionSwitchTarget, resolutionSwitchOldIndex, resolutionSwitchRestored;
   volatile int     resolutionSwitchAborting;
   float            resolutionSwitchPosition;
   char             resolutionSwitchVideoUrl[MAX_STREAM_URL];
   char             resolutionSwitchAudioUrl[MAX_STREAM_URL];
   char             resolutionSwitchOldVideoUrl[MAX_STREAM_URL];
   char             resolutionSwitchOldAudioUrl[MAX_STREAM_URL];
   sys_ppu_thread_t resolutionSwitchTid;
   char             audioUrls[MAX_AUDIO_TRACKS][MAX_STREAM_URL];
   char             audioNames[MAX_AUDIO_TRACKS][64];
   char             audioIds[MAX_AUDIO_TRACKS][40];
   int              audioItags[MAX_AUDIO_TRACKS];
   int              audioCount, audioPick;
   volatile int     audioSwitchDone;
   int              audioSwitchThreadActive, audioSwitchWasPaused;
   int              audioFallbackAttempted;
   sys_ppu_thread_t audioSwitchTid;
   float            audioSwitchPosition;
   int              audioSwitchTarget, audioSwitchFallback;
   int              isLive;           // a live stream: no duration, no resume/save, no seek
   const uint8_t   *lastFrame;        // fps measure: a changed pointer means a newly presented frame
   int              framesThisSecond, measuredFps;
   uint64_t         fpsTickUs;

   // seek: the UI owns the scrub target so the bar moves instantly; the engine seek is deferred until
   // the input goes quiet (each apply costs a decoder flush + fragment reload)
   int              seeking;
   float            seekTarget;
   uint64_t         lastSeekInputUs;
   uint64_t         controlsShownUs;   // last activity, for the bar/time auto-hide

   // sponsorblock: fetched on a side thread (in parallel with resolve/open), then auto-skipped during
   // playback and drawn on the seek bar. sbReady goes up once the thread is reaped and segments are safe.
   SponsorSegments  sb;
   volatile int     sbWorkerDone;
   int              sbThreadActive, sbReady;
   sys_ppu_thread_t sbWorkerTid;
   uint64_t         toastShownUs;      // brief toast timer (0 = hidden)

   // subtitles: triangle picks a track (-1 = off, the default); a worker fetches it in the background
   // (updatePlay respawns the fetch whenever the pick and the fetched track disagree), and once ready
   // the cue for the current position drives subtitleLabel each frame.
   CaptionTrack     captionTracks[MAX_CAPTION_TRACKS];   // copied from the resolve (worker sets)
   int              captionCount;
   int              subtitlePick;        // chosen track index, -1 = off
   int              subtitleFetched;     // track index the fetch (in flight or done) belongs to, -1 = none
   int              subtitlesReady;      // fetched track is safe to read
   int              subtitleScanFrom;    // cue lookup cache (getSubtitleCueText)
   SubtitleTrack   *subtitleTrack;       // heap; ~800 KB, allocated on first enable, freed on exit
   volatile int     subWorkerDone;
   int              subThreadActive;
   sys_ppu_thread_t subWorkerTid;
} state;

#define STAT_LINES 4

static Font  uiFont;
static Label statusLabel;
static Label statLabels[STAT_LINES];
static Label timeLeftLabel, timeRightLabel;   // seek bar: current time (left) / total (right)
static Label toastLabel;                      // brief centred toast ("Skipped ...", "Subtitles: ...")
static Label titleLabel;                      // video title, top-left while the seek bar is up
static Label nextNoticeHeadingLabel;           // "next video" notice heading
static Label nextNoticeTitleLabel;             // selected autoplay video title
static Label subtitleLabel;                   // active subtitle cue, above the seek bar
static Label subsHudLabel;                    // chosen subtitle language, top-right while the HUD is up
static Label chapterLabels[MAX_CHAPTERS];     // chapter titles (picker overlay rows)
static Label chapterTimeLabels[MAX_CHAPTERS]; // chapter start times, right-aligned in their own column
static TextTexture descriptionTexture;        // whole description as one tall texture (scrolled by draw)

// the shared pill meter; its level is kept in `volumeLevel` outside `state` so it survives across videos.
static VolumeMeter volumeMeter;
static int volumeLevel = VOLUME_DEFAULT;
// Playback display preference: deliberately outside `state` so it survives between videos.
// It starts OFF on application launch and is toggled with R2 during playback.
static int cropToFillEnabled;

static const StreamFormat *pickAudioForTrack(const StreamInfo *info, const char *trackId)
{
   const StreamFormat *best = NULL;
   int originalOnly = trackId && strcmp(trackId, "__original_upload__") == 0;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *f = &info->formats[i];
      if (!f->hasAudio || f->hasVideo || f->needsCipher || !f->url[0] || strcmp(f->container, "mp4") != 0) continue;
      if (originalOnly) {
         // YouTube commonly omits audioTrack metadata on the uploaded/original AAC rendition.
         if (f->audioTrackId[0] || f->audioIsAutoDubbed) continue;
      } else if (trackId && trackId[0] && strcmp(f->audioTrackId, trackId) != 0) continue;
      if (!best || f->itag == 140) best = f;
   }
   return best;
}

static void fail(const char *reason)
{
   logError("[yt] play failed: %s\n", reason);
   strCopy(state.message, sizeof state.message, reason);
   __sync_synchronize();   // publish message before the UI thread sees STAGE_FAILED
   state.stage = STAGE_FAILED;
}

// only languages FONT_POP renders reliably (hardware-checked: western/central europe + japanese fine,
// vietnamese diacritics broken). prefix match, so "en-GB" passes as "en". extend after testing a new
// language on the console.
static int isRenderableSubtitleLanguage(const char *code)
{
   static const char *supported[] = { "en", "es", "pt", "fr", "de", "it", "nl", "sv", "da", "no", "fi",
                                      "pl", "cs", "sk", "hu", "ro", "tr", "id", "ms", "ja" };
   for (int i = 0; i < (int)(sizeof supported / sizeof *supported); i++)
      if (strncmp(code, supported[i], strlen(supported[i])) == 0) return 1;
   return 0;
}

static void worker(uint64_t arg)
{
   (void)arg;
   StreamInfo *info = malloc(sizeof *info);   // large format/URL table; keep it off the stack
   const Extractor *extractor = findExtractor(requestedInput);
   if (!info || !extractor) { fail("unrecognised link or id"); goto done; }

   // resolve (stage stays STAGE_LOADING, set by initPlay's memset, until we adopt or fail)
   if (extractor->extract(requestedInput, info) != 0 || info->formatCount == 0) { fail("resolve failed"); goto done; }
   strCopy(state.title, sizeof state.title, info->title);
   strCopy(state.description, sizeof state.description, info->description);
   parseChapters(state.description, &state.chapters);
   for (int i = 0; i < info->captionCount; i++)   // keep only tracks the system font can render
      if (isRenderableSubtitleLanguage(info->captions[i].languageCode))
         state.captionTracks[state.captionCount++] = info->captions[i];

   const StreamFormat *video = pickBestVideo(info);
   if (!video) { fail("no playable mp4 video"); goto done; }
   for (int profile = 0; profile < 3; profile++) {
      const StreamFormat *candidate = pickVideoForProfile(info, profile);
      if (!candidate) continue;
      strCopy(state.videoUrls[profile], sizeof state.videoUrls[profile], candidate->url);
      state.videoItags[profile] = candidate->itag;
      state.videoWidths[profile] = candidate->width;
      state.videoHeights[profile] = candidate->height;
      state.videoFps[profile] = candidate->fps;
      state.videoHasAudio[profile] = candidate->hasAudio;
      state.videoAvailable[profile] = 1;
   }
   // a muxed pick (itag 18) already carries audio; a video-only pick needs a separate audio track
   const StreamFormat *audio = video->hasAudio ? NULL : pickBestAudio(info);
   if (!audio && video->hasAudio) {
      const StreamFormat *fallbackAudio = pickBestAudio(info);
      if (fallbackAudio) {
         strCopy(state.fallbackAudioUrl, sizeof state.fallbackAudioUrl, fallbackAudio->url);
         state.fallbackAudioItag = fallbackAudio->itag;
      }
   }
   if (video->hasAudio && !state.fallbackAudioUrl[0]) {
      // If there is no separate AAC rendition to carry across a switch, only offer
      // profiles whose video stream itself contains audio.
      for (int profile = 0; profile < 3; profile++)
         if (!state.videoHasAudio[profile]) state.videoAvailable[profile] = 0;
   }
   state.audioPick = -1;
   state.audioCount = 0;
   if (!video->hasAudio) {
      // Put the uploaded/original AAC rendition first so it cannot be crowded out by
      // the language-ordered auto-dub formats in YouTube's response.
      {
         const StreamFormat *original = pickAudioForTrack(info, "__original_upload__");
         if (original) {
            int n = state.audioCount++;
            strCopy(state.audioIds[n], sizeof state.audioIds[n], "__original_upload__");
            strCopy(state.audioNames[n], sizeof state.audioNames[n], "Original upload");
            strCopy(state.audioUrls[n], sizeof state.audioUrls[n], original->url);
            state.audioItags[n] = original->itag;
            logInfo("[audio-diag] preserved original AAC itag=%d urlLen=%d\n",
                    original->itag, (int)strlen(original->url));
         }
      }
      // Add explicitly original/non-dubbed language tracks before auto-dubs, regardless
      // of YouTube's format ordering. Bitrate/codec variants remain internal.
      for (int pass = 0; pass < 2 && state.audioCount < MAX_AUDIO_TRACKS; pass++) {
         for (int i = 0; i < info->formatCount && state.audioCount < MAX_AUDIO_TRACKS; i++) {
            const StreamFormat *f = &info->formats[i];
            if (!f->hasAudio || f->hasVideo || !f->audioTrackId[0]) continue;
            if ((f->audioIsAutoDubbed ? 1 : 0) != pass) continue;
            int found = 0;
            for (int j = 0; j < state.audioCount; j++)
               if (strcmp(state.audioIds[j], f->audioTrackId) == 0) found = 1;
            if (found) continue;
            const StreamFormat *best = pickAudioForTrack(info, f->audioTrackId);
            if (!best) continue;
            int n = state.audioCount++;
            strCopy(state.audioIds[n], sizeof state.audioIds[n], best->audioTrackId);
            if (best->audioIsAutoDubbed)
               snprintf(state.audioNames[n], sizeof state.audioNames[n], "%s (auto-dubbed)", best->audioTrackName[0] ? best->audioTrackName : best->audioTrackId);
            else
               strCopy(state.audioNames[n], sizeof state.audioNames[n], best->audioTrackName[0] ? best->audioTrackName : best->audioTrackId);
            strCopy(state.audioUrls[n], sizeof state.audioUrls[n], best->url);
            state.audioItags[n] = best->itag;
         }
      }
      int defaultPick = -1;
      // Match an explicitly identified non-dubbed original first. Never default to an auto-dub.
      for (int j = 0; j < state.audioCount; j++) {
         if (strcmp(state.audioIds[j], "__original_upload__") == 0) { defaultPick = j; break; }
         const StreamFormat *candidate = pickAudioForTrack(info, state.audioIds[j]);
         if (candidate && !candidate->audioIsAutoDubbed) { defaultPick = j; break; }
      }
      // Match YouTube's declared default only if it is not marked as an auto-dub.
      if (defaultPick < 0 && info->defaultAudioTrackId[0])
         for (int j = 0; j < state.audioCount; j++)
            if (strcmp(state.audioIds[j], info->defaultAudioTrackId) == 0) {
               const StreamFormat *candidate = pickAudioForTrack(info, state.audioIds[j]);
               if (candidate && !candidate->audioIsAutoDubbed) defaultPick = j;
               break;
            }
      // Some player responses omit the captions-level ID but mark a default adaptive format.
      if (defaultPick < 0)
         for (int i = 0; i < info->formatCount && defaultPick < 0; i++) {
            const StreamFormat *f = &info->formats[i];
            if (!f->hasAudio || f->audioIsAutoDubbed || !f->audioIsDefault || !f->audioTrackId[0]) continue;
            for (int j = 0; j < state.audioCount; j++)
               if (strcmp(state.audioIds[j], f->audioTrackId) == 0) { defaultPick = j; break; }
         }
      logInfo("[audio-diag] video defaultAudioTrackId='%s' defaultPick=%d tracks=%d\n",
              info->defaultAudioTrackId, defaultPick, state.audioCount);
      for (int i = 0; i < state.audioCount; i++) {
         const StreamFormat *trackFormat = pickAudioForTrack(info, state.audioIds[i]);
         logInfo("[audio-diag] track[%d] id='%s' name='%s' itag=%d default=%d autoDubbed=%d url=%d\n",
                 i, state.audioIds[i], state.audioNames[i], state.audioItags[i],
                 trackFormat ? trackFormat->audioIsDefault : 0,
                 trackFormat ? trackFormat->audioIsAutoDubbed : 0,
                 state.audioUrls[i][0] != 0);
      }
      if (state.audioCount > 0) {
         state.audioPick = defaultPick;
         if (defaultPick >= 0) {
            audio = pickAudioForTrack(info, state.audioIds[defaultPick]);
            logInfo("[audio-diag] selected default index=%d id='%s' name='%s' itag=%d\n",
                    defaultPick, state.audioIds[defaultPick], state.audioNames[defaultPick], audio ? audio->itag : 0);
         } else {
            audio = NULL;
            logWarn("[audio-diag] declared default audio track is not available as a selectable stream; starting without an arbitrary dubbed track\n");
         }
      } else {
         logWarn("[audio-diag] no selectable AAC audio tracks parsed\n");
      }
   }
   logInfo("[yt] play video itag %d %dx%d %s, audio itag %d\n", video->itag, video->width, video->height,
           video->hasAudio ? "muxed" : "video-only", audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   state.vidItag = video->itag; state.vidW = video->width; state.vidH = video->height; state.vidFps = video->fps;
   state.resolutionIndex = video->height <= 480 ? 0 : (video->height <= 720 ? 1 : 2);
   // Preserve the exact initial pick in the matching profile slot. It may be a muxed format
   // selected by the normal selector, while the profile helper found a different adaptive URL.
   strCopy(state.videoUrls[state.resolutionIndex], sizeof state.videoUrls[state.resolutionIndex], video->url);
   state.videoItags[state.resolutionIndex] = video->itag;
   state.videoWidths[state.resolutionIndex] = video->width;
   state.videoHeights[state.resolutionIndex] = video->height;
   state.videoFps[state.resolutionIndex] = video->fps;
   state.videoHasAudio[state.resolutionIndex] = video->hasAudio;
   state.videoAvailable[state.resolutionIndex] = 1;
   state.audItag = audio ? audio->itag : (video->hasAudio ? video->itag : 0);
   if (state.audioPick >= 0 && state.audioPick < state.audioCount) snprintf(state.message, sizeof state.message, "Loading audio: %s", state.audioNames[state.audioPick]);
   state.isLive = video->isLiveSegmented;

   // Open both streams. A failed separate audio open still permits silent video playback.
   state.pendingPlayer = createVideoPlayerSplit(video->url, audio ? audio->url : NULL, allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (!state.pendingPlayer) { fail("couldn't open stream"); goto done; }

   // resume where we left off. seekVideoPlayer only posts the target; the decode thread does the
   // reconnect + fragment reload. drawPlay keeps the "Loading..." status up until the first frame is
   // presented, so that reload is covered rather than showing a black screen. storage returns 0 for a
   // typed url (never a history key), so this no-ops for anything but a bare videoId.
   {
      int resumeAt = state.isLive ? 0 : getWatchedPosition(requestedInput);
      float duration = getVideoDurationSeconds(state.pendingPlayer);
      if (resumeAt > 3 && (duration <= 0 || resumeAt < duration - 10)) seekVideoPlayer(state.pendingPlayer, (float)resumeAt);
   }

done:
   free(info);
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
}

// a bare 11-char videoId (not a typed url) is what SponsorBlock and history key on; typed urls no-op.
static int isBareVideoId(const char *input)
{
   return input[0] && !strchr(input, '/') && !strchr(input, ':');
}

// fetch skip segments in parallel with resolve/open, so they cost no startup latency.
static void sponsorWorker(uint64_t arg)
{
   (void)arg;
   fetchSponsorSegments(requestedInput, &state.sb);
   __sync_synchronize();
   state.sbWorkerDone = 1;
   exitThread();
}

// asks /next for youtube's own chapter list; spawned only when the description parse found none.
static void chapterWorker(uint64_t arg)
{
   (void)arg;
   fetchChapters(requestedInput, &state.fetchedChapters);
   __sync_synchronize();
   state.chWorkerDone = 1;
   exitThread();
}

// Resolve YouTube's next recommendation off the UI thread; playback is untouched if this fails.
static void autoplayWorker(uint64_t arg)
{
   (void)arg;
   if (getNextVideoId(requestedInput, state.nextVideoId, sizeof state.nextVideoId,
                      state.nextVideoTitle, sizeof state.nextVideoTitle,
                      autoplayWatchedIds, autoplayWatchedCount) == 0) {
      uint8_t *jpeg = (uint8_t *)malloc(AUTOPLAY_THUMB_CAP);
      if (jpeg) {
         char url[128];
         snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", state.nextVideoId);
         HttpHeader headers[] = {
            { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/125.0.0.0 Safari/537.36" },
            { "Accept-Encoding", "identity" }
         };
         int length = 0, status = 0;
         int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)jpeg,
                            AUTOPLAY_THUMB_CAP, &length, &status);
         if (rc == 0 && status == 200 && length > 0) {
            state.nextThumbJpeg = jpeg;
            state.nextThumbJpegLen = length;
         } else {
            logWarn("[yt] autoplay thumbnail failed rc=%d status=%d\n", rc, status);
            free(jpeg);
         }
      }
   }
   __sync_synchronize();
   state.nextWorkerDone = 1;
   exitThread();
}

static void launchVideo(const char *input, int addToNavigation)
{
   strCopy(requestedInput, sizeof requestedInput, input);
   if (addToNavigation) appendPlaybackNavigation(input);
   pushScreen(&playScreen);
}

void playVideo(const char *input)
{
   launchVideo(input, 1);
}

static void initPlay(void)
{
   setHttpRequestsCancelled(0);
   memset(&state, 0, sizeof state);
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();

   uiFont = openSystemFont(FONT_POP);
   initLabel(&statusLabel, &uiFont, 0, 0, 1400, AUTO, 28, activeTheme->textPrimary, TEXT_NOWRAP, "");
   for (int i = 0; i < STAT_LINES; i++)
      initLabel(&statLabels[i], &uiFont, 0, 0, 600, AUTO, 22, activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&timeLeftLabel,  &uiFont, 0, 0, 200, AUTO, 22, activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabel(&timeRightLabel, &uiFont, 0, 0, 200, AUTO, 22, activeTheme->textPrimary, TEXT_NOWRAP, "");
   initLabelRaw(&toastLabel, &uiFont, 0, 0, 400, AUTO, 22, activeTheme->textPrimary, TEXT_NOWRAP, "");   // raw: shows track names
   initLabelRaw(&titleLabel, &uiFont, 0, 0, 1100, AUTO, 22, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabelRaw(&nextNoticeHeadingLabel, &uiFont, 0, 0, 500, AUTO, 22, activeTheme->accent, TEXT_NOWRAP, "NEXT VIDEO");
   initLabelRaw(&nextNoticeTitleLabel, &uiFont, 0, 0, 1040, AUTO, 30, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabelRaw(&subtitleLabel, &uiFont, 0, 0, state.screenW - 160, AUTO, SUBTITLE_TEXT_SIZE, activeTheme->textPrimary, TEXT_WRAP, "");
   initLabelRaw(&subsHudLabel, &uiFont, 0, 0, 500, AUTO, 22, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
   for (int i = 0; i < MAX_CHAPTERS; i++) {
      initLabelRaw(&chapterLabels[i], &uiFont, 0, 0, 990, AUTO, CHAPTER_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&chapterTimeLabels[i], &uiFont, 0, 0, 150, AUTO, CHAPTER_TEXT_SIZE, activeTheme->textSecondary, TEXT_NOWRAP, "");
   }
   state.subtitlePick = state.subtitleFetched = -1;   // default off (memset left them 0)

   initVolumeMeter(&volumeMeter, &uiFont, activeTheme->accent, volumeLevel);
   layoutVolumeMeter(&volumeMeter, state.screenW, state.screenH);

   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-play") == 0);
   if (!state.threadActive) fail("couldn't start worker");

   if (getSponsorblockMode() != SPONSORBLOCK_OFF && isBareVideoId(requestedInput))
      state.sbThreadActive = (spawnJoinableThread(&state.sbWorkerTid, sponsorWorker, 0,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-sponsor") == 0);
}

static void showControls(void) { state.controlsShownUs = sys_time_get_system_time(); }

static void formatTime(char *buffer, int cap, int seconds)
{
   if (seconds < 0) seconds = 0;
   if (seconds >= 3600) snprintf(buffer, cap, "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
   else                 snprintf(buffer, cap, "%d:%02d", seconds / 60, seconds % 60);
}

static void showToast(const char *text)
{
   setLabelText(&toastLabel, text);
   state.toastShownUs = sys_time_get_system_time();
   showControls();
}

// fetches the picked caption track in the background; updatePlay reaps it and respawns on a new pick.
static void subtitleWorker(uint64_t arg)
{
   (void)arg;
   fetchSubtitles(state.captionTracks[state.subtitleFetched].url, state.subtitleTrack);   // a failure leaves count 0
   __sync_synchronize();
   state.subWorkerDone = 1;
   exitThread();
}

// triangle: cycle off -> track 1 -> ... -> off; the pick shows in the top-right HUD badge. the fetch
// itself is driven by updatePlay, so a mid-fetch switch just supersedes the old pick.
static void cycleSubtitles(void)
{
   // live captions use a rolling delivery our fetcher can't parse, so live streams count as none
   if (state.captionCount == 0 || state.isLive) { showToast("No subtitles"); return; }
   state.subtitlePick = state.subtitlePick + 1 < state.captionCount ? state.subtitlePick + 1 : -1;

   setLabelText(&subtitleLabel, "");   // clear the old track's cue; the new one repopulates once fetched

   char hud[64];
   if (state.subtitlePick < 0) strCopy(hud, sizeof hud, "Subtitles: Off");
   else snprintf(hud, sizeof hud, "Subtitles: %s", state.captionTracks[state.subtitlePick].name);
   setLabelText(&subsHudLabel, hud);
   showControls();
}

static void audioSwitchWorker(uint64_t arg)
{
   (void)arg;
   int target = state.audioSwitchTarget;
   logInfo("[audio-diag] L3 switch begin old=%d target=%d id='%s' posMs=%d wasPaused=%d urlLen=%d\n",
           state.audioPick, target, state.audioIds[target],
           (int)(state.audioSwitchPosition * 1000.0f), state.audioSwitchWasPaused,
           (int)strlen(state.audioUrls[target]));
   int rc = replaceVideoPlayerAudio(state.player, state.audioUrls[target], state.audioSwitchPosition);
   state.audioSwitchFallback = rc == 0;
   if (rc == 0)
      logInfo("[audio-diag] L3 switch accepted target=%d itag=%d\n", target, state.audioItags[target]);
   else
      logWarn("[audio-diag] L3 switch failed target=%d; previous video remains active\n", target);
   __sync_synchronize();
   state.audioSwitchDone = 1;
   exitThread();
}

static VideoPlayer *openReadyResolutionPlayer(const char *videoUrl, const char *audioUrl, float position)
{
   VideoPlayer *replacement = createVideoPlayerSplit(videoUrl, audioUrl && audioUrl[0] ? audioUrl : NULL,
                                                     allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (!replacement) return NULL;
   if (audioUrl && audioUrl[0] && !isVideoPlayerUsingSeparateAudio(replacement)) {
      logWarn("[video-diag] replacement video did not open the requested separate audio source\n");
      destroyVideoPlayer(replacement);
      return NULL;
   }
   if (position > 0.5f) seekVideoPlayer(replacement, position);
   setVideoPaused(replacement, 1);

   // Only one VDEC instance is kept alive during a resolution change. The previous player is
   // retired before this function is called, avoiding a second decoder's SPU/work-memory demand.
   uint64_t started = sys_time_get_system_time();
   while (!isVideoPlayerReady(replacement) &&
          sys_time_get_system_time() - started < 15000000ull) sleepMs(50);
   if (!isVideoPlayerReady(replacement)) {
      logWarn("[video-diag] resolution stream did not reach decoded pre-roll\n");
      destroyVideoPlayer(replacement);
      return NULL;
   }
   return replacement;
}

static void resolutionSwitchWorker(uint64_t arg)
{
   (void)arg;
   VideoPlayer *oldPlayer = state.retiringResolutionPlayer;
   state.retiringResolutionPlayer = NULL;
   if (oldPlayer) destroyVideoPlayer(oldPlayer);

   VideoPlayer *replacement = NULL;
   if (!state.resolutionSwitchAborting)
      replacement = openReadyResolutionPlayer(state.resolutionSwitchVideoUrl,
                                               state.resolutionSwitchAudioUrl,
                                               state.resolutionSwitchPosition);

   // If the requested rendition cannot be opened, recover the previous rendition before
   // reporting completion. This avoids leaving playback blank after a failed manual switch.
   if (!replacement && !state.resolutionSwitchAborting) {
      logWarn("[video-diag] requested resolution failed; reopening previous rendition\n");
      replacement = openReadyResolutionPlayer(state.resolutionSwitchOldVideoUrl,
                                                state.resolutionSwitchOldAudioUrl,
                                                state.resolutionSwitchPosition);
      state.resolutionSwitchRestored = replacement != NULL;
   }

   if (!replacement && !state.resolutionSwitchAborting) {
      strCopy(state.message, sizeof state.message, "Resolution switch and recovery failed");
      state.stage = STAGE_FAILED;
   }
   state.pendingResolutionPlayer = replacement;
   __sync_synchronize();
   state.resolutionSwitchDone = 1;
   exitThread();
}

static int startResolutionSwitch(int target, int automaticFallback)
{
   if (target < 0 || target >= 3 || !state.videoAvailable[target] ||
       !state.player || state.resolutionSwitchThreadActive || state.audioSwitchThreadActive ||
       state.seeking || state.isLive) return 0;
   if (strcmp(state.videoUrls[target], state.videoUrls[state.resolutionIndex]) == 0) return 0;

   state.resolutionSwitchTarget = target;
   state.resolutionSwitchOldIndex = state.resolutionIndex;
   state.resolutionSwitchRestored = 0;
   state.resolutionSwitchAborting = 0;
   state.resolutionSwitchWasPaused = isVideoPaused(state.player);
   state.resolutionSwitchWasFirstFrameSeen = state.firstFrameSeen;
   state.resolutionSwitchPosition = getVideoPositionSeconds(state.player);
   strCopy(state.resolutionSwitchVideoUrl, sizeof state.resolutionSwitchVideoUrl, state.videoUrls[target]);
   strCopy(state.resolutionSwitchOldVideoUrl, sizeof state.resolutionSwitchOldVideoUrl,
           state.videoUrls[state.resolutionIndex]);
   state.resolutionSwitchAudioUrl[0] = 0;
   state.resolutionSwitchOldAudioUrl[0] = 0;
   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      strCopy(state.resolutionSwitchAudioUrl, sizeof state.resolutionSwitchAudioUrl, state.audioUrls[state.audioPick]);
   else if (state.fallbackAudioUrl[0])
      strCopy(state.resolutionSwitchAudioUrl, sizeof state.resolutionSwitchAudioUrl, state.fallbackAudioUrl);
   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      strCopy(state.resolutionSwitchOldAudioUrl, sizeof state.resolutionSwitchOldAudioUrl, state.audioUrls[state.audioPick]);
   else if (state.fallbackAudioUrl[0] && isVideoPlayerUsingSeparateAudio(state.player))
      strCopy(state.resolutionSwitchOldAudioUrl, sizeof state.resolutionSwitchOldAudioUrl, state.fallbackAudioUrl);

   setVideoPaused(state.player, 1);
   if (state.firstFrameSeen && !state.isLive)
      setWatchedPosition(requestedInput, (int)state.resolutionSwitchPosition);
   finishGfx();   // retire the displayed frame before the worker frees its RSX-visible buffers
   state.retiringResolutionPlayer = state.player;
   state.player = NULL;
   state.firstFrameSeen = 0;
   state.resolutionSwitchDone = 0;
   state.resolutionSwitchThreadActive = (spawnJoinableThread(&state.resolutionSwitchTid, resolutionSwitchWorker, 0,
                                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-resolution") == 0);
   if (!state.resolutionSwitchThreadActive) {
      state.player = state.retiringResolutionPlayer;
      state.retiringResolutionPlayer = NULL;
      if (!state.resolutionSwitchWasPaused && state.player) setVideoPaused(state.player, 0);
      state.firstFrameSeen = state.resolutionSwitchWasFirstFrameSeen;
      showToast("Resolution switch could not start");
      return 0;
   }
   {
      char notice[96];
      if (state.videoFps[target] > 0) {
         if (automaticFallback)
            snprintf(notice, sizeof notice, "Audio issue; reducing to %dp @ %d FPS",
                     state.videoHeights[target], state.videoFps[target]);
         else
            snprintf(notice, sizeof notice, "Changing to %dp @ %d FPS",
                     state.videoHeights[target], state.videoFps[target]);
      } else {
         if (automaticFallback)
            snprintf(notice, sizeof notice, "Audio issue; reducing to %dp", state.videoHeights[target]);
         else
            snprintf(notice, sizeof notice, "Changing to %dp", state.videoHeights[target]);
      }
      showToast(notice);
   }
   return 1;
}

static int findLowerResolutionProfile(void)
{
   for (int profile = state.resolutionIndex - 1; profile >= 0; profile--) {
      if (!state.videoAvailable[profile]) continue;
      if (strcmp(state.videoUrls[profile], state.videoUrls[state.resolutionIndex]) == 0) continue;
      return profile;
   }
   return -1;
}

static void cycleResolution(void)
{
   if (state.resolutionSwitchThreadActive || state.audioSwitchThreadActive || state.seeking) {
      showToast("Wait for the current operation");
      return;
   }
   for (int step = 1; step <= 3; step++) {
      int target = (state.resolutionIndex + step) % 3;
      if (target != state.resolutionIndex && state.videoAvailable[target] &&
          strcmp(state.videoUrls[target], state.videoUrls[state.resolutionIndex]) != 0) {
         state.audioFallbackAttempted = 0;
         startResolutionSwitch(target, 0);
         return;
      }
   }
   showToast("No other resolution available");
}

static void cycleAudioTrack(void)
{
   if (state.seeking) {
      logWarn("[audio-diag] L3 ignored while a seek transaction is pending\n");
      showToast("Wait for seek to finish before switching audio");
      return;
   }
   if (state.audioCount < 2 || state.isLive || !state.player || state.audioSwitchThreadActive) {
      showToast(state.audioCount < 2 ? "No alternate audio tracks" : "Audio track busy/unavailable");
      return;
   }
   state.audioSwitchPosition = getVideoPositionSeconds(state.player);
   state.audioSwitchWasPaused = isVideoPaused(state.player);
   state.audioSwitchTarget = (state.audioPick + 1) % state.audioCount;
   state.audioFallbackAttempted = 0;
   state.audioSwitchFallback = 0;
   setVideoPaused(state.player, 1);
   snprintf(state.message, sizeof state.message, "Loading audio: %s", state.audioNames[state.audioSwitchTarget]);
   { char notice[96]; snprintf(notice, sizeof notice, "Loading audio: %s", state.audioNames[state.audioSwitchTarget]); showToast(notice); }
   state.audioSwitchDone = 0;
   state.audioSwitchThreadActive = (spawnJoinableThread(&state.audioSwitchTid, audioSwitchWorker, 0,
                                    THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-audio") == 0);
   if (!state.audioSwitchThreadActive) {
      state.message[0] = 0;
      if (!state.audioSwitchWasPaused) setVideoPaused(state.player, 0);
      showToast("Audio switch failed");
   }
}

// accumulates a scrub target while left/right is held; the engine seek fires once input goes quiet
static void nudgeSeek(float deltaSeconds, uint64_t nowUs)
{
   if (!state.seeking) { state.seeking = 1; state.seekTarget = getVideoPositionSeconds(state.player); }
   state.seekTarget += deltaSeconds;
   float duration = getVideoDurationSeconds(state.player);
   if (state.seekTarget < 0.0f) state.seekTarget = 0.0f;
   if (duration > 0.0f && state.seekTarget > duration) state.seekTarget = duration;
   state.lastSeekInputUs = nowUs;
   showControls();
}

// left/right scrub, cross toggles pause (restarts if the video ended). only once playing.
static void handlePlaybackInput(void)
{
   if (state.audioSwitchThreadActive || state.resolutionSwitchThreadActive)
      return; // keep the source stable until the staged handoff commits
   uint64_t nowUs = sys_time_get_system_time();

   if (isPadButtonPressed(PAD_BTN_R2)) {
      cropToFillEnabled = !cropToFillEnabled;
      showToast(cropToFillEnabled ? "Crop to fill: ON" : "Crop to fill: OFF");
   }

   if (isPadButtonPressed(PAD_BTN_SQUARE)) cycleResolution();

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (isVideoEnded(state.player)) seekVideoPlayer(state.player, 0.0f);
      else setVideoPaused(state.player, !isVideoPaused(state.player));
      showControls();
   }

   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) cycleSubtitles();
   if (isPadButtonPressed(PAD_BTN_L3)) cycleAudioTrack();

   if (isPadButtonPressed(PAD_BTN_L1)) {
      char previousId[12];
      if (previousPlaybackId(previousId)) {
         popScreen();
         launchVideo(previousId, 0);
         return;
      }
      showToast("No previous video");
   }
   if (isPadButtonPressed(PAD_BTN_R1)) {
      char forwardId[12];
      if (state.nextThreadActive || state.nextNoticeVisible) {
         showToast("Next video is already loading");
      } else if (nextPlaybackId(forwardId)) {
         // Reuse the already-known forward entry before asking YouTube for another video.
         popScreen();
         launchVideo(forwardId, 0);
         return;
      } else if (state.isLive) {
         showToast("Unavailable for live streams");
      } else {
         state.nextAttempted = 1;
         state.nextWorkerDone = 0;
         state.nextVideoId[0] = 0;
         state.nextVideoTitle[0] = 0;
         state.nextThreadActive = (spawnJoinableThread(&state.nextWorkerTid, autoplayWorker, 0,
                                   THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-next") == 0);
         if (!state.nextThreadActive) showToast("Next video unavailable");
         else showToast("Loading next recommendation...");
      }
   }

   // volume: the widget steps itself on up/down; apply + persist the new level (works on live streams too)
   if (handleVolumeMeterInput(&volumeMeter)) {
      volumeLevel = volumeMeter.level;
      setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
   }

   if (state.isLive) return;   // a live stream has no seek index; scrubbing would break the segment stream

   static ButtonRepeat seekRepeat;
   if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_RIGHT)))     nudgeSeek(+SEEK_STEP_SECONDS, nowUs);
   else if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_LEFT))) nudgeSeek(-SEEK_STEP_SECONDS, nowUs);

   if (state.seeking && nowUs - state.lastSeekInputUs >= SEEK_APPLY_IDLE_US) {
      seekVideoPlayer(state.player, state.seekTarget);
      state.seeking = 0;
   }
}

// which categories the sponsorblock-mode setting auto-skips: ads = paid-promotion categories only.
// (every fetched segment still gets marked on the seek bar, whatever the mode.)
static int shouldAutoSkipSponsor(SponsorCategory category)
{
   SponsorblockMode mode = getSponsorblockMode();
   if (mode == SPONSORBLOCK_ALL) return 1;
   return mode == SPONSORBLOCK_ADS &&
          (category == SPONSOR_SPONSOR || category == SPONSOR_SELFPROMO || category == SPONSOR_INTERACTION);
}

// jump past the first not-yet-skipped sponsor segment the playhead has entered. one-shot per segment so a
// manual rewind into it doesn't fight the user.
static void autoSkipSponsor(void)
{
   float pos = getVideoPositionSeconds(state.player);
   for (int i = 0; i < state.sb.count; i++) {
      SponsorSegment *segment = &state.sb.segments[i];
      if (!shouldAutoSkipSponsor(segment->category)) continue;
      if (segment->skipped || pos < segment->start || pos >= segment->end - 0.2f) continue;
      segment->skipped = 1;
      seekVideoPlayerPast(state.player, segment->end);   // land on the next keyframe after the segment, not before it
      char notice[64];
      snprintf(notice, sizeof notice, "Skipped %s", getSponsorCategoryName(segment->category));
      showToast(notice);
      return;
   }
}

// SELECT opens the description: rasterise the text once, off the draw path, as one tall wrapped
// texture the draw scrolls through. playback carries on untouched underneath.
static void openDescription(void)
{
   if (!state.descriptionRendered) {
      renderFontRaw(&descriptionTexture, &uiFont, DESCRIPTION_TEXT_SIZE, state.description[0] ? state.description : "No description.",
                    activeTheme->textPrimary, state.screenW - 2 * DESCRIPTION_MARGIN_X, TEXT_WRAP);
      state.descriptionRendered = 1;
   }
   state.descriptionScroll = 0;
   state.descriptionVisible = 1;
}

// while the description is up it owns the pad: SELECT/circle close it, up/down scroll. playback
// input (seek, pause, back-out) is blocked underneath.
static void handleDescriptionInput(void)
{
   if (isPadButtonPressed(PAD_BTN_SELECT) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      state.descriptionVisible = 0;
      return;
   }

   static ButtonRepeat scrollRepeat;
   if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_DOWN)))    state.descriptionScroll += DESCRIPTION_SCROLL_STEP;
   else if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_UP))) state.descriptionScroll -= DESCRIPTION_SCROLL_STEP;

   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   int maxScroll = descriptionTexture.tex.h > viewHeight ? descriptionTexture.tex.h - viewHeight : 0;
   if (state.descriptionScroll > maxScroll) state.descriptionScroll = maxScroll;
   if (state.descriptionScroll < 0) state.descriptionScroll = 0;
}

// START opens the chapter picker: the rows are rasterised once (off the draw path), the widest time
// and title size the two columns, and the selection starts on the chapter the playhead is in.
static void openChapters(void)
{
   // Avoid fetching YouTube's large /next response on every playback. Fetch only when the user asks
   // for chapters; the request stays off the draw path and can be cancelled cleanly on exit.
   if (state.isLive) { showToast("No chapters"); return; }
   if (state.chapters.count == 0) {
      if (!state.chFetchTried && isBareVideoId(requestedInput)) {
         state.chFetchTried = 1;
         state.chWorkerDone = 0;
         state.chThreadActive = (spawnJoinableThread(&state.chWorkerTid, chapterWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-chapters") == 0);
      }
      showToast(state.chThreadActive ? "Loading chapters..." : "No chapters");
      return;
   }

   if (!state.chaptersRendered) {
      int widestTitle = 0;
      for (int i = 0; i < state.chapters.count; i++) {
         char timeText[16];
         formatTime(timeText, sizeof timeText, (int)state.chapters.chapters[i].start);
         setLabelText(&chapterTimeLabels[i], timeText);
         setLabelText(&chapterLabels[i], state.chapters.chapters[i].title);
         if (chapterTimeLabels[i].tt.tex.w > state.chapterTimeWidth) state.chapterTimeWidth = chapterTimeLabels[i].tt.tex.w;
         if (chapterLabels[i].tt.tex.w > widestTitle) widestTitle = chapterLabels[i].tt.tex.w;
      }
      state.chapterPanelWidth = state.chapterTimeWidth + CHAPTER_TIME_GAP + widestTitle;
      state.chaptersRendered = 1;
   }

   float position = getVideoPositionSeconds(state.player);
   state.chapterSelected = 0;
   for (int i = 0; i < state.chapters.count; i++)
      if (state.chapters.chapters[i].start <= position) state.chapterSelected = i;
   state.chaptersVisible = 1;
}

// while the picker is up it owns the pad: up/down move the selection, cross jumps to the chapter
// (playback carries on paused or playing, only the position moves), circle/START close without
// jumping. playback input is blocked underneath.
static void handleChaptersInput(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_START)) { state.chaptersVisible = 0; return; }

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      state.chaptersVisible = 0;
      state.seeking = 0;   // a pending scrub must not overwrite the jump
      seekVideoPlayer(state.player, state.chapters.chapters[state.chapterSelected].start);
      showControls();
      return;
   }

   static ButtonRepeat navigateRepeat;
   if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_DOWN)))    state.chapterSelected++;
   else if (isRepeatDue(&navigateRepeat, getPadButtonState(PAD_BTN_UP))) state.chapterSelected--;
   if (state.chapterSelected < 0) state.chapterSelected = 0;
   if (state.chapterSelected >= state.chapters.count) state.chapterSelected = state.chapters.count - 1;
}

static void updatePlay(void)
{
   // an overlay owns the pad for the whole frame it was (or just became) visible, so the button that
   // closed or opened it can't leak into playback input below (e.g. cross toggling pause on the way out)
   int overlayHadPad = state.descriptionVisible || state.chaptersVisible;
   int loadingPauseInput = 0;
   if (state.descriptionVisible) handleDescriptionInput();
   else if (state.chaptersVisible) handleChaptersInput();
   else {
      if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }   // back to the results list
      if (state.stage == STAGE_LOADING && isPadButtonPressed(PAD_BTN_CROSS)) {
         state.pauseWhenReady = !state.pauseWhenReady;
         loadingPauseInput = 1;
      }
      if (isPadButtonPressed(PAD_BTN_R3)) state.showStats = !state.showStats;
      if (isPadButtonPressed(PAD_BTN_SELECT) && state.stage == STAGE_PLAYING && state.player) openDescription();
      if (isPadButtonPressed(PAD_BTN_START) && state.stage == STAGE_PLAYING && state.player) openChapters();
   }

   // reap the worker once it's done; adopt the player it built
   if (state.workerDone && state.threadActive) {
      joinThread(state.workerTid);
      state.threadActive = 0;
      if (state.pendingPlayer) {
         state.player = state.pendingPlayer;
         state.pendingPlayer = NULL;
         state.message[0] = 0;
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
         if (state.pauseWhenReady) setVideoPaused(state.player, 1);
         setLabelText(&titleLabel, state.title);   // rasterise once, off the draw path
         state.stage = STAGE_PLAYING;
         rememberAutoplayVideo(requestedInput);
         if (state.audioPick >= 0 && state.audioPick < state.audioCount) { char notice[96]; snprintf(notice, sizeof notice, "Audio: %s", state.audioNames[state.audioPick]); showToast(notice); }
         else if (state.audioCount > 0) showToast("Default audio track unavailable; use L3 to choose a track");
      }
   }

   // Complete the audio-only handoff. The same VideoPlayer and H.264 decoder remain in place.
   if (state.audioSwitchDone && state.audioSwitchThreadActive) {
      joinThread(state.audioSwitchTid);
      state.audioSwitchThreadActive = 0;
      state.message[0] = 0;
      if (state.audioSwitchFallback) {
         state.audioPick = state.audioSwitchTarget;
         state.audItag = state.audioItags[state.audioPick];
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
         { char notice[96]; snprintf(notice, sizeof notice, "Audio: %s", state.audioNames[state.audioPick]); showToast(notice); }
      } else {
         showToast("Audio switch failed; video continues");
      }
      if (!state.audioSwitchWasPaused && state.player) setVideoPaused(state.player, 0);
      if (!state.audioSwitchFallback && !state.audioFallbackAttempted) {
         state.audioFallbackAttempted = 1;
         int lowerProfile = findLowerResolutionProfile();
         if (lowerProfile >= 0) startResolutionSwitch(lowerProfile, 1);
      }
   }

   // Commit a resolution change only after its replacement player has reached a decoded pre-roll.
   if (state.resolutionSwitchDone && state.resolutionSwitchThreadActive) {
      joinThread(state.resolutionSwitchTid);
      state.resolutionSwitchThreadActive = 0;
      if (state.pendingResolutionPlayer) {
         state.player = state.pendingResolutionPlayer;
         state.pendingResolutionPlayer = NULL;
         if (state.resolutionSwitchRestored) {
            state.resolutionIndex = state.resolutionSwitchOldIndex;
         } else {
            state.resolutionIndex = state.resolutionSwitchTarget;
            state.vidItag = state.videoItags[state.resolutionIndex];
            state.vidW = state.videoWidths[state.resolutionIndex];
            state.vidH = state.videoHeights[state.resolutionIndex];
            state.vidFps = state.videoFps[state.resolutionIndex];
            if (state.resolutionIndex == 2) state.audioFallbackAttempted = 0;
            if (state.audioPick >= 0 && state.audioPick < state.audioCount)
               state.audItag = state.audioItags[state.audioPick];
            else if (state.fallbackAudioItag)
               state.audItag = state.fallbackAudioItag;
         }
         setVideoPlayerAudioVolume(state.player, getVolumeMeterFraction(&volumeMeter));
         if (!state.resolutionSwitchWasPaused) setVideoPaused(state.player, 0);
         state.firstFrameSeen = 0;
         state.lastFrame = NULL;
         state.framesThisSecond = state.measuredFps = 0;
         state.message[0] = 0;
         if (state.resolutionSwitchRestored) {
            showToast("Resolution unavailable; previous stream restored");
         } else {
            char notice[64];
            if (state.vidFps > 0)
               snprintf(notice, sizeof notice, "Video: %dp @ %d FPS", state.vidH, state.vidFps);
            else
               snprintf(notice, sizeof notice, "Video: %dp", state.vidH);
            showToast(notice);
         }
      } else {
         showToast("Resolution switch and recovery failed");
      }
   }

   // A track can pass the initial AAC probe and then die during playback. Retry the current
   // audio track on the highest available lower video profile once; never loop retries.
   if (state.stage == STAGE_PLAYING && state.player &&
       (state.audioPick >= 0 || state.fallbackAudioUrl[0]) && !state.isLive &&
       !state.audioFallbackAttempted &&
       isVideoPlayerUsingSeparateAudio(state.player) &&
       !isVideoPlayerSeparateAudioAlive(state.player) && !isVideoEnded(state.player)) {
      float duration = getVideoDurationSeconds(state.player);
      float position = getVideoPositionSeconds(state.player);
      if (duration <= 0.0f || position < duration - 5.0f) {
         state.audioFallbackAttempted = 1;
         int lowerProfile = findLowerResolutionProfile();
         if (lowerProfile >= 0) startResolutionSwitch(lowerProfile, 1);
      }
   }

   if (state.nextWorkerDone && state.nextThreadActive) {
      joinThread(state.nextWorkerTid);
      state.nextThreadActive = 0;
      if (state.nextVideoId[0]) {
         if (!state.nextVideoTitle[0])
            snprintf(state.nextVideoTitle, sizeof state.nextVideoTitle, "Video %s", state.nextVideoId);
         setLabelText(&nextNoticeTitleLabel, state.nextVideoTitle);
         state.nextNoticeVisible = 1;
         state.nextNoticeStartedUs = sys_time_get_system_time();
         if (state.nextThumbJpeg && state.nextThumbJpegLen > 0) {
            if (requestImageMemAsync(state.nextThumbJpeg, (uint32_t)state.nextThumbJpegLen) == 0)
               state.nextImageRequested = 1;
            free(state.nextThumbJpeg);
            state.nextThumbJpeg = NULL;
            state.nextThumbJpegLen = 0;
         }
      } else {
         showToast("No next recommendation");
      }
   }

   if (state.nextImageRequested && !state.nextThumbTexture.offset) {
      ImageBuffer decoded;
      int imageResult = pollImageAsync(&decoded);
      if (imageResult == 1) {
         state.nextThumbTexture = uploadImageBuffer(&decoded);
         freeImageBuffer(&decoded);
         state.nextImageRequested = 0;
      } else if (imageResult < 0) {
         state.nextImageRequested = 0;
      }
   }

   if (state.nextNoticeVisible &&
       sys_time_get_system_time() - state.nextNoticeStartedUs >= AUTOPLAY_NOTICE_US) {
      char nextId[16];
      strCopy(nextId, sizeof nextId, state.nextVideoId);
      state.nextNoticeVisible = 0;
      if (state.nextThumbTexture.offset) retireGfxTexture(&state.nextThumbTexture);
      popScreen();
      launchVideo(nextId, 1);
      return;
   }

   // reap the sponsorblock fetch; its segments are safe to read once joined
   if (state.sbWorkerDone && state.sbThreadActive) {
      joinThread(state.sbWorkerTid);
      state.sbThreadActive = 0;
      state.sbReady = 1;
   }

   // Chapter fetching is on demand from openChapters(), avoiding an unnecessary API request at startup.
   if (state.chWorkerDone && state.chThreadActive) {
      joinThread(state.chWorkerTid);
      state.chThreadActive = 0;
      state.chapters = state.fetchedChapters;   // safe to adopt now the worker is reaped
   }

   // subtitles: reap a finished fetch (stale if the pick moved mid-fetch), then start one whenever
   // the pick and the fetched track disagree
   if (state.subWorkerDone && state.subThreadActive) {
      joinThread(state.subWorkerTid);
      state.subThreadActive = 0;
      state.subtitlesReady = (state.subtitleFetched == state.subtitlePick);
      if (!state.subtitlesReady) state.subtitleFetched = -1;   // pick moved mid-fetch: re-picking must refetch
      else if (state.subtitleTrack->count == 0) {   // fetch failed or empty track: drop to Off, visibly
         state.subtitlePick = state.subtitleFetched = -1;
         state.subtitlesReady = 0;
         setLabelText(&subsHudLabel, "Subtitles: Off");
         showToast("Subtitles unavailable");
      }
   }
   if (state.subtitlePick >= 0 && state.subtitlePick != state.subtitleFetched && !state.subThreadActive) {
      if (!state.subtitleTrack) state.subtitleTrack = malloc(sizeof *state.subtitleTrack);
      if (state.subtitleTrack) {
         state.subtitlesReady = 0;
         state.subtitleScanFrom = 0;
         state.subWorkerDone = 0;
         state.subtitleFetched = state.subtitlePick;
         state.subThreadActive = (spawnJoinableThread(&state.subWorkerTid, subtitleWorker, 0,
                                  THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-subs") == 0);
         if (!state.subThreadActive) state.subtitleFetched = -1;   // spawn failed: retry next frame
      }
   }

   // feed the cue covering the current position into the label; setLabelText only rasterises on change
   if (state.subtitlesReady && state.subtitlePick >= 0 && state.stage == STAGE_PLAYING && state.player)
      setLabelText(&subtitleLabel, getSubtitleCueText(state.subtitleTrack, getVideoPositionSeconds(state.player), &state.subtitleScanFrom));

   if (state.stage == STAGE_PLAYING && state.player && !overlayHadPad && !loadingPauseInput &&
       !state.descriptionVisible && !state.chaptersVisible)
      handlePlaybackInput();
   if (state.sbReady && state.stage == STAGE_PLAYING && state.player && !isVideoPaused(state.player) && !state.seeking)
      autoSkipSponsor();

   // Automatic playback: ask /next once after a VOD reaches its end. Network failure is non-fatal.
   if (state.stage == STAGE_PLAYING && state.player && !state.isLive &&
       isVideoEnded(state.player) && !state.nextAttempted && !state.nextThreadActive) {
      state.nextAttempted = 1;
      state.nextWorkerDone = 0;
      state.nextVideoId[0] = 0;
      state.nextThreadActive = (spawnJoinableThread(&state.nextWorkerTid, autoplayWorker, 0,
                                THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-autoplay") == 0);
      if (!state.nextThreadActive) showToast("Autoplay unavailable");
   }

   // status text: the failure reason, or "Loading..." until playback. setLabelText skips unchanged
   // text, so calling it each frame only rasterises on a real change.
   if (state.audioSwitchThreadActive) setLabelText(&statusLabel, state.message[0] ? state.message : "Loading audio...");
   else if (state.resolutionSwitchThreadActive) setLabelText(&statusLabel, "Changing resolution...");
   else if (state.stage == STAGE_FAILED) setLabelText(&statusLabel, state.message);
   else if (state.stage != STAGE_PLAYING) setLabelText(&statusLabel, state.message[0] ? state.message : "Loading...");
}

// measure the real presented frame rate: a changed frame pointer is a new frame; tally them per second.
static void measureFps(const uint8_t *frame)
{
   if (frame && frame != state.lastFrame) { state.framesThisSecond++; state.lastFrame = frame; }
   uint64_t now = sys_time_get_system_time();
   if (now - state.fpsTickUs >= 1000000) { state.measuredFps = state.framesThisSecond; state.framesThisSecond = 0; state.fpsTickUs = now; }
}

// rebuild the overlay lines. setLabelText skips unchanged text, so calling this per frame only
// rasterises when a value actually moves (fps + time once a second).
static void updateStatLabels(void)
{
   char line[160];
   int rate = 0, channels = 0;
   const char *audioName = NULL;
   getAudioTrackInfo(state.player, &rate, &channels);
   float pos = getVideoPositionSeconds(state.player), duration = getVideoDurationSeconds(state.player);

   if (state.audioPick >= 0 && state.audioPick < state.audioCount)
      audioName = state.audioNames[state.audioPick];

   snprintf(line, sizeof line, "Video  itag %d   %dx%d   H.264", state.vidItag, state.vidW, state.vidH);
   setLabelText(&statLabels[0], line);
   snprintf(line, sizeof line, "FPS  %d   (target %d)", state.measuredFps, state.vidFps);
   setLabelText(&statLabels[1], line);
   if (state.audItag) {
      if (audioName && audioName[0])
         snprintf(line, sizeof line, "Audio  itag %d   %s   AAC %d Hz   %d ch",
                  state.audItag, audioName, rate, channels);
      else
         snprintf(line, sizeof line, "Audio  itag %d   AAC %d Hz   %d ch",
                  state.audItag, rate, channels);
   } else {
      strCopy(line, sizeof line, "Audio  none");
   }
   setLabelText(&statLabels[2], line);
   snprintf(line, sizeof line, "Time  %d:%02d / %d:%02d", (int)pos / 60, (int)pos % 60, (int)duration / 60, (int)duration % 60);
   setLabelText(&statLabels[3], line);
}

static void drawStatsOverlay(void)
{
   int x = 40, y = 44, lineHeight = 30;
   fillGfxRectangle(x - 14, y - 12, 500, STAT_LINES * lineHeight + 14, activeTheme->badgeFill);
   for (int i = 0; i < STAT_LINES; i++) drawLabelAt(&statLabels[i], x, y + i * lineHeight);
}

// seek bar + time show while scrubbing / paused / ended, else auto-hide after the last activity
static int controlsVisible(void)
{
   if (isVideoPaused(state.player) || isVideoEnded(state.player) || state.seeking) return 1;
   return state.controlsShownUs != 0 && sys_time_get_system_time() - state.controlsShownUs < CONTROLS_VISIBLE_US;
}

// paint each sponsor segment onto the bar in its category colour, over the progress fill so it stays
// visible in the already-watched region (as YouTube's SponsorBlock overlay does).
static void drawSponsorSegments(int barLeft, int span, int barTopY, int barH, float duration)
{
   if (duration <= 0.0f) return;
   for (int i = 0; i < state.sb.count; i++) {
      const SponsorSegment *segment = &state.sb.segments[i];
      float startFrac = segment->start / duration, endFrac = segment->end / duration;
      if (startFrac < 0.0f) startFrac = 0.0f;
      if (endFrac > 1.0f) endFrac = 1.0f;
      int segX = barLeft + (int)(startFrac * span);
      int segW = (int)((endFrac - startFrac) * span);
      if (segW < 2) segW = 2;
      fillGfxRectangle(segX, barTopY, segW, barH, getSponsorCategoryColor(segment->category));
   }
}

// a red YouTube-style progress bar low on the screen with current/total time on each side, drawn from
// plain rectangles (no spritesheet), with sponsor segments marked over the fill.
static void drawSeekBar(void)
{
   float duration = getVideoDurationSeconds(state.player);
   float position = state.seeking ? state.seekTarget : getVideoPositionSeconds(state.player);
   float progress = duration > 0.0f ? position / duration : 0.0f;
   if (progress < 0.0f) progress = 0.0f;
   if (progress > 1.0f) progress = 1.0f;

   int barLeft  = (int)(state.screenW * 0.24f);
   int barRight = state.screenW - barLeft;
   int span     = barRight - barLeft;
   int barY     = (int)(state.screenH * 0.94f);
   int barH     = 8;
   int barTopY  = barY - barH / 2;
   int filledW  = (int)(progress * span);

   fillGfxRectangle(barLeft, barTopY, span, barH, activeTheme->seekTrack);
   fillGfxRectangle(barLeft, barTopY, filledW, barH, activeTheme->accent);   // progress (youtube red)
   if (state.sbReady) drawSponsorSegments(barLeft, span, barTopY, barH, duration);
   if (duration > 0.0f)   // chapter boundaries notch the bar, youtube-style
      for (int i = 1; i < state.chapters.count; i++)
         fillGfxRectangle(barLeft + (int)(state.chapters.chapters[i].start / duration * span), barTopY, 3, barH, activeTheme->seekNotch);
   fillGfxRectangle(barLeft + filledW - 3, barY - 11, 6, 22, activeTheme->textPrimary);   // scrubber handle

   char text[16];
   formatTime(text, sizeof text, (int)position);            setLabelText(&timeLeftLabel, text);
   formatTime(text, sizeof text, (int)(duration + 0.5f));   setLabelText(&timeRightLabel, text);
   drawLabelAt(&timeLeftLabel,  barLeft - 20 - timeLeftLabel.tt.tex.w, barY - timeLeftLabel.tt.tex.h / 2);
   drawLabelAt(&timeRightLabel, barRight + 20, barY - timeRightLabel.tt.tex.h / 2);
}

// a top-of-screen HUD badge in the thumbnails' duration-tag style: label on a translucent black box,
// with breathing room either side so the text never touches the box edges.
#define BADGE_PAD_X 14

static void drawBadge(Label *label, int x, int y)
{
   fillGfxRectangle(x, y, label->tt.tex.w + 2 * BADGE_PAD_X, label->tt.tex.h + 8, activeTheme->badgeFill);
   drawLabelAt(label, x + BADGE_PAD_X, y + 4);
}

// title top-left + chosen subtitle language top-right, shown whenever the seek bar is up.
static void drawTitle(void)
{
   if (titleLabel.tt.tex.w > 0) drawBadge(&titleLabel, 40, 32);
}

static void drawSubsHud(void)
{
   if (subsHudLabel.tt.tex.w > 0) drawBadge(&subsHudLabel, state.screenW - 40 - (subsHudLabel.tt.tex.w + 2 * BADGE_PAD_X), 32);
}

// the active subtitle cue, centred above the seek bar on a dim backdrop.
static void drawSubtitle(void)
{
   if (state.subtitlePick < 0 || subtitleLabel.tt.tex.w == 0) return;
   int x = state.screenW / 2 - subtitleLabel.tt.tex.w / 2;
   int y = (int)(state.screenH * 0.94f) - 40 - subtitleLabel.tt.tex.h;
   fillGfxRectangle(x - 12, y - 6, subtitleLabel.tt.tex.w + 24, subtitleLabel.tt.tex.h + 12, activeTheme->badgeFill);
   drawLabelAt(&subtitleLabel, x, y);
}

// the description over the whole video: a semi-transparent black layer, then the slice of the text
// texture the scroll position selects (the texture is drawn through a window, so no clipping is needed).
static void drawDescription(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);
   if (descriptionTexture.tex.w == 0) return;

   int viewHeight = state.screenH - 2 * DESCRIPTION_MARGIN_Y;
   int visibleHeight = descriptionTexture.tex.h - state.descriptionScroll;
   if (visibleHeight > viewHeight) visibleHeight = viewHeight;
   if (visibleHeight <= 0) return;

   float sliceTop    = (float)state.descriptionScroll / descriptionTexture.tex.h;
   float sliceBottom = (float)(state.descriptionScroll + visibleHeight) / descriptionTexture.tex.h;
   drawGfxTexture(DESCRIPTION_MARGIN_X, DESCRIPTION_MARGIN_Y, descriptionTexture.tex.w, visibleHeight,
                  descriptionTexture.tex, 0.0f, sliceTop, 1.0f, sliceBottom, 0xFFFFFFFF, GFX_FILTER_NEAREST);
}

// the chapter picker: a dim layer, then a centred column of rows windowed around the selection;
// the selected row gets a subtle highlight with a youtube-red accent on its left edge.
static void drawChapters(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, activeTheme->scrim);

   // window the rows around the selection so long lists stay reachable without a scrollbar
   int rows = state.chapters.count < CHAPTER_VISIBLE_ROWS ? state.chapters.count : CHAPTER_VISIBLE_ROWS;
   int firstRow = state.chapterSelected - rows / 2;
   if (firstRow > state.chapters.count - rows) firstRow = state.chapters.count - rows;
   if (firstRow < 0) firstRow = 0;

   int panelX = state.screenW / 2 - state.chapterPanelWidth / 2;
   int titleX = panelX + state.chapterTimeWidth + CHAPTER_TIME_GAP;
   int topY   = state.screenH / 2 - rows * CHAPTER_ROW_HEIGHT / 2;

   for (int row = 0; row < rows; row++) {
      int index = firstRow + row;
      int rowY  = topY + row * CHAPTER_ROW_HEIGHT;
      if (index == state.chapterSelected) {
         fillGfxRectangle(panelX - 19, rowY, state.chapterPanelWidth + 43, CHAPTER_ROW_HEIGHT, activeTheme->rowHighlight);
         fillGfxRectangle(panelX - 24, rowY, 5, CHAPTER_ROW_HEIGHT, activeTheme->accent);   // red accent, flush with the highlight
      }
      drawLabelAt(&chapterTimeLabels[index], panelX + state.chapterTimeWidth - chapterTimeLabels[index].tt.tex.w,
                  getCenteredLabelY(&chapterTimeLabels[index], rowY, CHAPTER_ROW_HEIGHT));
      drawLabelAt(&chapterLabels[index], titleX, getCenteredLabelY(&chapterLabels[index], rowY, CHAPTER_ROW_HEIGHT));
   }
}

// a brief toast in the top-right notification spot, one badge row below the subtitle-language badge.
static void drawToast(void)
{
   if (state.toastShownUs == 0 || sys_time_get_system_time() - state.toastShownUs >= TOAST_VISIBLE_US) return;
   drawBadge(&toastLabel, state.screenW - 40 - (toastLabel.tt.tex.w + 2 * BADGE_PAD_X), 80);
}

// Fill the display while preserving the source aspect ratio. Any excess image area is
// positioned outside the screen and clipped by the RSX viewport (a centred crop).
static void getCropToFillRect(int frameW, int frameH, int *dx, int *dy, int *dw, int *dh)
{
   float frameAspect = (float)frameW / (float)frameH;
   float screenAspect = (float)state.screenW / (float)state.screenH;

   if (frameAspect > screenAspect) {
      *dh = state.screenH;
      *dw = (int)(state.screenH * frameAspect + 0.5f);
      *dx = (state.screenW - *dw) / 2;
      *dy = 0;
   } else {
      *dw = state.screenW;
      *dh = (int)(state.screenW / frameAspect + 0.5f);
      *dx = 0;
      *dy = (state.screenH - *dh) / 2;
   }
}

static void drawAutoplayNotice(void)
{
   uint64_t now = sys_time_get_system_time();
   float elapsed = (float)(now - state.nextNoticeStartedUs) / (float)AUTOPLAY_NOTICE_US;
   if (elapsed < 0.0f) elapsed = 0.0f;
   if (elapsed > 1.0f) elapsed = 1.0f;

   fillGfxRectangle(0, 0, state.screenW, state.screenH, 0xB8000000);
   int panelW = 1120, panelH = 610;
   int panelX = (state.screenW - panelW) / 2;
   int panelY = (state.screenH - panelH) / 2;
   fillGfxRectangle(panelX, panelY, panelW, panelH, activeTheme->surface);
   strokeGfxRectangle(panelX, panelY, panelW, panelH, 2, activeTheme->focusBorder);

   // The thumbnail is centred and kept at 16:9. A placeholder remains if download/decode fails.
   int headingX = state.screenW / 2 - nextNoticeHeadingLabel.tt.tex.w / 2;
   drawLabelAt(&nextNoticeHeadingLabel, headingX, panelY + 8);
   int thumbW = 800, thumbH = 450;
   int thumbX = state.screenW / 2 - thumbW / 2;
   int thumbY = panelY + 42;
   if (state.nextThumbTexture.offset) {
      drawGfxTextureFixedSize(thumbX, thumbY, thumbW, thumbH, state.nextThumbTexture,
                              0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, GFX_FILTER_LINEAR);
   } else {
      fillGfxRectangle(thumbX, thumbY, thumbW, thumbH, activeTheme->scrim);
   }

   int titleX = state.screenW / 2 - nextNoticeTitleLabel.tt.tex.w / 2;
   int titleY = thumbY + thumbH + 18;
   if (titleX < panelX + 30) titleX = panelX + 30;
   drawLabelAt(&nextNoticeTitleLabel, titleX, titleY);

   // 24 dots form a countdown ring; the lit arc grows as the three-second delay elapses.
   int cx = state.screenW / 2, cy = panelY + panelH - 32;
   int radius = 20;
   int lit = (int)(elapsed * 24.0f + 0.5f);
   for (int i = 0; i < 24; i++) {
      float angle = -1.5707963f + (float)i * 6.2831853f / 24.0f;
      int x = cx + (int)(cosf(angle) * radius);
      int y = cy + (int)(sinf(angle) * radius);
      fillGfxCircle(x, y, 4, i < lit ? activeTheme->accent : activeTheme->textSecondary);
   }
}

static void drawPlay(void)
{
   // The SD UI zoom is intended for library/navigation screens only. Keep all playback
   // controls and overlays at their original 1.0x geometry so the seek bar stays on-screen.
   float selectedUiScale = getGfxSdUiScale();
   setGfxSdUiScale(1.0f);

   fillGfxRectangle(0, 0, state.screenW, state.screenH, COLOR_BLACK);   // letterbox bars read as black, not the app background

   const uint8_t *frame = NULL;
   if (state.player) {
      int w, h;
      frame = getVideoFrame(state.player, &w, &h);
      if (frame) {
         if (!state.firstFrameSeen) showControls();   // bring up the HUD as playback starts; it auto-hides as usual
         state.firstFrameSeen = 1;
         int dx, dy, dw, dh;
         if (cropToFillEnabled) getCropToFillRect(w, h, &dx, &dy, &dw, &dh);
         else                   getGfxLetterboxRect(w, h, &dx, &dy, &dw, &dh);
         drawGfxYuvFrame(dx, dy, dw, dh, frame, w, h);
      }
      measureFps(frame);
      if (state.showStats && frame) { updateStatLabels(); drawStatsOverlay(); }
      int overlayUp = state.chaptersVisible || state.descriptionVisible;   // a full-screen overlay hides the HUD noise
      if (state.stage == STAGE_PLAYING && !state.isLive && controlsVisible() && !overlayUp)   // live has no timeline
         { drawSeekBar(); drawTitle(); drawSubsHud(); }
      if (state.stage == STAGE_PLAYING && !overlayUp) { drawSubtitle(); drawToast(); drawVolumeMeter(&volumeMeter); }
      if (state.chaptersVisible) drawChapters();
      if (state.descriptionVisible) drawDescription();
      if (state.nextNoticeVisible) drawAutoplayNotice();
   }

   // until the first frame is presented (open, then the async resume seek's reload) keep the loading
   // status up instead of a black screen; with no player this shows the failure message.
   if (!state.firstFrameSeen || state.audioSwitchThreadActive || state.resolutionSwitchThreadActive)
      drawLabelAt(&statusLabel, state.screenW / 2 - statusLabel.tt.tex.w / 2, state.screenH / 2 - 14);

   // Restore the selected library scale before the next screen is drawn.
   setGfxSdUiScale(selectedUiScale);
}

static void termPlay(void)
{
   // Wake worker threads blocked in TLS connect/read before joining them. Without this, leaving during
   // a slow YouTube resolve can make the screen manager wait through several 10-second socket timeouts.
   setHttpRequestsCancelled(1);
   state.resolutionSwitchAborting = 1;
   if (state.threadActive) { joinThread(state.workerTid); state.threadActive = 0; }
   if (state.sbThreadActive) { joinThread(state.sbWorkerTid); state.sbThreadActive = 0; }   // usually already done
   if (state.subThreadActive) { joinThread(state.subWorkerTid); state.subThreadActive = 0; }
   if (state.audioSwitchThreadActive) { joinThread(state.audioSwitchTid); state.audioSwitchThreadActive = 0; }
   if (state.resolutionSwitchThreadActive) { joinThread(state.resolutionSwitchTid); state.resolutionSwitchThreadActive = 0; }
   if (state.chThreadActive) { joinThread(state.chWorkerTid); state.chThreadActive = 0; }
   if (state.nextThreadActive) { joinThread(state.nextWorkerTid); state.nextThreadActive = 0; }
   free(state.subtitleTrack);
   state.subtitleTrack = NULL;
   // save the resume position on the way out; a finished video is saved as 0 so it restarts next time.
   // only once a frame has actually played: a failed open or a resume seek that never landed must not
   // clobber a good saved position with 0. storage ignores non-id keys, so typed urls no-op.
   if (state.player && state.firstFrameSeen && !state.isLive)
      setWatchedPosition(requestedInput, isVideoEnded(state.player) ? 0 : (int)getVideoPositionSeconds(state.player));
   if (state.player || state.pendingPlayer || state.pendingResolutionPlayer || state.retiringResolutionPlayer)
      finishGfx();   // RSX may still be sampling a frame buffer
   if (state.player) { destroyVideoPlayer(state.player); state.player = NULL; }
   if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
   if (state.pendingResolutionPlayer) { destroyVideoPlayer(state.pendingResolutionPlayer); state.pendingResolutionPlayer = NULL; }
   if (state.retiringResolutionPlayer) { destroyVideoPlayer(state.retiringResolutionPlayer); state.retiringResolutionPlayer = NULL; }
   freeLabel(&statusLabel);
   for (int i = 0; i < STAT_LINES; i++) freeLabel(&statLabels[i]);
   freeLabel(&timeLeftLabel);
   freeLabel(&timeRightLabel);
   freeLabel(&toastLabel);
   freeLabel(&titleLabel);
   freeLabel(&nextNoticeHeadingLabel);
   freeLabel(&nextNoticeTitleLabel);
   freeLabel(&subtitleLabel);
   freeLabel(&subsHudLabel);
   for (int i = 0; i < MAX_CHAPTERS; i++) { freeLabel(&chapterLabels[i]); freeLabel(&chapterTimeLabels[i]); }
   freeVolumeMeter(&volumeMeter);
   freeTextTexture(&descriptionTexture);
   if (state.nextThumbJpeg) { free(state.nextThumbJpeg); state.nextThumbJpeg = NULL; }
   if (state.nextImageRequested) cancelImageAsync();
   if (state.nextThumbTexture.offset) { finishGfx(); freeGfxTexture(&state.nextThumbTexture); }
   closeFont(&uiFont);
   setHttpRequestsCancelled(0);
}

Screen playScreen = { initPlay, NULL, updatePlay, drawPlay, NULL, termPlay, SCREEN_TERMINATED };
