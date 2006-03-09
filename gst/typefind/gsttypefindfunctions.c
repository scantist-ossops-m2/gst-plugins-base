/* GStreamer
 * Copyright (C) 2003 Benjamion Otte <in7y118@public.uni-hamburg.de>
 *
 * gsttypefindfunctions.c: collection of various typefind functions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib/gstrfuncs.h>

#include <gst/gsttypefind.h>
#include <gst/gstelement.h>
#include <gst/gstversion.h>
#include <gst/gstinfo.h>
#include <gst/gstutils.h>

#include <string.h>
#include <ctype.h>

GST_DEBUG_CATEGORY_STATIC (type_find_debug);
#define GST_CAT_DEFAULT type_find_debug

/*** text/plain ***/
static gboolean xml_check_first_element (GstTypeFind * tf,
    const gchar * element, guint elen);


static GstStaticCaps utf8_caps = GST_STATIC_CAPS ("text/plain");

#define UTF8_CAPS gst_static_caps_get(&utf8_caps)

static gboolean
utf8_type_find_have_valid_utf8_at_offset (GstTypeFind * tf, guint64 offset,
    GstTypeFindProbability * prob)
{
  guint8 *data;

  /* randomly decided values */
  guint min_size = 16;          /* minimum size  */
  guint size = 32 * 1024;       /* starting size */
  guint probability = 95;       /* starting probability */
  guint step = 10;              /* how much we reduce probability in each
                                 * iteration */

  while (probability > step && size > min_size) {
    data = gst_type_find_peek (tf, offset, size);
    if (data) {
      gchar *end;
      gchar *start = (gchar *) data;

      if (g_utf8_validate (start, size, (const gchar **) &end) || (end - start + 4 > size)) {   /* allow last char to be cut off */
        *prob = probability;
        return TRUE;
      }
      *prob = 0;
      return FALSE;
    }
    size /= 2;
    probability -= step;
  }
  *prob = 0;
  return FALSE;
}

static void
utf8_type_find (GstTypeFind * tf, gpointer unused)
{
  GstTypeFindProbability start_prob, mid_prob;
  guint64 length;

  /* leave xml to the xml typefinders */
  if (xml_check_first_element (tf, "", 0))
    return;

  /* check beginning of stream */
  if (!utf8_type_find_have_valid_utf8_at_offset (tf, 0, &start_prob))
    return;

  GST_LOG ("start is plain text with probability of %u", start_prob);

  /* POSSIBLE is the highest probability we ever return if we can't
   * probe into the middle of the file and don't know its length */

  length = gst_type_find_get_length (tf);
  if (length == 0 || length == (guint64) - 1) {
    gst_type_find_suggest (tf, MIN (start_prob, GST_TYPE_FIND_POSSIBLE),
        UTF8_CAPS);
    return;
  }

  if (length < 64 * 1024) {
    gst_type_find_suggest (tf, start_prob, UTF8_CAPS);
    return;
  }

  /* check middle of stream */
  if (!utf8_type_find_have_valid_utf8_at_offset (tf, length / 2, &mid_prob))
    return;

  GST_LOG ("middle is plain text with probability of %u", mid_prob);
  gst_type_find_suggest (tf, (start_prob + mid_prob) / 2, UTF8_CAPS);
}

/*** text/uri-list ***/

static GstStaticCaps uri_caps = GST_STATIC_CAPS ("text/uri-list");

#define URI_CAPS (gst_static_caps_get(&uri_caps))
#define BUFFER_SIZE 16          /* If the string is < 16 bytes we're screwed */
#define INC_BUFFER {                                                    \
  pos++;                                                                \
  if (pos == BUFFER_SIZE) {                                             \
    pos = 0;                                                            \
    offset += BUFFER_SIZE;                                              \
    data = gst_type_find_peek (tf, offset, BUFFER_SIZE);                \
    if (data == NULL) return;                                           \
  } else {                                                              \
    data++;                                                             \
  }                                                                     \
}
static void
uri_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, BUFFER_SIZE);
  guint pos = 0;
  guint offset = 0;

  if (data) {
    /* Search for # comment lines */
    while (*data == '#') {
      /* Goto end of line */
      while (*data != '\n') {
        INC_BUFFER;
      }

      INC_BUFFER;
    }

    if (!g_ascii_isalpha (*data)) {
      /* Had a non alpha char - can't be uri-list */
      return;
    }

    INC_BUFFER;

    while (g_ascii_isalnum (*data)) {
      INC_BUFFER;
    }

    if (*data != ':') {
      /* First non alpha char is not a : */
      return;
    }

    /* Get the next 2 bytes as well */
    data = gst_type_find_peek (tf, offset + pos, 3);
    if (data == NULL)
      return;

    if (data[1] != '/' && data[2] != '/') {
      return;
    }

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, URI_CAPS);
  }
}


/*** application/xml **********************************************************/

#define XML_BUFFER_SIZE 256
#define XML_INC_BUFFER {                                                \
  pos++;                                                                \
  if (pos == XML_BUFFER_SIZE) {                                         \
    pos = 0;                                                            \
    offset += XML_BUFFER_SIZE;                                          \
    data = gst_type_find_peek (tf, offset, XML_BUFFER_SIZE);            \
    if (data == NULL) return FALSE;                                     \
  } else {                                                              \
    data++;                                                             \
  }                                                                     \
}

static gboolean
xml_check_first_element (GstTypeFind * tf, const gchar * element, guint elen)
{
  guint8 *data = gst_type_find_peek (tf, 0, XML_BUFFER_SIZE);
  guint offset = 0;
  guint pos = 0;

  /* look for the XMLDec,
   * see XML spec 2.8, Prolog and Document Type Declaration
   * http://www.w3.org/TR/2004/REC-xml-20040204/#sec-prolog-dtd */
  if (!data || memcmp (data, "<?xml", 5) != 0)
    return FALSE;

  pos += 5;
  data += 5;

  /* look for the first element, it has to be a <smil> element */
  while (data) {
    while (*data != '<') {
      XML_INC_BUFFER;
    }

    XML_INC_BUFFER;
    if (!g_ascii_isalpha (*data)) {
      /* if not alphabetic, it's a PI or an element / attribute declaration
       * like <?xxx or <!xxx */
      XML_INC_BUFFER;
      continue;
    }

    /* the first normal element, check if it's the one asked for */
    data = gst_type_find_peek (tf, offset + pos, elen + 1);
    return (data && element && strncmp ((char *) data, element, elen) == 0);
  }

  return FALSE;
}

static GstStaticCaps generic_xml_caps = GST_STATIC_CAPS ("application/xml");

#define GENERIC_XML_CAPS (gst_static_caps_get(&generic_xml_caps))
static void
xml_type_find (GstTypeFind * tf, gpointer unused)
{
  if (xml_check_first_element (tf, "", 0)) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MINIMUM, GENERIC_XML_CAPS);
  }
}

/*** application/smil *********************************************************/

static GstStaticCaps smil_caps = GST_STATIC_CAPS ("application/smil");

#define SMIL_CAPS (gst_static_caps_get(&smil_caps))
static void
smil_type_find (GstTypeFind * tf, gpointer unused)
{
  if (xml_check_first_element (tf, "smil", 4)) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SMIL_CAPS);
  }
}

/*** video/x-fli ***/

static GstStaticCaps flx_caps = GST_STATIC_CAPS ("video/x-fli");

#define FLX_CAPS gst_static_caps_get(&flx_caps)
static void
flx_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 134);

  if (data) {
    /* check magic and the frame type of the first frame */
    if ((data[4] == 0x11 || data[4] == 0x12 ||
            data[4] == 0x30 || data[4] == 0x44) &&
        data[5] == 0xaf &&
        ((data[132] == 0x00 || data[132] == 0xfa) && data[133] == 0xf1)) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, FLX_CAPS);
    }
    return;
  }
  data = gst_type_find_peek (tf, 0, 6);
  if (data) {
    /* check magic only */
    if ((data[4] == 0x11 || data[4] == 0x12 ||
            data[4] == 0x30 || data[4] == 0x44) && data[5] == 0xaf) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, FLX_CAPS);
    }
    return;
  }
}

/*** application/x-id3 ***/

static GstStaticCaps id3_caps = GST_STATIC_CAPS ("application/x-id3");

#define ID3_CAPS gst_static_caps_get(&id3_caps)
static void
id3_type_find (GstTypeFind * tf, gpointer unused)
{
  /* detect ID3v2 first */
  guint8 *data = gst_type_find_peek (tf, 0, 10);

  if (data) {
    /* detect valid header */
    if (memcmp (data, "ID3", 3) == 0 &&
        data[3] != 0xFF && data[4] != 0xFF &&
        (data[6] & 0x80) == 0 && (data[7] & 0x80) == 0 &&
        (data[8] & 0x80) == 0 && (data[9] & 0x80) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, ID3_CAPS);
      return;
    }
  }
  data = gst_type_find_peek (tf, -128, 3);
  if (data && memcmp (data, "TAG", 3) == 0) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 3, ID3_CAPS);
  }
}

/*** application/x-ape ***/

static GstStaticCaps apetag_caps = GST_STATIC_CAPS ("application/x-apetag");

#define APETAG_CAPS gst_static_caps_get(&apetag_caps)
static void
apetag_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data;

  /* APEv1/2 at start of file */
  data = gst_type_find_peek (tf, 0, 8);
  if (data && !memcmp (data, "APETAGEX", 8)) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 1, APETAG_CAPS);
    return;
  }

  /* APEv1/2 at end of file */
  data = gst_type_find_peek (tf, -32, 8);
  if (data && !memcmp (data, "APETAGEX", 8)) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 2, APETAG_CAPS);
    return;
  }
}

/*** audio/x-ttafile ***/

static GstStaticCaps tta_caps = GST_STATIC_CAPS ("audio/x-ttafile");

#define TTA_CAPS gst_static_caps_get(&tta_caps)
static void
tta_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 3);

  if (data) {
    if (memcmp (data, "TTA", 3) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, TTA_CAPS);
      return;
    }
  }
}

/*** audio/mpeg version 2, 4 ***/

static GstStaticCaps aac_caps = GST_STATIC_CAPS ("audio/mpeg, "
    "mpegversion = (int) { 2, 4 }, framed = (bool) false");
#define AAC_CAPS (gst_static_caps_get(&aac_caps))
#define AAC_AMOUNT (4096)
static void
aac_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, AAC_AMOUNT);
  gint snc;

  /* detect adts header or adif header.
   * The ADIF header is 4 bytes, that should be OK. The ADTS header, on
   * the other hand, is 14 bits only, so we require one valid frame with
   * again a valid syncpoint on the next one (28 bits) for certainty. We
   * require 4 kB, which is quite a lot, since frames are generally 200-400
   * bytes.
   */
  if (data) {
    gint n;

    for (n = 0; n < AAC_AMOUNT - 3; n++) {
      snc = GST_READ_UINT16_BE (&data[n]);
      if ((snc & 0xfff6) == 0xfff0) {
        /* ADTS header - find frame length */
        gint len;

        GST_DEBUG ("Found one ADTS syncpoint at offset 0x%x, tracing next...",
            n);
        if (AAC_AMOUNT - n < 5) {
          GST_DEBUG ("Not enough data to parse ADTS header");
          break;
        }
        len = ((data[n + 3] & 0x03) << 11) |
            (data[n + 4] << 3) | ((data[n + 5] & 0xe0) >> 5);
        if (n + len + 2 >= AAC_AMOUNT) {
          GST_DEBUG ("Next frame is not within reach");
          break;
        } else if (len == 0) {
          continue;
        }

        snc = GST_READ_UINT16_BE (&data[n + len]);
        if ((snc & 0xfff6) == 0xfff0) {
          gint mpegversion = (data[n + 1] & 0x08) ? 2 : 4;
          GstCaps *caps = gst_caps_new_simple ("audio/mpeg",
              "framed", G_TYPE_BOOLEAN, FALSE,
              "mpegversion", G_TYPE_INT, mpegversion,
              NULL);

          gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, caps);
          gst_caps_unref (caps);

          GST_DEBUG ("Found ADTS-%d syncpoint at offset 0x%x (framelen %u)",
              mpegversion, n, len);
          break;
        }

        GST_DEBUG ("No next frame found... (should be at 0x%x)", n + len);
      } else if (!memcmp (&data[n], "ADIF", 4)) {
        /* ADIF header */
        GstCaps *caps = gst_caps_new_simple ("audio/mpeg",
            "framed", G_TYPE_BOOLEAN, FALSE,
            "mpegversion", G_TYPE_INT, 4,
            NULL);

        gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, caps);
        gst_caps_unref (caps);
      }
    }
  }
}

/*** audio/mpeg version 1 ***/

/*
 * The chance that random data is identified as a valid mp3 header is 63 / 2^18
 * (0.024%) per try. This makes the function for calculating false positives
 *   1 - (1 - ((63 / 2 ^18) ^ GST_MP3_TYPEFIND_MIN_HEADERS)) ^ buffersize)
 * This has the following probabilities of false positives:
 * datasize               MIN_HEADERS
 * (bytes)      1       2       3       4
 * 4096         62.6%    0.02%   0%      0%
 * 16384        98%      0.09%   0%      0%
 * 1 MiB       100%      5.88%   0%      0%
 * 1 GiB       100%    100%      1.44%   0%
 * 1 TiB       100%    100%    100%      0.35%
 * This means that the current choice (3 headers by most of the time 4096 byte
 * buffers is pretty safe for now.
 *
 * The max. size of each frame is 1440 bytes, which means that for N frames to
 * be detected, we need 1440 * GST_MP3_TYPEFIND_MIN_HEADERS + 3 bytes of data.
 * Assuming we step into the stream right after the frame header, this
 * means we need 1440 * (GST_MP3_TYPEFIND_MIN_HEADERS + 1) - 1 + 3 bytes
 * of data (5762) to always detect any mp3.
 */

static guint mp3types_bitrates[2][3][16] =
    { {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448,},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,},
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,}},
{{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256,},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,}},
};

static guint mp3types_freqs[3][3] = { {11025, 12000, 8000},
{22050, 24000, 16000},
{44100, 48000, 32000}
};

static inline guint
mp3_type_frame_length_from_header (guint32 header, guint * put_layer,
    guint * put_channels, guint * put_bitrate, guint * put_samplerate,
    gboolean * may_be_free_format, gint possible_free_framelen)
{
  guint bitrate, layer, length, mode, samplerate, version, channels;

  if ((header & 0xffe00000) != 0xffe00000)
    return 0;

  /* we don't need extension, copyright, original or
   * emphasis for the frame length */
  header >>= 6;

  /* mode */
  mode = header & 0x3;
  header >>= 3;

  /* padding */
  length = header & 0x1;
  header >>= 1;

  /* sampling frequency */
  samplerate = header & 0x3;
  if (samplerate == 3)
    return 0;
  header >>= 2;

  /* bitrate index */
  bitrate = header & 0xF;
  if (bitrate == 0 && possible_free_framelen == -1) {
    GST_LOG ("Possibly a free format mp3 - signalling");
    *may_be_free_format = TRUE;
  }
  if (bitrate == 15 || (bitrate == 0 && possible_free_framelen == -1))
    return 0;

  /* ignore error correction, too */
  header >>= 5;

  /* layer */
  layer = 4 - (header & 0x3);
  if (layer == 4)
    return 0;
  header >>= 2;

  /* version 0=MPEG2.5; 2=MPEG2; 3=MPEG1 */
  version = header & 0x3;
  if (version == 1)
    return 0;

  /* lookup */
  channels = (mode == 3) ? 1 : 2;
  samplerate = mp3types_freqs[version > 0 ? version - 1 : 0][samplerate];
  if (bitrate == 0) {
    if (layer == 1) {
      length *= 4;
      length += possible_free_framelen;
      bitrate = length * samplerate / 48000;
    } else {
      length += possible_free_framelen;
      bitrate = length * samplerate /
          ((layer == 3 && version != 3) ? 72000 : 144000);
    }
  } else {
    /* calculating */
    bitrate = mp3types_bitrates[version == 3 ? 0 : 1][layer - 1][bitrate];
    if (layer == 1) {
      length = ((12000 * bitrate / samplerate) + length) * 4;
    } else {
      length += ((layer == 3
              && version != 3) ? 72000 : 144000) * bitrate / samplerate;
    }
  }

  GST_LOG ("mp3typefind: calculated mp3 frame length of %u bytes", length);
  GST_LOG
      ("mp3typefind: samplerate = %u - bitrate = %u - layer = %u - version = %u"
      " - channels = %u", samplerate, bitrate, layer, version, channels);

  if (put_layer)
    *put_layer = layer;
  if (put_channels)
    *put_channels = channels;
  if (put_bitrate)
    *put_bitrate = bitrate;
  if (put_samplerate)
    *put_samplerate = samplerate;

  return length;
}


static GstStaticCaps mp3_caps = GST_STATIC_CAPS ("audio/mpeg, "
    "mpegversion = (int) 1, layer = (int) [ 1, 3 ]");
#define MP3_CAPS (gst_static_caps_get(&mp3_caps))
/*
 * random values for typefinding
 * if no more data is available, we will return a probability of
 * (found_headers/TRY_HEADERS) * (MAXIMUM * (TRY_SYNC - bytes_skipped)
 *        / TRY_SYNC)
 * if found_headers >= MIN_HEADERS
 */
#define GST_MP3_TYPEFIND_MIN_HEADERS (2)
#define GST_MP3_TYPEFIND_TRY_HEADERS (5)
#define GST_MP3_TYPEFIND_TRY_SYNC (GST_TYPE_FIND_MAXIMUM * 100) /* 10kB */
#define GST_MP3_TYPEFIND_SYNC_SIZE (2048)

static void
mp3_type_find_at_offset (GstTypeFind * tf, guint64 start_off,
    guint * found_layer, GstTypeFindProbability * found_prob)
{
  guint8 *data = NULL;
  guint8 *data_end;
  guint size;
  guint64 skipped;
  gint last_free_offset = -1;
  gint last_free_framelen = -1;

  *found_layer = 0;
  *found_prob = 0;

  size = 0;
  skipped = 0;
  while (skipped < GST_MP3_TYPEFIND_TRY_SYNC) {
    if (size <= 0) {
      size = GST_MP3_TYPEFIND_SYNC_SIZE * 2;
      do {
        size /= 2;
        data = gst_type_find_peek (tf, skipped + start_off, size);
      } while (size > 10 && !data);
      if (!data)
        break;
      data_end = data + size;
    }
    if (*data == 0xFF) {
      guint8 *head_data = NULL;
      guint layer = 0, bitrate, samplerate, channels;
      guint found = 0;          /* number of valid headers found */
      guint64 offset = skipped;

      while (found < GST_MP3_TYPEFIND_TRY_HEADERS) {
        guint32 head;
        guint length;
        guint prev_layer = 0, prev_bitrate = 0;
        guint prev_channels = 0, prev_samplerate = 0;
        gboolean free = FALSE;

        if ((gint64) (offset - skipped + 4) >= 0 &&
            data + offset - skipped + 4 < data_end) {
          head_data = data + offset - skipped;
        } else {
          head_data = gst_type_find_peek (tf, offset + start_off, 4);
        }
        if (!head_data)
          break;
        head = GST_READ_UINT32_BE (head_data);
        if (!(length = mp3_type_frame_length_from_header (head, &layer,
                    &channels, &bitrate, &samplerate, &free,
                    last_free_framelen))) {
          if (free) {
            if (last_free_offset == -1)
              last_free_offset = offset;
            else {
              last_free_framelen = offset - last_free_offset;
              offset = last_free_offset;
              continue;
            }
          } else {
            last_free_framelen = -1;
          }

          GST_LOG ("%d. header at offset %" G_GUINT64_FORMAT
              " (0x%" G_GINT64_MODIFIER "x) was not an mp3 header "
              "(possibly-free: %s)", found + 1, start_off + offset,
              start_off + offset, free ? "yes" : "no");
          break;
        }
        if ((prev_layer && prev_layer != layer) ||
            /* (prev_bitrate && prev_bitrate != bitrate) || <-- VBR */
            (prev_samplerate && prev_samplerate != samplerate) ||
            (prev_channels && prev_channels != channels)) {
          /* this means an invalid property, or a change, which might mean
           * that this is not a mp3 but just a random bytestream. It could
           * be a freaking funky encoded mp3 though. We'll just not count
           * this header*/
          prev_layer = layer;
          prev_bitrate = bitrate;
          prev_channels = channels;
          prev_samplerate = samplerate;
        } else {
          found++;
          GST_LOG ("found %d. header at offset %" G_GUINT64_FORMAT " (0x%"
              G_GINT64_MODIFIER "X)", found, start_off + offset,
              start_off + offset);
        }
        offset += length;
      }
      g_assert (found <= GST_MP3_TYPEFIND_TRY_HEADERS);
      if (found == GST_MP3_TYPEFIND_TRY_HEADERS ||
          (found >= GST_MP3_TYPEFIND_MIN_HEADERS && head_data == NULL)) {
        /* we can make a valid guess */
        guint probability = found * GST_TYPE_FIND_MAXIMUM *
            (GST_MP3_TYPEFIND_TRY_SYNC - skipped) /
            GST_MP3_TYPEFIND_TRY_HEADERS / GST_MP3_TYPEFIND_TRY_SYNC;

        if (probability < GST_TYPE_FIND_MINIMUM)
          probability = GST_TYPE_FIND_MINIMUM;
        if (start_off > 0)
          probability /= 2;

        GST_INFO
            ("audio/mpeg calculated %u  =  %u  *  %u / %u  *  (%u - %u) / %u",
            probability, GST_TYPE_FIND_MAXIMUM, found,
            GST_MP3_TYPEFIND_TRY_HEADERS, GST_MP3_TYPEFIND_TRY_SYNC, skipped,
            GST_MP3_TYPEFIND_TRY_SYNC);
        /* make sure we're not id3 tagged */
        head_data = gst_type_find_peek (tf, -128, 3);
        if (!head_data) {
          probability = probability * 4 / 5;
        } else if (memcmp (head_data, "TAG", 3) == 0) {
          probability = 0;
        }
        g_assert (probability <= GST_TYPE_FIND_MAXIMUM);

        *found_prob = probability;
        if (probability > 0)
          *found_layer = layer;
        return;
      }
    }
    data++;
    skipped++;
    size--;
  }
}

static void
mp3_type_find (GstTypeFind * tf, gpointer unused)
{
  GstTypeFindProbability prob, mid_prob;
  guint8 *data;
  guint layer, mid_layer;
  guint64 length;

  mp3_type_find_at_offset (tf, 0, &layer, &prob);
  length = gst_type_find_get_length (tf);

  if (length == 0 || length == (guint64) - 1) {
    if (prob != 0)
      goto suggest;
    return;
  }

  /* if we're pretty certain already, skip the additional check */
  if (prob >= GST_TYPE_FIND_LIKELY)
    goto suggest;

  mp3_type_find_at_offset (tf, length / 2, &mid_layer, &mid_prob);

  if (mid_prob > 0) {
    if (prob == 0) {
      GST_LOG ("detected audio/mpeg only in the middle (p=%u)", mid_prob);
      layer = mid_layer;
      prob = mid_prob;
      goto suggest;
    }

    if (layer != mid_layer) {
      GST_WARNING ("audio/mpeg layer discrepancy: %u vs. %u", layer, mid_layer);
      return;                   /* FIXME: or should we just go with the one in the middle? */
    }

    /* detected mpeg audio both in middle of the file and at the start */
    prob = (prob + mid_prob) / 2;
    goto suggest;
  }

  /* let's see if there's a valid header right at the start */
  data = gst_type_find_peek (tf, 0, 4); /* use min. frame size? */
  if (data && mp3_type_frame_length_from_header (GST_READ_UINT32_BE (data),
          &layer, NULL, NULL, NULL, NULL, 0) != 0) {
    if (prob == 0)
      prob = GST_TYPE_FIND_POSSIBLE - 10;
    else
      prob = MAX (GST_TYPE_FIND_POSSIBLE - 10, prob + 10);
  }

  if (prob > 0)
    goto suggest;

  return;

suggest:
  {
    GstCaps *caps;

    g_assert (layer > 0);

    caps = gst_caps_make_writable (MP3_CAPS);
    gst_structure_set (gst_caps_get_structure (caps, 0), "layer",
        G_TYPE_INT, layer, NULL);
    gst_type_find_suggest (tf, prob, caps);
    gst_caps_unref (caps);
    return;
  }
}

/*** audio/x-ac3 ***/
static GstStaticCaps ac3_caps = GST_STATIC_CAPS ("audio/x-ac3");

#define AC3_CAPS (gst_static_caps_get(&ac3_caps))

static void
ac3_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 2);

  if (data) {
    /* pretty lame method... */
    if (data[0] == 0x0b && data[1] == 0x77) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_POSSIBLE, AC3_CAPS);
      return;
    }
  }
}

/*** wavpack ***/

static GstStaticCaps wavpack_caps =
GST_STATIC_CAPS ("audio/x-wavpack, framed = (boolean) false");

#define WAVPACK_CAPS (gst_static_caps_get(&wavpack_caps))

static GstStaticCaps wavpack_correction_caps =
GST_STATIC_CAPS ("audio/x-wavpack-correction, framed = (boolean) false");

#define WAVPACK_CORRECTION_CAPS (gst_static_caps_get(&wavpack_correction_caps))

static void
wavpack_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 32);

  if (!data)
    return;

  if (data[0] == 'w' && data[1] == 'v' && data[2] == 'p' && data[3] == 'k') {
    guint32 blocksize, sublen;
    gint32 left;

    GST_LOG ("got wavpack header");

    /* wavpack blocks can be fairly large, possibly larger than the max.
     * limits imposed by certain typefinding elements like id3demux or
     * apedemux. So if the first wavpack block is larger than any particular
     * limit, we try to get a smaller chunk and hope we get lucky parsing
     * only bits of it (case at hand: first wavpack block: 42kB, with a
     * max. limit imposed by apedemux/id3demux: 40kB) */
    blocksize = GST_READ_UINT32_LE (data + 4);
    do {
      /* peek from offset 0, otherwise it won't work with apedemux */
      data = gst_type_find_peek (tf, 0, 32 + blocksize);
      if (data != NULL)
        break;
      if (32 + blocksize < 512) /* random threshold */
        break;
      blocksize = (blocksize * 3) / 4;
    } while (data == NULL);
    if (!data)
      return;
    data += 32;
    left = (gint32) blocksize;
    while (left > 2) {
      sublen = ((guint32) data[1]) << 1;
      if (data[0] & 0x80) {
        sublen |= (((guint32) data[2]) << 9) | (((guint32) data[3]) << 17);
        sublen += 1 + 3;        /* id + length */
      } else {
        sublen += 1 + 1;        /* id + length */
      }
      if (sublen > blocksize)
        return;
      if ((data[0] & 0x20) == 0) {
        switch (data[0] & 0x0f) {
          case 0xa:            /* ID_WV_BITSTREAM  */
          case 0xc:            /* ID_WVX_BITSTREAM */
            gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, WAVPACK_CAPS);
            return;
          case 0xb:            /* ID_WVC_BITSTREAM */
            gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY,
                WAVPACK_CORRECTION_CAPS);
            return;
          default:
            break;
        }
      }
      left -= sublen;
      data += sublen;
    }
  }
}


/*** video/mpeg systemstream ***/

static GstStaticCaps mpeg_sys_caps = GST_STATIC_CAPS ("video/mpeg, "
    "systemstream = (boolean) true, mpegversion = (int) [ 1, 2 ]");
#define MPEG_SYS_CAPS gst_static_caps_get(&mpeg_sys_caps)
#define IS_MPEG_HEADER(data)            ((((guint8 *)data)[0] == 0x00) &&       \
                                         (((guint8 *)data)[1] == 0x00) &&       \
                                         (((guint8 *)data)[2] == 0x01) &&       \
                                         (((guint8 *)data)[3] == 0xBA))
#define IS_MPEG_SYSTEM_HEADER(data)     ((((guint8 *)data)[0] == 0x00) &&       \
                                         (((guint8 *)data)[1] == 0x00) &&       \
                                         (((guint8 *)data)[2] == 0x01) &&       \
                                         (((guint8 *)data)[3] == 0xBB))
#define IS_MPEG_PACKET_HEADER(data)     ((((guint8 *)data)[0] == 0x00) &&       \
                                         (((guint8 *)data)[1] == 0x00) &&       \
                                         (((guint8 *)data)[2] == 0x01) &&       \
                                         ((((guint8 *)data)[3] & 0x80) == 0x80))
#define IS_MPEG_PES_HEADER(data)        ((((guint8 *)data)[0] == 0x00) &&       \
                                         (((guint8 *)data)[1] == 0x00) &&       \
                                         (((guint8 *)data)[2] == 0x01) && \
                                         ((((guint8 *)data)[3] == 0xE0) || \
                                          (((guint8 *)data)[3] == 0xC0) || \
                                          (((guint8 *)data)[3] == 0xBD)))

static void
mpeg2_sys_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 5);

  if (data && IS_MPEG_HEADER (data)) {
    if ((data[4] & 0xC0) == 0x40) {
      /* type 2 */
      GstCaps *caps = gst_caps_copy (MPEG_SYS_CAPS);

      gst_structure_set (gst_caps_get_structure (caps, 0), "mpegversion",
          G_TYPE_INT, 2, NULL);
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, caps);
    } else if ((data[4] & 0xF0) == 0x20) {
      GstCaps *caps = gst_caps_copy (MPEG_SYS_CAPS);

      gst_structure_set (gst_caps_get_structure (caps, 0), "mpegversion",
          G_TYPE_INT, 1, NULL);
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, caps);
      gst_caps_unref (caps);
    }
  } else if (data && IS_MPEG_PES_HEADER (data)) {
    /* PES stream */
    GstCaps *caps = gst_caps_copy (MPEG_SYS_CAPS);

    gst_structure_set (gst_caps_get_structure (caps, 0), "mpegversion",
        G_TYPE_INT, 2, NULL);
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, caps);
  }

};

/* ATTENTION: ugly return value:
 * 0 -  invalid data
 * 1 - not enough data
 * anything else - size until next package
 */
static guint
mpeg1_parse_header (GstTypeFind * tf, guint64 offset)
{
  guint8 *data = gst_type_find_peek (tf, offset, 4);
  guint size;

  if (!data) {
    GST_LOG ("couldn't get MPEG header bytes");
    return 1;
  }

  if (data[0] != 0 || data[1] != 0 || data[2] != 1) {
    return 0;
  }
  offset += 4;

  switch (data[3]) {
    case 0xBA:                 /* pack header */
      data = gst_type_find_peek (tf, offset, 8);
      if (!data) {
        GST_LOG ("couldn't get MPEG pack header bytes");
        return 1;
      }
      size = 12;
      /* check marker bits */
      if ((data[0] & 0xF1) != 0x21 ||
          (data[2] & 0x01) != 0x01 ||
          (data[4] & 0x01) != 0x01 ||
          (data[5] & 0x80) != 0x80 || (data[7] & 0x01) != 0x01)
        return 0;
      break;

    case 0xB9:                 /* ISO end code */
      size = 4;
      break;

    case 0xBB:                 /* system header */
      data = gst_type_find_peek (tf, offset, 2);
      if (!data) {
        GST_LOG ("couldn't get MPEG pack header bytes");
        return 1;
      }
      size = GST_READ_UINT16_BE (data) + 6;
      offset += 2;
      data = gst_type_find_peek (tf, offset, size - 6);
      if (!data) {
        GST_LOG ("couldn't get MPEG pack header bytes");
        return 1;
      }
      /* check marker bits */
      if ((data[0] & 0x80) != 0x80 ||
          (data[2] & 0x01) != 0x01 || (data[4] & 0x20) != 0x20)
        return 0;
      /* check stream marker bits */
      for (offset = 6; offset < (size - 6); offset += 3) {
        if (data[offset] <= 0xBB || (data[offset + 1] & 0xC0) != 0xC0)
          return 0;
      }
      break;

    default:
      if (data[3] < 0xB9)
        return 0;
      data = gst_type_find_peek (tf, offset, 2);
      if (!data) {
        GST_LOG ("couldn't get MPEG pack header bytes");
        return 1;
      }
      size = GST_READ_UINT16_BE (data) + 6;
      /* FIXME: we could check PTS/DTS marker bits here... (bit overkill) */
      break;
  }

  return size;
}

/* calculation of possibility to identify random data as mpeg systemstream:
 * bits that must match in header detection:            32 (or more)
 * chance that random data is identifed:                1/2^32
 * chance that GST_MPEG_TYPEFIND_TRY_HEADERS headers are identified:
 *                                      1/2^(32*GST_MPEG_TYPEFIND_TRY_HEADERS)
 * chance that this happens in GST_MPEG_TYPEFIND_TRY_SYNC bytes:
 *                                      1-(1+1/2^(32*GST_MPEG_TYPEFIND_TRY_HEADERS)^GST_MPEG_TYPEFIND_TRY_SYNC)
 * for current values:
 *                                      1-(1+1/2^(32*4)^101024)
 *                                    = <some_number>
 */
#define GST_MPEG_TYPEFIND_TRY_HEADERS 4
#define GST_MPEG_TYPEFIND_TRY_SYNC (100 * 1024) /* 100kB */
#define GST_MPEG_TYPEFIND_SYNC_SIZE 2048
static void
mpeg1_sys_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = NULL;
  guint size = 0;
  guint64 skipped = 0;
  GstCaps *caps;

  while (skipped < GST_MPEG_TYPEFIND_TRY_SYNC) {
    if (size < 4) {
      data = gst_type_find_peek (tf, skipped, GST_MPEG_TYPEFIND_SYNC_SIZE);
      if (!data)
        break;
      size = GST_MPEG_TYPEFIND_SYNC_SIZE;
    }
    if (IS_MPEG_HEADER (data)) {
      /* found packet start code */
      guint found = 0;
      guint packet_size = 0;
      guint64 offset = skipped;

      while (found < GST_MPEG_TYPEFIND_TRY_HEADERS) {
        packet_size = mpeg1_parse_header (tf, offset);
        if (packet_size <= 1)
          break;
        offset += packet_size;
        found++;
      }
      g_assert (found <= GST_MPEG_TYPEFIND_TRY_HEADERS);
      if (found == GST_MPEG_TYPEFIND_TRY_HEADERS || packet_size == 1) {
        caps = gst_caps_copy (MPEG_SYS_CAPS);
        gst_structure_set (gst_caps_get_structure (caps, 0), "mpegversion",
            G_TYPE_INT, 1, NULL);
        gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 1, caps);
        gst_caps_unref (caps);
        return;
      }
    }
    data++;
    skipped++;
    size--;
  }
}

/*** video/mpeg MPEG-4 elementary video stream ***/

static GstStaticCaps mpeg4_video_caps = GST_STATIC_CAPS ("video/mpeg, "
    "systemstream = (boolean) false, mpegversion = 4");
#define MPEG4_VIDEO_CAPS gst_static_caps_get(&mpeg4_video_caps)
static void
mpeg4_video_type_find (GstTypeFind * tf, gpointer unused)
{
  /* Header is a video object start code followed by a video object layer
   * start code. The last byte of this 8-byte header can be from 0x20 - 0x2F */
  static const guint8 header[] = { 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
  guint8 *data = NULL;

  data = gst_type_find_peek (tf, 0, 8);

  if (data && memcmp (data, header, 7) == 0 &&
      data[7] >= 0x20 && data[7] <= 0x2F) {
    GstCaps *caps = gst_caps_copy (MPEG4_VIDEO_CAPS);

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 1, caps);
    gst_caps_unref (caps);
  }
}

/*** video/mpeg video stream ***/

static GstStaticCaps mpeg_video_caps = GST_STATIC_CAPS ("video/mpeg, "
    "systemstream = (boolean) false");
#define MPEG_VIDEO_CAPS gst_static_caps_get(&mpeg_video_caps)
static void
mpeg_video_type_find (GstTypeFind * tf, gpointer unused)
{
  static const guint8 sequence_header[] = { 0x00, 0x00, 0x01, 0xb3 };
  guint8 *data = NULL;

  data = gst_type_find_peek (tf, 0, 8);

  if (data && memcmp (data, sequence_header, 4) == 0) {
    GstCaps *caps = gst_caps_copy (MPEG_VIDEO_CAPS);

    gst_structure_set (gst_caps_get_structure (caps, 0), "mpegversion",
        G_TYPE_INT, 1, NULL);
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 1, caps);
    gst_caps_unref (caps);
  }
}

/*
 * Idea is the same as MPEG system stream typefinding: We check each
 * byte of the stream to see if - from that point on - the stream
 * matches a predefined set of marker bits as defined in the MPEG
 * video specs.
 *
 * I'm sure someone will do a chance calculation here too.
 */

#define GST_MPEGVID_TYPEFIND_TRY_PICTURES 6
#define GST_MPEGVID_TYPEFIND_TRY_SYNC (100 * 1024)      /* 100 kB */
#define GST_MPEGVID_TYPEFIND_SYNC_SIZE 2048

static void
mpeg_video_stream_type_find (GstTypeFind * tf, gpointer unused)
{
  gint size = 0, found = 0;
  guint64 skipped = 0;
  guint8 *data = NULL;

  while (1) {
    if (found >= GST_MPEGVID_TYPEFIND_TRY_PICTURES) {
      GstCaps *caps = gst_caps_copy (MPEG_VIDEO_CAPS);

      gst_structure_set (gst_caps_get_structure (caps, 0), "mpegversion",
          G_TYPE_INT, 1, NULL);
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM - 2, caps);
      gst_caps_unref (caps);
      return;
    }

    if (skipped > GST_MPEGVID_TYPEFIND_TRY_SYNC)
      break;

    if (size < 5) {
      data = gst_type_find_peek (tf, skipped, GST_MPEGVID_TYPEFIND_SYNC_SIZE);
      if (!data)
        break;
      size = GST_MPEGVID_TYPEFIND_SYNC_SIZE;
    }

    /* are we a sequence (0xB3) or GOP (0xB8) header? */
    if (data[0] == 0x0 && data[1] == 0x0 && data[2] == 0x1 &&
        (data[3] == 0xB3 || data[3] == 0xB8)) {
      size -= 8;
      data += 8;
      skipped += 8;
      if (data[3] == 0xB3)
        continue;
      else if (size < 4) {
        data = gst_type_find_peek (tf, skipped, GST_MPEGVID_TYPEFIND_SYNC_SIZE);
        size = GST_MPEGVID_TYPEFIND_SYNC_SIZE;
        if (!data)
          break;
      }
      /* else, we should now see an image */
    }

    /* image header (and, when found, slice header) */
    if (data[0] == 0x0 && data[1] == 0x0 && data[2] == 0x1 && data[4] == 0x0) {
      size -= 8;
      data += 8;
      skipped += 8;
      if (size < 5) {
        data = gst_type_find_peek (tf, skipped, GST_MPEGVID_TYPEFIND_SYNC_SIZE);
        size = GST_MPEGVID_TYPEFIND_SYNC_SIZE;
        if (!data)
          break;
      }
      if ((data[0] == 0x0 && data[1] == 0x0 &&
              data[2] == 0x1 && data[3] == 0x1) ||
          (data[1] == 0x0 && data[2] == 0x0 &&
              data[3] == 0x1 && data[4] == 0x1)) {
        size -= 4;
        data += 4;
        skipped += 4;
        found += 1;
        continue;
      }
    }

    size--;
    data++;
    skipped++;
  }
}

/*** audio/x-aiff ***/

static GstStaticCaps aiff_caps = GST_STATIC_CAPS ("audio/x-aiff");

#define AIFF_CAPS gst_static_caps_get(&aiff_caps)
static void
aiff_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data && memcmp (data, "FORM", 4) == 0) {
    data += 8;
    if (memcmp (data, "AIFF", 4) == 0 || memcmp (data, "AIFC", 4) == 0)
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, AIFF_CAPS);
  }
}

/*** audio/x-aiff ***/

static GstStaticCaps svx_caps = GST_STATIC_CAPS ("audio/x-svx");

#define SVX_CAPS gst_static_caps_get(&svx_caps)
static void
svx_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data && memcmp (data, "FORM", 4) == 0) {
    data += 8;
    if (memcmp (data, "8SVX", 4) == 0 || memcmp (data, "16SV", 4) == 0)
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SVX_CAPS);
  }
}

/*** audio/x-shorten ***/

static GstStaticCaps shn_caps = GST_STATIC_CAPS ("audio/x-shorten");

#define SHN_CAPS gst_static_caps_get(&shn_caps)
static void
shn_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data && memcmp (data, "ajkg", 4) == 0) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SHN_CAPS);
  }
  data = gst_type_find_peek (tf, -8, 8);
  if (data && memcmp (data, "SHNAMPSK", 8) == 0) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SHN_CAPS);
  }
}

/*** application/x-ape ***/

static GstStaticCaps ape_caps = GST_STATIC_CAPS ("application/x-ape");

#define APE_CAPS gst_static_caps_get(&ape_caps)
static void
ape_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data && memcmp (data, "MAC ", 4) == 0) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY + 10, APE_CAPS);
  }
}

/*** ISO FORMATS ***/

/*** audio/x-m4a ***/

static GstStaticCaps m4a_caps = GST_STATIC_CAPS ("audio/x-m4a");

#define M4A_CAPS (gst_static_caps_get(&m4a_caps))
static void
m4a_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 4, 8);

  if (data &&
      (memcmp (data, "ftypM4A ", 8) == 0 ||
          memcmp (data, "ftypmp42", 8) == 0)) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, M4A_CAPS);
  }
}

/*** application/x-3gp ***/

/* The Q is there because variables can't start with a number. */


static GstStaticCaps q3gp_caps = GST_STATIC_CAPS ("application/x-3gp");

#define Q3GP_CAPS (gst_static_caps_get(&q3gp_caps))
static void
q3gp_type_find (GstTypeFind * tf, gpointer unused)
{

  guint32 ftyp_size = 0;
  gint offset = 0;
  guint8 *data = NULL;

  if ((data = gst_type_find_peek (tf, 0, 12)) == NULL) {
    return;
  }

  data += 4;
  if (memcmp (data, "ftyp", 4) != 0) {
    return;
  }

  /* check major brand */
  data += 4;
  if (memcmp (data, "3gp", 3) == 0 ||
      memcmp (data, "3gr", 3) == 0 ||
      memcmp (data, "3gs", 3) == 0 || memcmp (data, "3gg", 3) == 0) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, Q3GP_CAPS);
    return;
  }

  /* check compatible brands */
  if ((data = gst_type_find_peek (tf, 0, 4)) != NULL) {
    ftyp_size = GST_READ_UINT32_BE (data);
  }
  for (offset = 16; offset < ftyp_size; offset += 4) {
    if ((data = gst_type_find_peek (tf, offset, 3)) == NULL) {
      break;
    }
    if (memcmp (data, "3gp", 3) == 0 ||
        memcmp (data, "3gr", 3) == 0 ||
        memcmp (data, "3gs", 3) == 0 || memcmp (data, "3gg", 3) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, Q3GP_CAPS);
      break;
    }
  }

  return;

}

/*** video/quicktime ***/

static GstStaticCaps qt_caps = GST_STATIC_CAPS ("video/quicktime");

#define QT_CAPS gst_static_caps_get(&qt_caps)
#define STRNCMP(x,y,z) (strncmp ((char*)(x), (char*)(y), z))

static void
qt_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data;
  guint tip = 0;
  guint64 offset = 0;
  guint64 size;

  while ((data = gst_type_find_peek (tf, offset, 8)) != NULL) {
    /* box/atom types that are in common with ISO base media file format */
    if (STRNCMP (&data[4], "moov", 4) == 0 ||
        STRNCMP (&data[4], "mdat", 4) == 0 ||
        STRNCMP (&data[4], "ftyp", 4) == 0 ||
        STRNCMP (&data[4], "free", 4) == 0 ||
        STRNCMP (&data[4], "skip", 4) == 0) {
      if (tip == 0) {
        tip = GST_TYPE_FIND_LIKELY;
      } else {
        tip = GST_TYPE_FIND_NEARLY_CERTAIN;
      }
    }
    /* other box/atom types, apparently quicktime specific */
    else if (STRNCMP (&data[4], "pnot", 4) == 0 ||
        STRNCMP (&data[4], "PICT", 4) == 0 ||
        STRNCMP (&data[4], "wide", 4) == 0) {
      tip = GST_TYPE_FIND_MAXIMUM;
      break;
    } else {
      tip = 0;
      break;
    }
    size = GST_READ_UINT32_BE (data);
    if (size == 1) {
      guint8 *sizedata;

      sizedata = gst_type_find_peek (tf, offset + 8, 8);
      if (sizedata == NULL)
        break;

      size = GST_READ_UINT64_BE (sizedata);
    } else {
      if (size < 8)
        break;
    }
    offset += size;
  }
  if (tip > 0) {
    gst_type_find_suggest (tf, tip, QT_CAPS);
  }
};


/*** audio/x-mod ***/

static GstStaticCaps mod_caps = GST_STATIC_CAPS ("audio/x-mod");

#define MOD_CAPS gst_static_caps_get(&mod_caps)
/* FIXME: M15 CheckType to do */
static void
mod_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data;

  /* MOD */
  if ((data = gst_type_find_peek (tf, 1080, 4)) != NULL) {
    /* Protracker and variants */
    if ((memcmp (data, "M.K.", 4) == 0) || (memcmp (data, "M!K!", 4) == 0) ||
        /* Star Tracker */
        (memcmp (data, "FLT", 3) == 0 && isdigit (data[3])) ||
        (memcmp (data, "EXO", 3) == 0 && isdigit (data[3])) ||
        /* Oktalyzer (Amiga) */
        (memcmp (data, "OKTA", 4) == 0) ||
        /* Oktalyser (Atari) */
        (memcmp (data, "CD81", 4) == 0) ||
        /* Fasttracker */
        (memcmp (data + 1, "CHN", 3) == 0 && isdigit (data[0])) ||
        /* Fasttracker or Taketracker */
        (memcmp (data + 2, "CH", 2) == 0 && isdigit (data[0])
            && isdigit (data[1])) || (memcmp (data + 2, "CN", 2) == 0
            && isdigit (data[0]) && isdigit (data[1]))) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
      return;
    }
  }
  /* XM */
  if ((data = gst_type_find_peek (tf, 0, 38)) != NULL) {
    if (memcmp (data, "Extended Module: ", 17) == 0 && data[37] == 0x1A) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
      return;
    }
  }
  /* OKT */
  if (data || (data = gst_type_find_peek (tf, 0, 8)) != NULL) {
    if (memcmp (data, "OKTASONG", 8) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
      return;
    }
  }
  if (data || (data = gst_type_find_peek (tf, 0, 4)) != NULL) {
    /* 669 */
    if ((memcmp (data, "if", 2) == 0) || (memcmp (data, "JN", 2) == 0)) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, MOD_CAPS);
      return;
    }
    /* AMF */
    if ((memcmp (data, "AMF", 3) == 0 && data[3] > 10 && data[3] < 14) ||
        /* IT */
        (memcmp (data, "IMPM", 4) == 0) ||
        /* MED */
        (memcmp (data, "MMD0", 4) == 0) || (memcmp (data, "MMD1", 4) == 0) ||
        /* MTM */
        (memcmp (data, "MTM", 3) == 0)) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
      return;
    }
    /* DSM */
    if (memcmp (data, "RIFF", 4) == 0) {
      guint8 *data2 = gst_type_find_peek (tf, 8, 4);

      if (data2) {
        if (memcmp (data2, "DSMF", 4) == 0) {
          gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
          return;
        }
      }
    }
    /* FAM */
    if (memcmp (data, "FAM\xFE", 4) == 0) {
      guint8 *data2 = gst_type_find_peek (tf, 44, 3);

      if (data2) {
        if (memcmp (data2, "compare", 3) == 0) {
          gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
          return;
        }
      } else {
        gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, MOD_CAPS);
        return;
      }
    }
    /* GDM */
    if (memcmp (data, "GDM\xFE", 4) == 0) {
      guint8 *data2 = gst_type_find_peek (tf, 71, 4);

      if (data2) {
        if (memcmp (data2, "GMFS", 4) == 0) {
          gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
          return;
        }
      } else {
        gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, MOD_CAPS);
        return;
      }
    }
  }
  /* IMF */
  if ((data = gst_type_find_peek (tf, 60, 4)) != NULL) {
    if (memcmp (data, "IM10", 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
      return;
    }
  }
  /* S3M */
  if ((data = gst_type_find_peek (tf, 44, 4)) != NULL) {
    if (memcmp (data, "SCRM", 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MOD_CAPS);
      return;
    }
  }
}

/*** application/x-shockwave-flash ***/

static GstStaticCaps swf_caps =
GST_STATIC_CAPS ("application/x-shockwave-flash");
#define SWF_CAPS (gst_static_caps_get(&swf_caps))
static void
swf_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data && (data[0] == 'F' || data[0] == 'C') &&
      data[1] == 'W' && data[2] == 'S') {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SWF_CAPS);
  }
}

/*** image/jpeg ***/

static GstStaticCaps jpeg_caps = GST_STATIC_CAPS ("image/jpeg");

#define JPEG_CAPS (gst_static_caps_get(&jpeg_caps))
static void
jpeg_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 10);
  guint8 header[2] = { 0xFF, 0xD8 };

  if (data && memcmp (data, header, 2) == 0) {
    if (memcmp (data + 6, "JFIF", 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, JPEG_CAPS);
    } else if (memcmp (data + 6, "Exif", 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, JPEG_CAPS);
    } else {
      gst_type_find_suggest (tf, GST_TYPE_FIND_POSSIBLE, JPEG_CAPS);
    }
  }
}

/*** image/bmp ***/

static GstStaticCaps bmp_caps = GST_STATIC_CAPS ("image/bmp");

#define BMP_CAPS (gst_static_caps_get(&bmp_caps))
static void
bmp_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 18);

  if (data && memcmp (data, "BM", 2) == 0) {
    if ((data[14] == 0x0C ||
            data[14] == 0x28 ||
            data[14] == 0xF0) &&
        data[15] == 0 && data[16] == 0 && data[17] == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, BMP_CAPS);
    }
  }
}

/*** image/tiff ***/
static GstStaticCaps tiff_caps = GST_STATIC_CAPS ("image/tiff, "
    "endianness = (int) { BIG_ENDIAN, LITTLE_ENDIAN }");
#define TIFF_CAPS (gst_static_caps_get(&tiff_caps))
static GstStaticCaps tiff_be_caps = GST_STATIC_CAPS ("image/tiff, "
    "endianness = (int) BIG_ENDIAN");
#define TIFF_BE_CAPS (gst_static_caps_get(&tiff_be_caps))
static GstStaticCaps tiff_le_caps = GST_STATIC_CAPS ("image/tiff, "
    "endianness = (int) LITTLE_ENDIAN");
#define TIFF_LE_CAPS (gst_static_caps_get(&tiff_le_caps))
static void
tiff_type_find (GstTypeFind * tf, gpointer ununsed)
{
  guint8 *data = gst_type_find_peek (tf, 0, 8);
  guint8 le_header[4] = { 0x49, 0x49, 0x2A, 0x00 };
  guint8 be_header[4] = { 0x4D, 0x4D, 0x00, 0x2A };

  if (data) {
    if (memcmp (data, le_header, 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, TIFF_LE_CAPS);
    } else if (memcmp (data, be_header, 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, TIFF_BE_CAPS);
    }
  }
}

static GstStaticCaps sds_caps = GST_STATIC_CAPS ("audio/x-sds");

#define SDS_CAPS (gst_static_caps_get(&sds_caps))
static void
sds_type_find (GstTypeFind * tf, gpointer ununsed)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);
  guint8 mask[4] = { 0xFF, 0xFF, 0x80, 0xFF };
  guint8 match[4] = { 0xF0, 0x7E, 0, 0x01 };
  gint x;

  if (data) {
    for (x = 0; x < 4; x++) {
      if ((data[x] & mask[x]) != match[x]) {
        return;
      }
    }
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SDS_CAPS);
  }
}

static GstStaticCaps ircam_caps = GST_STATIC_CAPS ("audio/x-ircam");

#define IRCAM_CAPS (gst_static_caps_get(&ircam_caps))
static void
ircam_type_find (GstTypeFind * tf, gpointer ununsed)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);
  guint8 mask[4] = { 0xFF, 0xFF, 0xF8, 0xFF };
  guint8 match[4] = { 0x64, 0xA3, 0x00, 0x00 };
  gint x;
  gboolean matched = TRUE;

  if (!data) {
    return;
  }
  for (x = 0; x < 4; x++) {
    if ((data[x] & mask[x]) != match[x]) {
      matched = FALSE;
    }
  }
  if (matched) {
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, IRCAM_CAPS);
    return;
  }
  /* now try the reverse version */
  matched = TRUE;
  for (x = 0; x < 4; x++) {
    if ((data[x] & mask[3 - x]) != match[3 - x]) {
      matched = FALSE;
    }
  }
}


/*** video/x-matroska ***/
static GstStaticCaps matroska_caps = GST_STATIC_CAPS ("video/x-matroska");

#define MATROSKA_CAPS (gst_static_caps_get(&matroska_caps))
static void
matroska_type_find (GstTypeFind * tf, gpointer ununsed)
{
  /* 4 bytes for EBML ID, 1 byte for header length identifier */
  guint8 *data = gst_type_find_peek (tf, 0, 4 + 1);
  gint len_mask = 0x80, size = 1, n = 1, total;
  guint8 probe_data[] = { 'm', 'a', 't', 'r', 'o', 's', 'k', 'a' };

  if (!data)
    return;

  /* ebml header? */
  if (data[0] != 0x1A || data[1] != 0x45 || data[2] != 0xDF || data[3] != 0xA3)
    return;

  /* length of header */
  total = data[4];
  while (size <= 8 && !(total & len_mask)) {
    size++;
    len_mask >>= 1;
  }
  if (size > 8)
    return;
  total &= (len_mask - 1);
  while (n < size)
    total = (total << 8) | data[4 + n++];

  /* get new data for full header, 4 bytes for EBML ID,
   * EBML length tag and the actual header */
  data = gst_type_find_peek (tf, 0, 4 + size + total);
  if (!data)
    return;

  /* the header must contain the document type 'matroska'. For now,
   * we don't parse the whole header but simply check for the
   * availability of that array of characters inside the header.
   * Not fully fool-proof, but good enough. */
  for (n = 4 + size; n <= 4 + size + total - sizeof (probe_data); n++)
    if (!memcmp (&data[n], probe_data, sizeof (probe_data))) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, MATROSKA_CAPS);
      break;
    }
}

/*** video/x-dv ***/

static GstStaticCaps dv_caps = GST_STATIC_CAPS ("video/x-dv, "
    "systemstream = (boolean) true");
#define DV_CAPS (gst_static_caps_get(&dv_caps))
static void
dv_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data;

  data = gst_type_find_peek (tf, 0, 5);

  /* check for DIF  and DV flag */
  if (data && (data[0] == 0x1f) && (data[1] == 0x07) && (data[2] == 0x00) &&
      ((data[4] & 0x01) == 0)) {
    gchar *format;
    GstCaps *caps = gst_caps_copy (DV_CAPS);

    if (data[3] & 0x80) {
      format = "PAL";
    } else {
      format = "NTSC";
    }
    gst_structure_set (gst_caps_get_structure (caps, 0), "format",
        G_TYPE_STRING, format, NULL);

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, caps);
    gst_caps_unref (caps);
  }
}


/*** application/ogg and application/x-annodex ***/
static GstStaticCaps ogg_caps = GST_STATIC_CAPS ("application/ogg");
static GstStaticCaps annodex_caps = GST_STATIC_CAPS ("application/x-annodex");
static GstStaticCaps ogganx_caps =
    GST_STATIC_CAPS ("application/ogg; application/x-annodex");
#define OGGANX_CAPS (gst_static_caps_get(&ogganx_caps))

static void
ogganx_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 28, 8);
  gboolean is_annodex = FALSE;

  if ((data != NULL) && (memcmp (data, "fishead\0", 8) == 0)) {
    is_annodex = TRUE;
  }

  data = gst_type_find_peek (tf, 0, 4);
  if ((data != NULL) && (memcmp (data, "OggS", 4) == 0)) {
    if (is_annodex == TRUE) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM,
          gst_static_caps_get (&annodex_caps));
    }
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM,
        gst_static_caps_get (&ogg_caps));
  }
}

/*** audio/x-vorbis ***/
static GstStaticCaps vorbis_caps = GST_STATIC_CAPS ("audio/x-vorbis");

#define VORBIS_CAPS (gst_static_caps_get(&vorbis_caps))
static void
vorbis_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 30);

  if (data) {
    guint blocksize_0;
    guint blocksize_1;

    /* 1 byte packet type (identification=0x01)
       6 byte string "vorbis"
       4 byte vorbis version */
    if (memcmp (data, "\001vorbis\000\000\000\000", 11) != 0)
      return;
    data += 11;
    /* 1 byte channels must be != 0 */
    if (data[0] == 0)
      return;
    data++;
    /* 4 byte samplerate must be != 0 */
    if (*((guint32 *) data) == 0)
      return;
    data += 16;
    /* blocksize checks */
    blocksize_0 = data[0] & 0x0F;
    blocksize_1 = (data[0] & 0xF0) >> 4;
    if (blocksize_0 > blocksize_1)
      return;
    if (blocksize_0 < 6 || blocksize_0 > 13)
      return;
    if (blocksize_1 < 6 || blocksize_1 > 13)
      return;
    data++;
    /* framing bit */
    if ((data[0] & 0x01) != 1)
      return;
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, VORBIS_CAPS);
  }
}

/*** video/x-theora ***/

static GstStaticCaps theora_caps = GST_STATIC_CAPS ("video/x-theora");

#define THEORA_CAPS (gst_static_caps_get(&theora_caps))
static void
theora_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 7); //42);

  if (data) {
    if (data[0] != 0x80)
      return;
    if (memcmp (&data[1], "theora", 6) != 0)
      return;
    /* FIXME: make this more reliable when specs are out */

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, THEORA_CAPS);
  }
}

/*** application/x-ogm-video or audio***/

static GstStaticCaps ogmvideo_caps =
GST_STATIC_CAPS ("application/x-ogm-video");
#define OGMVIDEO_CAPS (gst_static_caps_get(&ogmvideo_caps))
static void
ogmvideo_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 9);

  if (data) {
    if (memcmp (data, "\001video\000\000\000", 9) != 0)
      return;
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, OGMVIDEO_CAPS);
  }
}

static GstStaticCaps ogmaudio_caps =
GST_STATIC_CAPS ("application/x-ogm-audio");
#define OGMAUDIO_CAPS (gst_static_caps_get(&ogmaudio_caps))
static void
ogmaudio_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 9);

  if (data) {
    if (memcmp (data, "\001audio\000\000\000", 9) != 0)
      return;
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, OGMAUDIO_CAPS);
  }
}

static GstStaticCaps ogmtext_caps = GST_STATIC_CAPS ("application/x-ogm-text");

#define OGMTEXT_CAPS (gst_static_caps_get(&ogmtext_caps))
static void
ogmtext_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 9);

  if (data) {
    if (memcmp (data, "\001text\000\000\000\000", 9) != 0)
      return;
    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, OGMTEXT_CAPS);
  }
}

/*** audio/x-speex ***/

static GstStaticCaps speex_caps = GST_STATIC_CAPS ("audio/x-speex");

#define SPEEX_CAPS (gst_static_caps_get(&speex_caps))
static void
speex_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 80);

  if (data) {
    /* 8 byte string "Speex   "
       24 byte speex version string + int */
    if (memcmp (data, "Speex   ", 8) != 0)
      return;
    data += 32;

    /* 4 byte header size >= 80 */
    if (GST_READ_UINT32_LE (data) < 80)
      return;
    data += 4;

    /* 4 byte sample rate <= 48000 */
    if (GST_READ_UINT32_LE (data) > 48000)
      return;
    data += 4;

    /* currently there are only 3 speex modes. */
    if (GST_READ_UINT32_LE (data) > 3)
      return;
    data += 12;

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, SPEEX_CAPS);
  }
}

/*** application/x-ogg-skeleton ***/
static GstStaticCaps ogg_skeleton_caps =
GST_STATIC_CAPS ("application/x-ogg-skeleton, parsed=(boolean)FALSE");
#define OGG_SKELETON_CAPS (gst_static_caps_get(&ogg_skeleton_caps))
static void
oggskel_type_find (GstTypeFind * tf, gpointer private)
{
  guint8 *data = gst_type_find_peek (tf, 0, 12);

  if (data) {
    /* 8 byte string "fishead\0" for the ogg skeleton stream */
    if (memcmp (data, "fishead\0", 8) != 0)
      return;
    data += 8;

    /* Require that the header contains version 3.0 */
    if (GST_READ_UINT16_LE (data) != 3)
      return;
    data += 2;
    if (GST_READ_UINT16_LE (data) != 0)
      return;

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, OGG_SKELETON_CAPS);
  }
}

static GstStaticCaps cmml_caps = GST_STATIC_CAPS ("text/x-cmml");

#define CMML_CAPS (gst_static_caps_get(&cmml_caps))
static void
cmml_type_find (GstTypeFind * tf, gpointer private)
{
  /* Header is 12 bytes minimum (though we don't check the minor version */
  guint8 *data = gst_type_find_peek (tf, 0, 12);

  if (data) {

    /* 8 byte string "CMML\0\0\0\0" for the magic number */
    if (memcmp (data, "CMML\0\0\0\0", 8) != 0)
      return;
    data += 8;

    /* Require that the header contains at least version 2.0 */
    if (GST_READ_UINT16_LE (data) < 2)
      return;

    gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, CMML_CAPS);
  }
}

/*** application/x-tar ***/

static GstStaticCaps tar_caps = GST_STATIC_CAPS ("application/x-tar");

#define TAR_CAPS (gst_static_caps_get(&tar_caps))
#define OLDGNU_MAGIC "ustar  "  /* 7 chars and a NUL */
#define NEWGNU_MAGIC "ustar"    /* 5 chars and a NUL */
static void
tar_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 257, 8);

  /* of course we are not certain, but we don't want other typefind funcs
   * to detect formats of files within the tar archive, e.g. mp3s */
  if (data) {
    if (memcmp (data, OLDGNU_MAGIC, 8) == 0) {  /* sic */
      gst_type_find_suggest (tf, GST_TYPE_FIND_NEARLY_CERTAIN, TAR_CAPS);
    } else if (memcmp (data, NEWGNU_MAGIC, 6) == 0 &&   /* sic */
        g_ascii_isdigit (data[6]) && g_ascii_isdigit (data[7])) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_NEARLY_CERTAIN, TAR_CAPS);
    }
  }
}

/*** application/x-ar ***/

static GstStaticCaps ar_caps = GST_STATIC_CAPS ("application/x-ar");

#define AR_CAPS (gst_static_caps_get(&ar_caps))
static void
ar_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 24);

  if (data && memcmp (data, "!<arch>", 7) == 0) {
    gint i;

    for (i = 7; i < 24; ++i) {
      if (!g_ascii_isprint (data[i]) && data[i] != '\n') {
        gst_type_find_suggest (tf, GST_TYPE_FIND_POSSIBLE, AR_CAPS);
      }
    }

    gst_type_find_suggest (tf, GST_TYPE_FIND_NEARLY_CERTAIN, AR_CAPS);
  }
}

/*** audio/x-au ***/

/* NOTE: we cannot replace this function with TYPE_FIND_REGISTER_START_WITH,
 * as it is only possible to register one typefind factory per 'name'
 * (which is in this case the caps), and the first one would be replaced by
 * the second one. */
static GstStaticCaps au_caps = GST_STATIC_CAPS ("audio/x-au");

#define AU_CAPS (gst_static_caps_get(&au_caps))
static void
au_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data) {
    if (memcmp (data, ".snd", 4) == 0 || memcmp (data, "dns.", 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, AU_CAPS);
    }
  }
}

/*** audio/x-paris ***/
/* NOTE: do not replace this function with two TYPE_FIND_REGISTER_START_WITH */
static GstStaticCaps paris_caps = GST_STATIC_CAPS ("audio/x-paris");

#define PARIS_CAPS (gst_static_caps_get(&paris_caps))
static void
paris_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 4);

  if (data) {
    if (memcmp (data, " paf", 4) == 0 || memcmp (data, "fap ", 4) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, PARIS_CAPS);
    }
  }
}

/*** audio/iLBC-sh ***/
/* NOTE: do not replace this function with two TYPE_FIND_REGISTER_START_WITH */
static GstStaticCaps ilbc_caps = GST_STATIC_CAPS ("audio/iLBC-sh");

#define ILBC_CAPS (gst_static_caps_get(&ilbc_caps))
static void
ilbc_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 8);

  if (data) {
    if (memcmp (data, "#!iLBC30", 8) == 0 || memcmp (data, "#!iLBC20", 8) == 0) {
      gst_type_find_suggest (tf, GST_TYPE_FIND_LIKELY, ILBC_CAPS);
    }
  }
}

/*** application/x-ms-dos-executable ***/

static GstStaticCaps msdos_caps =
GST_STATIC_CAPS ("application/x-ms-dos-executable");
#define MSDOS_CAPS (gst_static_caps_get(&msdos_caps))
/* see http://www.madchat.org/vxdevl/papers/winsys/pefile/pefile.htm */
static void
msdos_type_find (GstTypeFind * tf, gpointer unused)
{
  guint8 *data = gst_type_find_peek (tf, 0, 64);

  if (data && data[0] == 'M' && data[1] == 'Z' &&
      GST_READ_UINT16_LE (data + 8) == 4) {
    guint32 pe_offset = GST_READ_UINT32_LE (data + 60);

    data = gst_type_find_peek (tf, pe_offset, 2);
    if (data && data[0] == 'P' && data[1] == 'E') {
      gst_type_find_suggest (tf, GST_TYPE_FIND_NEARLY_CERTAIN, MSDOS_CAPS);
    }
  }
}

/*** generic typefind for streams that have some data at a specific position***/
typedef struct
{
  guint8 *data;
  guint size;
  guint probability;
  GstCaps *caps;
}
GstTypeFindData;

static void
start_with_type_find (GstTypeFind * tf, gpointer private)
{
  GstTypeFindData *start_with = (GstTypeFindData *) private;
  guint8 *data;

  GST_LOG ("trying to find mime type %s with the first %u bytes of data",
      gst_structure_get_name (gst_caps_get_structure (start_with->caps, 0)),
      start_with->size);
  data = gst_type_find_peek (tf, 0, start_with->size);
  if (data && memcmp (data, start_with->data, start_with->size) == 0) {
    gst_type_find_suggest (tf, start_with->probability, start_with->caps);
  }
}

#define TYPE_FIND_REGISTER_START_WITH(plugin,name,rank,ext,_data,_size,_probability)\
G_BEGIN_DECLS{                                                          \
  GstTypeFindData *sw_data = g_new (GstTypeFindData, 1);                \
  sw_data->data = (gpointer)_data;                                      \
  sw_data->size = _size;                                                \
  sw_data->probability = _probability;                                  \
  sw_data->caps = gst_caps_new_simple (name, NULL);                     \
  if (!gst_type_find_register (plugin, name, rank, start_with_type_find,\
                      ext, sw_data->caps, sw_data, (GDestroyNotify) (g_free))) {        \
    gst_caps_unref (sw_data->caps);                                     \
    g_free (sw_data);                                                   \
  }                                                                     \
}G_END_DECLS

/*** same for riff types ***/

static void
riff_type_find (GstTypeFind * tf, gpointer private)
{
  GstTypeFindData *riff_data = (GstTypeFindData *) private;
  guint8 *data = gst_type_find_peek (tf, 0, 12);

  if (data && memcmp (data, "RIFF", 4) == 0) {
    data += 8;
    if (memcmp (data, riff_data->data, 4) == 0)
      gst_type_find_suggest (tf, riff_data->probability, riff_data->caps);
  }
}

#define TYPE_FIND_REGISTER_RIFF(plugin,name,rank,ext,_data)             \
G_BEGIN_DECLS{                                                          \
  GstTypeFindData *sw_data = g_new (GstTypeFindData, 1);                \
  sw_data->data = (gpointer)_data;                                      \
  sw_data->size = 4;                                                    \
  sw_data->probability = GST_TYPE_FIND_MAXIMUM;                         \
  sw_data->caps = gst_caps_new_simple (name, NULL);                     \
  if (!gst_type_find_register (plugin, name, rank, riff_type_find,      \
                      ext, sw_data->caps, sw_data, (GDestroyNotify) (g_free))) {                        \
    gst_caps_unref (sw_data->caps);                                     \
    g_free (sw_data);                                                   \
  }                                                                     \
}G_END_DECLS

/*** plugin initialization ***/

#define TYPE_FIND_REGISTER(plugin,name,rank,func,ext,caps,priv,notify) \
G_BEGIN_DECLS{\
  if (!gst_type_find_register (plugin, name, rank, func, ext, caps, priv, notify))\
    return FALSE; \
}G_END_DECLS
static gboolean
plugin_init (GstPlugin * plugin)
{
  /* can't initialize this via a struct as caps can't be statically initialized */

  /* note: asx/wax/wmx are XML files, asf doesn't handle them */
  static gchar *asf_exts[] = { "asf", "wm", "wma", "wmv", NULL };
  static gchar *au_exts[] = { "au", "snd", NULL };
  static gchar *avi_exts[] = { "avi", NULL };
  static gchar *cdxa_exts[] = { "dat", NULL };
  static gchar *flac_exts[] = { "flac", NULL };
  static gchar *flx_exts[] = { "flc", "fli", NULL };
  static gchar *id3_exts[] =
      { "mp3", "mp2", "mp1", "mpga", "ogg", "flac", "tta", NULL };
  static gchar *apetag_exts[] = { "ape", "mpc", "wv", NULL };   /* and mp3 and wav? */
  static gchar *tta_exts[] = { "tta", NULL };
  static gchar *mod_exts[] = { "669", "amf", "dsm", "gdm", "far", "imf",
    "it", "med", "mod", "mtm", "okt", "sam",
    "s3m", "stm", "stx", "ult", "xm", NULL
  };
  static gchar *mp3_exts[] = { "mp3", "mp2", "mp1", "mpga", NULL };
  static gchar *ac3_exts[] = { "ac3", NULL };
  static gchar *musepack_exts[] = { "mpc", NULL };
  static gchar *mpeg_sys_exts[] = { "mpe", "mpeg", "mpg", NULL };
  static gchar *mpeg_video_exts[] = { "mpv", "mpeg", "mpg", NULL };
  static gchar *ogg_exts[] = { "anx", "ogg", "ogm", NULL };
  static gchar *qt_exts[] = { "mov", NULL };
  static gchar *rm_exts[] = { "ra", "ram", "rm", "rmvb", NULL };
  static gchar *swf_exts[] = { "swf", "swfl", NULL };
  static gchar *utf8_exts[] = { "txt", NULL };
  static gchar *wav_exts[] = { "wav", NULL };
  static gchar *aiff_exts[] = { "aiff", "aif", "aifc", NULL };
  static gchar *svx_exts[] = { "iff", "svx", NULL };
  static gchar *paris_exts[] = { "paf", NULL };
  static gchar *nist_exts[] = { "nist", NULL };
  static gchar *voc_exts[] = { "voc", NULL };
  static gchar *sds_exts[] = { "sds", NULL };
  static gchar *ircam_exts[] = { "sf", NULL };
  static gchar *w64_exts[] = { "w64", NULL };
  static gchar *shn_exts[] = { "shn", NULL };
  static gchar *ape_exts[] = { "ape", NULL };
  static gchar *uri_exts[] = { "ram", NULL };
  static gchar *smil_exts[] = { "smil", NULL };
  static gchar *xml_exts[] = { "xml", NULL };
  static gchar *jpeg_exts[] = { "jpg", "jpe", "jpeg", NULL };
  static gchar *gif_exts[] = { "gif", NULL };
  static gchar *png_exts[] = { "png", NULL };
  static gchar *bmp_exts[] = { "bmp", NULL };
  static gchar *tiff_exts[] = { "tif", "tiff", NULL };
  static gchar *matroska_exts[] = { "mkv", "mka", NULL };
  static gchar *dv_exts[] = { "dv", "dif", NULL };
  static gchar *amr_exts[] = { "amr", NULL };
  static gchar *ilbc_exts[] = { "ilbc", NULL };
  static gchar *sid_exts[] = { "sid", NULL };
  static gchar *xcf_exts[] = { "xcf", NULL };
  static gchar *mng_exts[] = { "mng", NULL };
  static gchar *jng_exts[] = { "jng", NULL };
  static gchar *xpm_exts[] = { "xpm", NULL };
  static gchar *ras_exts[] = { "ras", NULL };
  static gchar *bz2_exts[] = { "bz2", NULL };
  static gchar *gz_exts[] = { "gz", NULL };
  static gchar *zip_exts[] = { "zip", NULL };
  static gchar *compress_exts[] = { "Z", NULL };
  static gchar *m4a_exts[] = { "m4a", NULL };
  static gchar *q3gp_exts[] = { "3gp", NULL };
  static gchar *aac_exts[] = { "aac", NULL };
  static gchar *spc_exts[] = { "spc", NULL };
  static gchar *wavpack_exts[] = { "wv", "wvp", NULL };
  static gchar *wavpack_correction_exts[] = { "wvc", NULL };
  static gchar *rar_exts[] = { "rar", NULL };
  static gchar *tar_exts[] = { "tar", NULL };
  static gchar *ar_exts[] = { "a", NULL };
  static gchar *msdos_exts[] = { "dll", "exe", "ocx", "sys", "scr",
    "msstyles", "cpl", NULL
  };
  static gchar *flv_exts[] = { "flv", NULL };
  static gchar *m4v_exts[] = { "m4v" };

  GST_DEBUG_CATEGORY_INIT (type_find_debug, "typefindfunctions",
      GST_DEBUG_FG_GREEN | GST_DEBUG_BG_RED, "generic type find functions");

  /* must use strings, macros don't accept initializers */
  TYPE_FIND_REGISTER_START_WITH (plugin, "video/x-ms-asf", GST_RANK_SECONDARY,
      asf_exts,
      "\060\046\262\165\216\146\317\021\246\331\000\252\000\142\316\154", 16,
      GST_TYPE_FIND_MAXIMUM);
  /* -1 so id3v1 or apev1/2 are detected with higher preference */
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-musepack", GST_RANK_PRIMARY,
      musepack_exts, "MP+", 3, GST_TYPE_FIND_LIKELY + 10);
  TYPE_FIND_REGISTER (plugin, "audio/x-au", GST_RANK_MARGINAL,
      au_type_find, au_exts, AU_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_RIFF (plugin, "video/x-msvideo", GST_RANK_PRIMARY,
      avi_exts, "AVI ");
  TYPE_FIND_REGISTER_RIFF (plugin, "video/x-cdxa", GST_RANK_PRIMARY,
      cdxa_exts, "CDXA");
  TYPE_FIND_REGISTER_START_WITH (plugin, "video/x-vcd", GST_RANK_PRIMARY,
      cdxa_exts, "\000\377\377\377\377\377\377\377\377\377\377\000", 12,
      GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-flac", GST_RANK_PRIMARY,
      flac_exts, "fLaC", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "video/x-fli", GST_RANK_MARGINAL, flx_type_find,
      flx_exts, FLX_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-id3", GST_RANK_PRIMARY + 2,
      id3_type_find, id3_exts, ID3_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-apetag", GST_RANK_PRIMARY + 1,
      apetag_type_find, apetag_exts, APETAG_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-ttafile", GST_RANK_PRIMARY,
      tta_type_find, tta_exts, TTA_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-mod", GST_RANK_SECONDARY, mod_type_find,
      mod_exts, MOD_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/mpeg", GST_RANK_PRIMARY, mp3_type_find,
      mp3_exts, MP3_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-ac3", GST_RANK_PRIMARY, ac3_type_find,
      ac3_exts, AC3_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/mpeg1", GST_RANK_PRIMARY,
      mpeg1_sys_type_find, mpeg_sys_exts, MPEG_SYS_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/mpeg2", GST_RANK_SECONDARY,
      mpeg2_sys_type_find, mpeg_sys_exts, MPEG_SYS_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/ogg", GST_RANK_PRIMARY,
      ogganx_type_find, ogg_exts, OGGANX_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/mpeg", GST_RANK_SECONDARY,
      mpeg_video_type_find, mpeg_video_exts, MPEG_VIDEO_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/mpeg-stream", GST_RANK_MARGINAL,
      mpeg_video_stream_type_find, mpeg_video_exts, MPEG_VIDEO_CAPS, NULL,
      NULL);
  TYPE_FIND_REGISTER (plugin, "video/mpeg4", GST_RANK_PRIMARY,
      mpeg4_video_type_find, m4v_exts, MPEG_VIDEO_CAPS, NULL, NULL);

  /* ISO formats */
  TYPE_FIND_REGISTER (plugin, "audio/x-m4a", GST_RANK_PRIMARY, m4a_type_find,
      m4a_exts, M4A_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-3gp", GST_RANK_PRIMARY,
      q3gp_type_find, q3gp_exts, Q3GP_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/quicktime", GST_RANK_SECONDARY,
      qt_type_find, qt_exts, QT_CAPS, NULL, NULL);

  TYPE_FIND_REGISTER_START_WITH (plugin, "application/vnd.rn-realmedia",
      GST_RANK_SECONDARY, rm_exts, ".RMF", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "application/x-shockwave-flash",
      GST_RANK_SECONDARY, swf_type_find, swf_exts, SWF_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "video/x-flv", GST_RANK_SECONDARY,
      flv_exts, "FLV", 3, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "text/plain", GST_RANK_MARGINAL, utf8_type_find,
      utf8_exts, UTF8_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "text/uri-list", GST_RANK_MARGINAL, uri_type_find,
      uri_exts, URI_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/smil", GST_RANK_SECONDARY,
      smil_type_find, smil_exts, SMIL_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/xml", GST_RANK_MARGINAL,
      xml_type_find, xml_exts, GENERIC_XML_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_RIFF (plugin, "audio/x-wav", GST_RANK_PRIMARY, wav_exts,
      "WAVE");
  TYPE_FIND_REGISTER (plugin, "audio/x-aiff", GST_RANK_SECONDARY,
      aiff_type_find, aiff_exts, AIFF_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-svx", GST_RANK_SECONDARY, svx_type_find,
      svx_exts, SVX_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-paris", GST_RANK_SECONDARY,
      paris_type_find, paris_exts, PARIS_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-nist", GST_RANK_SECONDARY,
      nist_exts, "NIST", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-voc", GST_RANK_SECONDARY,
      voc_exts, "Creative", 8, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "audio/x-sds", GST_RANK_SECONDARY, sds_type_find,
      sds_exts, SDS_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-ircam", GST_RANK_SECONDARY,
      ircam_type_find, ircam_exts, IRCAM_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-w64", GST_RANK_SECONDARY,
      w64_exts, "riff", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "audio/x-shorten", GST_RANK_SECONDARY,
      shn_type_find, shn_exts, SHN_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ape", GST_RANK_SECONDARY,
      ape_type_find, ape_exts, APE_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "image/jpeg", GST_RANK_PRIMARY, jpeg_type_find,
      jpeg_exts, JPEG_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "image/gif", GST_RANK_PRIMARY,
      gif_exts, "GIF8", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "image/png", GST_RANK_PRIMARY,
      png_exts, "\211PNG\015\012\032\012", 8, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "image/bmp", GST_RANK_PRIMARY, bmp_type_find,
      bmp_exts, BMP_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "image/tiff", GST_RANK_PRIMARY, tiff_type_find,
      tiff_exts, TIFF_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/x-matroska", GST_RANK_PRIMARY,
      matroska_type_find, matroska_exts, MATROSKA_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/x-dv", GST_RANK_SECONDARY, dv_type_find,
      dv_exts, DV_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-amr-nb-sh", GST_RANK_PRIMARY,
      amr_exts, "#!AMR", 5, GST_TYPE_FIND_LIKELY);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-amr-wb-sh", GST_RANK_PRIMARY,
      amr_exts, "#!AMR-WB", 7, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "audio/iLBC-sh", GST_RANK_PRIMARY,
      ilbc_type_find, ilbc_exts, ILBC_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-sid", GST_RANK_MARGINAL,
      sid_exts, "PSID", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "image/x-xcf", GST_RANK_SECONDARY,
      xcf_exts, "gimp xcf", 8, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "video/x-mng", GST_RANK_SECONDARY,
      mng_exts, "\212MNG\015\012\032\012", 8, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "image/x-jng", GST_RANK_SECONDARY,
      jng_exts, "\213JNG\015\012\032\012", 8, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "image/x-xpixmap", GST_RANK_SECONDARY,
      xpm_exts, "/* XPM */", 9, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "image/x-sun-raster",
      GST_RANK_SECONDARY, ras_exts, "\131\246\152\225", 4,
      GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER_START_WITH (plugin, "application/x-bzip",
      GST_RANK_SECONDARY, bz2_exts, "BZh", 3, GST_TYPE_FIND_LIKELY);
  TYPE_FIND_REGISTER_START_WITH (plugin, "application/x-gzip",
      GST_RANK_SECONDARY, gz_exts, "\037\213", 2, GST_TYPE_FIND_LIKELY);
  TYPE_FIND_REGISTER_START_WITH (plugin, "application/zip", GST_RANK_SECONDARY,
      zip_exts, "PK\003\004", 4, GST_TYPE_FIND_LIKELY);
  TYPE_FIND_REGISTER_START_WITH (plugin, "application/x-compress",
      GST_RANK_SECONDARY, compress_exts, "\037\235", 2, GST_TYPE_FIND_LIKELY);
  TYPE_FIND_REGISTER (plugin, "audio/x-vorbis", GST_RANK_PRIMARY,
      vorbis_type_find, NULL, VORBIS_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "video/x-theora", GST_RANK_PRIMARY,
      theora_type_find, NULL, THEORA_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ogm-video", GST_RANK_PRIMARY,
      ogmvideo_type_find, NULL, OGMVIDEO_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ogm-audio", GST_RANK_PRIMARY,
      ogmaudio_type_find, NULL, OGMAUDIO_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ogm-text", GST_RANK_PRIMARY,
      ogmtext_type_find, NULL, OGMTEXT_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-speex", GST_RANK_PRIMARY,
      speex_type_find, NULL, SPEEX_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ogg-skeleton", GST_RANK_PRIMARY,
      oggskel_type_find, NULL, OGG_SKELETON_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "text/x-cmml", GST_RANK_PRIMARY, cmml_type_find,
      NULL, CMML_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "application/x-executable",
      GST_RANK_MARGINAL, NULL, "\177ELF", 4, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "adts_mpeg_stream", GST_RANK_SECONDARY,
      aac_type_find, aac_exts, AAC_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "audio/x-spc", GST_RANK_SECONDARY,
      spc_exts, "SNES-SPC700 Sound File Data", 27, GST_TYPE_FIND_MAXIMUM);
  TYPE_FIND_REGISTER (plugin, "audio/x-wavpack", GST_RANK_SECONDARY,
      wavpack_type_find, wavpack_exts, WAVPACK_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "audio/x-wavpack-correction", GST_RANK_SECONDARY,
      wavpack_type_find, wavpack_correction_exts, WAVPACK_CORRECTION_CAPS, NULL,
      NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "application/x-rar",
      GST_RANK_SECONDARY, rar_exts, "Rar!", 4, GST_TYPE_FIND_LIKELY);
  TYPE_FIND_REGISTER (plugin, "application/x-tar", GST_RANK_SECONDARY,
      tar_type_find, tar_exts, TAR_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ar", GST_RANK_SECONDARY,
      ar_type_find, ar_exts, AR_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER (plugin, "application/x-ms-dos-executable",
      GST_RANK_SECONDARY, msdos_type_find, msdos_exts, MSDOS_CAPS, NULL, NULL);
  TYPE_FIND_REGISTER_START_WITH (plugin, "video/x-dirac",
      GST_RANK_PRIMARY, NULL, "BBCD", 4, GST_TYPE_FIND_LIKELY);

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "typefindfunctions",
    "default typefind functions",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
