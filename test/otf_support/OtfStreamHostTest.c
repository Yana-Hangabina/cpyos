// Streaming-read pressure test for CFF faces, mirroring the firmware's
// SdCardFontSystem::prefixRead + TtfSource::bytes pattern: the first
// `prefix` bytes are served from a RAM cache, anything past it (and reads
// straddling the boundary) comes from a file read. FreeType is handed a
// custom FT_Stream exactly like FtFont's streamed face, so CFF's
// end-of-file tables (CharStrings / charset / FDArray) exercise the
// split-read path the same way the device does.
//
// usage: OtfStreamHostTest <font> <prefix-bytes>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned char* prefix;  // RAM-cached head of the file
  unsigned long prefixLen;
  unsigned char* full;  // fallback file contents (stands in for the SD read)
  unsigned long fullLen;
  unsigned long seeks;   // diagnostic counters
  unsigned long tailReads;
} Split;

static unsigned long splitRead(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
  Split* s = (Split*)stream->descriptor.pointer;
  if (count == 0) return 0;
  if (offset >= s->fullLen) return 0;
  if (offset < s->prefixLen) {
    const unsigned long fromCache = (offset + count <= s->prefixLen) ? count : s->prefixLen - offset;
    memcpy(buffer, s->prefix + offset, fromCache);
    if (fromCache == count) return count;
    const unsigned long rest = count - fromCache;
    const unsigned long tailOff = s->prefixLen;
    const unsigned long avail = s->fullLen - tailOff;
    const unsigned long take = rest < avail ? rest : avail;
    memcpy(buffer + fromCache, s->full + tailOff, take);
    s->tailReads++;
    return fromCache + take;
  }
  const unsigned long avail = s->fullLen - offset;
  const unsigned long take = count < avail ? count : avail;
  memcpy(buffer, s->full + offset, take);
  s->tailReads++;
  return take;
}

static void splitClose(FT_Stream stream) { (void)stream; }

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <font> <prefix-bytes>\n", argv[0]);
    return 2;
  }
  const unsigned long prefix = strtoul(argv[2], NULL, 10);

  FILE* fp = fopen(argv[1], "rb");
  if (!fp) return 2;
  fseek(fp, 0, SEEK_END);
  const unsigned long size = (unsigned long)ftell(fp);
  fseek(fp, 0, SEEK_SET);
  unsigned char* full = (unsigned char*)malloc(size);
  if (fread(full, 1, size, fp) != size) return 2;
  fclose(fp);

  const unsigned long prefixLen = prefix < size ? prefix : size;
  Split split = {full, prefixLen, full, size, 0, 0};

  FT_Library lib;
  if (FT_Init_FreeType(&lib)) return 1;
  FT_Add_Default_Modules(lib);

  FT_Stream stream = (FT_Stream)calloc(1, sizeof(*stream));
  stream->read = splitRead;
  stream->close = splitClose;
  stream->descriptor.pointer = &split;
  stream->size = size;

  FT_Open_Args args;
  memset(&args, 0, sizeof(args));
  args.flags = FT_OPEN_STREAM;
  args.stream = stream;

  FT_Face face;
  if (FT_Open_Face(lib, &args, 0, &face)) {
    fprintf(stderr, "FAIL: FT_Open_Face via split stream\n");
    return 1;
  }
  printf("face: family=%s glyphs=%ld prefix=%luB/%luB mode=%s\n", face->family_name, face->num_glyphs, prefixLen, size,
         prefixLen < size ? "split" : "fully-cached");

  FT_Set_Pixel_Sizes(face, 0, 28);
  const unsigned long cps[] = {'A', 'g', '0', 0x4F60, 0x597D, 0x6C38, 0x548C};
  unsigned checked = 0;
  for (unsigned i = 0; i < sizeof(cps) / sizeof(cps[0]); ++i) {
    const FT_UInt gi = FT_Get_Char_Index(face, cps[i]);
    if (!gi) continue;
    if (FT_Load_Glyph(face, gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT) ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
      fprintf(stderr, "FAIL: rasterize U+%04lX via split stream\n", cps[i]);
      return 1;
    }
    unsigned covered = 0;
    for (unsigned y = 0; y < (unsigned)face->glyph->bitmap.rows; ++y)
      for (unsigned x = 0; x < (unsigned)face->glyph->bitmap.width; ++x)
        if (face->glyph->bitmap.buffer[y * face->glyph->bitmap.pitch + x]) ++covered;
    if (!covered) {
      fprintf(stderr, "FAIL: U+%04lX empty via split stream\n", cps[i]);
      return 1;
    }
    ++checked;
  }
  if (checked < 5) {
    fprintf(stderr, "FAIL: too few glyphs checked (%u)\n", checked);
    return 1;
  }
  printf("checked=%u tailReads=%lu\n", checked, split.tailReads);
  if (prefixLen < size && split.tailReads == 0) {
    fprintf(stderr, "FAIL: split mode never hit the tail — test is not exercising the boundary\n");
    return 1;
  }
  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  free(stream);
  free(full);
  printf("PASS split-stream\n");
  return 0;
}
