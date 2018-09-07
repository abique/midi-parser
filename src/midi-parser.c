#include <assert.h>
#include <math.h>

#include <midi-parser.h>

const char *
midi_file_format_name(int fmt)
{
  switch (fmt) {
  case MIDI_FILE_FORMAT_SINGLE_TRACK: return "single track";
  case MIDI_FILE_FORMAT_MULTIPLE_TRACKS: return "multiple tracks";
  case MIDI_FILE_FORMAT_MULTIPLE_SONGS: return "multiple songs";

  default: return "(unknown)";
  }
}

const char *
midi_status_name(int status)
{
  switch (status) {
  case MIDI_STATUS_NOTE_OFF: return "Note Off";
  case MIDI_STATUS_NOTE_ON: return "Note On";
  case MIDI_STATUS_NOTE_AT: return "Note Aftertouch";
  case MIDI_STATUS_CC: return "CC";
  case MIDI_STATUS_PGM_CHANGE: return "Program Change";
  case MIDI_STATUS_CHANNEL_AT: return "Channel Aftertouch";
  case MIDI_STATUS_PITCH_BEND: return "Pitch Bend";

  default: return "(unknown)";
  }
}

const char *
midi_meta_name(int type)
{
  switch (type) {
  case MIDI_META_SEQ_NUM: return "Sequence Number";
  case MIDI_META_TEXT: return "Text";
  case MIDI_META_COPYRIGHT: return "Copyright";
  case MIDI_META_TRACK_NAME: return "Track Name";
  case MIDI_META_INSTRUMENT_NAME: return "Instrument Name";
  case MIDI_META_LYRICS: return "Lyrics";
  case MIDI_META_MAKER: return "Maker";
  case MIDI_META_CUE_POINT: return "Cue Point";
  case MIDI_META_CHANNEL_PREFIX: return "Channel Prefix";
  case MIDI_META_END_OF_TRACK: return "End of Track";
  case MIDI_META_SET_TEMPO: return "Set Tempo";
  case MIDI_META_SMPTE_OFFSET: return "SMPTE Offset";
  case MIDI_META_TIME_SIGNATURE: return "Time Signature";
  case MIDI_META_KEY_SIGNATURE: return "Key Signature";
  case MIDI_META_SEQ_SPECIFIC: return "Sequencer Specific";

  default: return "(unknown)";
  }
}

static inline uint16_t
midi_parse_be16(const uint8_t *in)
{
  return (in[0] << 8) | in[1];
}

static inline uint32_t
midi_parse_be32(const uint8_t *in)
{
  return (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];
}

static inline uint64_t
midi_parse_variable_length(struct midi_parser *parser, int32_t *offset)
{
  uint64_t value = 0;
  int32_t  i     = *offset;

  for (; i < parser->size; ++i) {
    value = (value << 7) | (parser->in[i] & 0x7f);
    if (!(parser->in[i] & 0x80))
      break;
  }
  *offset = i + 1;
  return value;
}

static inline enum midi_parser_status
midi_parse_header(struct midi_parser *parser)
{
  if (parser->size < 14)
    return MIDI_PARSER_EOB;

  if (memcmp(parser->in, "MThd", 4))
    return MIDI_PARSER_ERROR;

  parser->header.size          = midi_parse_be32(parser->in + 4);
  parser->header.format        = midi_parse_be16(parser->in + 8);
  parser->header.tracks_count  = midi_parse_be16(parser->in + 10);
  parser->header.time_division = midi_parse_be16(parser->in + 12);

  parser->in   += 14;
  parser->size -= 14;
  parser->state = MIDI_PARSER_HEADER;
  return MIDI_PARSER_HEADER;
}

static inline enum midi_parser_status
midi_parse_track(struct midi_parser *parser)
{
  if (parser->size < 8)
    return MIDI_PARSER_EOB;

  parser->track.size  = midi_parse_be32(parser->in + 4);
  parser->state       = MIDI_PARSER_TRACK;
  parser->in         += 8;
  parser->size       -= 8;
  return MIDI_PARSER_TRACK;
}

static inline bool
midi_parse_vtime(struct midi_parser *parser)
{
  uint8_t nbytes = 0;
  uint8_t cont   = 1; // continue flag

  parser->vtime = 0;
  while (cont) {
    ++nbytes;

    if (parser->size < nbytes)
      return false;

    uint8_t b = parser->in[nbytes - 1];
    parser->vtime = (parser->vtime << 7) | (b & 0x7f);

    cont = b & 0x80;
  }

  parser->in += nbytes;
  parser->size -= nbytes;
  parser->track.size -= nbytes;

  return true;
}

static inline enum midi_parser_status
midi_parse_channel_event(struct midi_parser *parser)
{
  if (parser->size < 3)
    return MIDI_PARSER_EOB;

  parser->midi.status  = parser->in[0] >> 4;
  parser->midi.channel = parser->in[0] & 0xf;
  parser->midi.param1  = parser->in[1];
  parser->midi.param2  = parser->in[2];

  parser->in         += 3;
  parser->size       -= 3;
  parser->track.size -= 3;

  return MIDI_PARSER_TRACK_MIDI;
}

static inline enum midi_parser_status
midi_parse_meta_event(struct midi_parser *parser)
{
  assert(parser->in[0] == 0xff);

  if (parser->size < 2)
    return MIDI_PARSER_EOB;

  parser->meta.type = parser->in[1];
  int32_t offset   = 2;
  parser->meta.length = midi_parse_variable_length(parser, &offset);

  // length should never be negative
  if (parser->meta.length < 0)
    return MIDI_PARSER_ERROR;

  // check buffer size
  if (parser->size < offset + parser->meta.length)
    return MIDI_PARSER_EOB;

  offset += parser->meta.length;
  parser->in += offset;
  parser->size -= offset;
  parser->track.size -= offset;
  return MIDI_PARSER_TRACK_META;
}

static inline enum midi_parser_status
midi_parse_event(struct midi_parser *parser)
{
  if (!midi_parse_vtime(parser))
    return MIDI_PARSER_EOB;

  if (parser->in[0] < 0xf0)
    return midi_parse_channel_event(parser);
  if (parser->in[0] == 0xff)
    return midi_parse_meta_event(parser);
  return MIDI_PARSER_ERROR;
}

enum midi_parser_status
midi_parse(struct midi_parser *parser)
{
  if (!parser->in || parser->size < 1)
    return MIDI_PARSER_EOB;

  switch (parser->state) {
  case MIDI_PARSER_INIT:
    return midi_parse_header(parser);

  case MIDI_PARSER_HEADER:
    return midi_parse_track(parser);

  case MIDI_PARSER_TRACK:
    if (parser->track.size == 0) {
      // we reached the end of the track
      parser->state = MIDI_PARSER_HEADER;
      return midi_parse(parser);
    }
    return midi_parse_event(parser);

  default:
    return MIDI_PARSER_ERROR;
  }
}
