// stream-select - format selection shared by playback and download (see stream-select.h).

#include "stream-select.h"
#include "dbg.h"
#include <string.h>

// what the PS3 H.264 decoder can keep up with: 1080p at <=30 fps, or 720p at <=60 fps (fewer pixels/sec
// than 1080p30). 1080p60 (itag 298/299) exceeds it and is skipped, falling to 720p60.
static int decodableVideo(const StreamFormat *format)
{
   if (format->height <= 0 || format->fps <= 0 || format->fps > 60) return 0;
   if (format->height <= 720) return 1;
   return format->height <= 1080 && format->fps <= 30;
}

const StreamFormat *pickBestVideo(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;   // mp4 == avc1 for these itags
      if (!decodableVideo(format)) continue;
      if (!best || format->height > best->height || (format->height == best->height && format->fps > best->fps))
         best = format;
   }

   // Only dump candidates when selection fails. This keeps normal logs unchanged and
   // bounds one diagnostic burst to MAX_STREAM_FORMATS short lines.
   if (!best) {
      logError("[yt] VIDEO SELECT FAILED: formats=%d\n", info->formatCount);
      for (int i = 0; i < info->formatCount; i++) {
         const StreamFormat *f = &info->formats[i];
         const char *reason = "not-video";
         if (f->hasVideo) {
            if (f->needsCipher) reason = "cipher";
            else if (!f->url[0]) reason = "no-url";
            else if (strcmp(f->container, "mp4") != 0) reason = "container";
            else if (!decodableVideo(f)) reason = "resolution-fps";
            else reason = "eligible-check-selector";
         } else if (f->hasAudio) {
            reason = f->needsCipher ? "audio-cipher" : (!f->url[0] ? "audio-no-url" : "audio-only");
         }
         logError("[yt] fmt[%d] itag=%d v=%d a=%d ct=%s cipher=%d url=%d size=%dx%d fps=%d reason=%s track=%s\n",
                  i, f->itag, f->hasVideo, f->hasAudio, f->container,
                  f->needsCipher, f->url[0] != 0, f->width, f->height, f->fps,
                  reason, f->audioTrackId[0] ? f->audioTrackId : "-");
      }
   } else {
      logInfo("[yt] video selector chose itag=%d %dx%d fps=%d\n", best->itag, best->width, best->height, best->fps);
   }
   return best;
}

const StreamFormat *pickVideoForProfile(const StreamInfo *info, int profile)
{
   if (!info || profile < 0 || profile > 2) return NULL;

   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;
      if (format->height <= 0 || format->fps <= 0 || format->fps > 60) continue;

      if (profile == 0 && format->height > 480) continue;
      if (profile == 1 && (format->height <= 480 || format->height > 720)) continue;
      if (profile == 2 && (format->height <= 720 || format->height > 1080 || format->fps > 30)) continue;

      if (!best || format->height > best->height ||
          (format->height == best->height && format->fps > best->fps))
         best = format;
   }
   return best;
}

const StreamFormat *pickBestAudio(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasAudio || format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;
      if (!best || format->itag == 140) best = format;
   }
   return best;
}
