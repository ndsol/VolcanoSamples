/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroApp implementation.
 */

#include <ctype.h>
#include <errno.h>

#include "include/libretro.h"
#include "retroweb.h"

static std::string getHeader(std::vector<char>& hdr, size_t maxHdrLen,
                             const std::string& filename, off_t filesize) {
  if (filesize < (off_t)maxHdrLen) {
    hdr.resize(filesize);
  } else {
    hdr.resize(maxHdrLen);
  }
  FILE* f = fopen(filename.c_str(), "rb");
  if (!f) {
    char e[256];
    snprintf(e, sizeof(e), "(unable to open: %d %s)", errno, strerror(errno));
    return e;
  }
  if (fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) {
    char e[256];
    snprintf(e, sizeof(e), "(unable to read: %d %s)", errno, strerror(errno));
    fclose(f);
    return e;
  }
  fclose(f);
  return "";
}

static std::string getExtension(const std::string& filename) {
  auto pos = filename.rfind(".");
  if (pos == std::string::npos) {
    return "(file has no extension)";
  }
  std::string r = filename.substr(pos + 1);
  for (size_t i = 0; i < r.size(); i++) {
    r[i] = tolower(r[i]);
  }
  return r;
}

static bool isAtari2600(const std::string& filename, off_t filesize) {
  return getExtension(filename) == "a26" && filesize == 4096;
}

static bool isNES(const std::vector<char>& hdr) {
  if (hdr.size() < 16 || strncmp(hdr.data(), "NES\x1a", 4)) {
    return false;  // See https://wiki.nesdev.com/w/index.php/NES_2.0
  }
  return true;  // Detect NES2.0 if ((hdr.at(7) & 0x0C) == 0x08)
}

static bool isUNIF(const std::vector<char>& hdr) {
  if (hdr.size() < 16 || strncmp(hdr.data(), "UNIF", 4)) {
    return false;  // See https://wiki.nesdev.com/w/index.php/UNIF
  }
  for (size_t i = 8; i < 32 && i < hdr.size(); i++) {
    if (hdr.at(0) != 0) {
      return false;
    }
  }
  return true;
}

static bool isFDS(const std::vector<char>& hdr) {
  if (hdr.size() < 16 || strncmp(hdr.data(), "FDS\x1a", 4)) {
    return false;  // See https://wiki.nesdev.com/w/index.php/FDS_file_format
  }
  for (size_t i = 5; i < 16; i++) {
    if (hdr.at(0) != 0) {
      return false;
    }
  }
  return true;
}

// isSuperNESheader takes an assumed 'loROM' value and tries to make sense of
// the rom bytes. Try both values and go with the one that doesn't fail.
// See https://en.wikibooks.org/wiki/Super_NES_Programming/SNES_memory_map
static bool isSuperNESheader(const std::vector<char>& rom, bool hiROM,
                             bool debug, size_t ignoreHeader) {
  size_t ofs = ignoreHeader + 0x7fc0;
  const char* romStr = "loROM";
  uint8_t romType = (uint8_t)rom.at(ofs + 0x15);
  if (hiROM) {
    ofs += 0x8000;
    romStr = "hiROM";
    switch (romType) {
      case 0x21:
      case 0x23:
      case 0x31:
      case 0x35:
        break;
      default:
        if (debug) logW("%s: unknown romType %02x\n", romStr, romType);
        return false;
    }
  } else {
    switch (romType) {
      case 0x20:
      case 0x30:
      case 0x32:
        break;
      default:
        if (debug) logW("%s: unknown romType %02x\n", romStr, romType);
        return false;
    }
  }
  if (rom.size() < ofs + 0x40) {
    if (debug) logW("%s: rom cut off before any header\n", romStr);
    return false;
  }
  if ((romType & 1) != (int)hiROM) {
    if (debug)
      logW("%s: rom lo/hi got %d want %d from type %02x\n", romStr, romType & 1,
           (int)hiROM, romType);
    return false;
  }
  size_t i;
  for (i = 0; i < 0x15; i++) {
    if (rom.at(ofs + i) < 0x20 || rom.at(ofs + i) > 0x7e) {
      if (debug) {
        logW("%s: title invalid at %zu (want %d)\n", romStr, i, 0x15);
        logW("%s: got title \"%.*s\"\n", romStr, (int)i, rom.data() + ofs);
      }
      return false;
    }
  }
  // 0x1c,0x1d is checksum complemented and 0x1e,0x1f is checksum, but
  // the algorithm used is not uniform. See https://github.com/snes9xgit
  unsigned sum1 = ((unsigned)rom.at(ofs + 0x1c) & 0xff) |
                  ((unsigned)rom.at(ofs + 0x1d) << 8);
  unsigned sum2 = ((unsigned)rom.at(ofs + 0x1e) & 0xff) |
                  ((unsigned)rom.at(ofs + 0x1f) << 8);
  if (((sum1 ^ sum2) & 0xffff) != 0xffff) {
    if (debug)
      logW("%s: sum1 = %04x sum2 = %04x not complements: %04x\n", romStr, sum1,
           sum2, (sum1 ^ sum2) & 0xffff);
    return false;
  }
  return true;
}

static bool isSuperNES(const std::string& filename, off_t filesize, off_t h,
                       bool debug) {
  std::vector<char> rom;
  std::string err = getHeader(rom, 0x10000 + h, filename, filesize);
  if (!err.empty()) {
    if (debug) logW("isSuperNES: %s\n", err.c_str());
    return false;
  }
  if (h) {
    if (h != 512) {
      logE("isSuperNES(%s): unknown header\n", filename.c_str());
      return false;
    }
    // Parse SMC header
    if ((off_t)rom.size() < h) {
      if (debug) logW("isSuperNES(%s): missing SMC hdr\n", filename.c_str());
      return false;
    }
    unsigned romSize1 = ((unsigned)rom.at(0)) & 0xff;
    if (romSize1 != ((filesize / 8192) & 0xff)) {
      if (debug)
        logW("isSuperNES(%s): romSize1 got %x want %x\n", filename.c_str(),
             romSize1, (unsigned)(filesize / 8192) & 0xff);
      return false;
    }
    unsigned romSize2 = ((unsigned)rom.at(1)) & 0xff;
    if (romSize2 != ((filesize >> 8) & 0xff)) {
      if (debug)
        logW("isSuperNES(%s): romSize2 got %x want %x\n", filename.c_str(),
             romSize2, (unsigned)(filesize >> 8) & 0xff);
      return false;
    }
    for (size_t i = 3; i < 512; i++) {
      if (rom.at(i)) {
        if (debug)
          logW("isSuperNES(%s): bad SMC header at %zu\n", filename.c_str(), i);
        return false;
      }
    }
  }
  if (debug) logW("isSuperNES(%s): try loROM:\n", filename.c_str());
  if (isSuperNESheader(rom, false, debug, h)) {
    if (debug) logW("isSuperNES: loROM worked\n");
    return true;
  }
  if (isSuperNESheader(rom, true, debug, h)) {
    if (debug) logW("isSuperNES: hiROM worked\n");
    return true;
  }
  if (debug) logW("isSuperNES(%s): no match, fail.\n", filename.c_str());
  return false;
}

static bool isSFC(const std::string& filename, off_t filesize) {
  // SFC has no 512-byte header. SMC does.
  return ((filesize % 1024) == 0) && isSuperNES(filename, filesize, 0, false);
}

// isSMC does not look at the filename. SMC, SWC, FIG are identical.
static bool isSMC(const std::string& filename, off_t filesize) {
  // SFC has no 512-byte header. SMC does.
  return ((filesize % 1024) == 512) &&
         isSuperNES(filename, filesize, 512, false);
}

std::string RetroWeb::getFileType(const std::string filename, off_t filesize) {
  if (filesize < 4) {
    return "(file too small - corrupt?)";
  }

  std::vector<char> hdr;
  std::string t = getHeader(hdr, 16, filename, filesize);
  if (!t.empty()) return t;
  // Atari-2600
  else if (isAtari2600(filename, filesize))
    return "a26";
  // Nintendo Entertainment System / Famicom Family Computer Disk System.
  else if (isNES(hdr))
    return "nes";
  else if (isUNIF(hdr))
    return "unif";
  else if (isFDS(hdr))
    return "fds";
  // Super Nintendo Entertainment System
  else if (isSFC(filename, filesize))
    return "sfc";
  else if (isSMC(filename, filesize))
    return "smc";

  // Fall back to using file extension, but log a warning.
  t = getExtension(filename);
  if (t == "nes" || t == "sfc" || t == "smc") {
    logW("WARNING: %s file but magic not recognized\n", t.c_str());
  } else if (t == "unf" || t == "unif") {
    logW("WARNING: %s file but magic not recognized\n", "unif");
    return "unif";  // change any "unf" to "unif"
  } else {
    logW("TODO: detect magic for \"%s\", try %s for now\n", filename.c_str(),
         t.c_str());
  }
  return t;
}

static std::map<std::string, std::string> overrides{};

RetroVar::RetroVar(const struct retro_variable* def) : key(def->key) {
  const char* p = strchr(def->value, ';');
  if (!p || p[1] != ' ') {
    logW("invalid set_var: \"%s\" = \"%s\"\n", def->key, def->value);
    desc = "invalid: ";
    desc += def->value;
    choice.emplace_back(def->value);
    return;
  }
  desc.assign(def->value, p - def->value);
  p += 2;  // consume ';' and ' '
  for (;; p++) {
    size_t n = strcspn(p, "|");
    choice.emplace_back(std::string(p, n));
    p += n;
    if (*p != '|') {
      break;
    }
  }

  auto i = overrides.find(key);
  if (i != overrides.end()) {
    size_t j = 0;
    for (; j < choice.size(); j++) {
      if (choice.at(j) == i->second) {
        setCurrent(j);
        break;
      }
    }
    if (j >= choice.size()) {
      logW("override \"%s\" to \"%s\": not in choices\n", i->first.c_str(),
           i->second.c_str());
    }
  }
}
