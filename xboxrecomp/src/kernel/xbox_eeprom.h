/* The EEPROM's user-settings block, as ExQueryNonVolatileSetting returns it
 * for XC_MAX_OS (0xFF), and the whole 256-byte EEPROM for XC_MAX_ALL
 * (0xFFFF) with that block at 0x60 (layout as Cxbx-Reloaded's
 * XBOX_USER_SETTINGS and xboxdevwiki's EEPROM page).
 *
 * Conker: Live & Reloaded reads the block (0xFF) instead of XC_VIDEO alone;
 * answered with zeros it saw no 16:9 flag and rendered 4:3.
 *
 * Header-only, no kernel types: tools/recomp/test_eeprom_user.py compiles it
 * on its own. */
#ifndef XBOX_EEPROM_H
#define XBOX_EEPROM_H

#include <stdint.h>
#include <string.h>

#define XBOX_EEPROM_USER_OFFSET  0x60u  /* in the 256-byte EEPROM */
#define XBOX_EEPROM_USER_SIZE    0x60u
#define XBOX_EEPROM_SIZE         0x100u

/* Offsets inside the user-settings block. */
#define XBOX_US_TZ_BIAS     0x04u
#define XBOX_US_LANGUAGE    0x30u
#define XBOX_US_VIDEO       0x34u
#define XBOX_US_AUDIO       0x38u
#define XBOX_US_DVD_REGION  0x5Cu
/* The factory section's video standard (EEPROM 0x58): NTSC-M. */
#define XBOX_EEPROM_VIDEO_STANDARD      0x58u
#define XBOX_VIDEO_STANDARD_NTSC_M      0x00400100u

/* XC_LANGUAGE (XDK): 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish,
 * 6 Italian, 7 Korean, 8 Chinese, 9 Portuguese. A number, or a name in any
 * case ("french", "German"); anything else is English. RECOMP_LANGUAGE. */
static inline uint32_t xbox_eeprom_language(const char *s)
{
    static const char *const names[] = { "english", "japanese", "german", "french",
                                         "spanish", "italian", "korean", "chinese",
                                         "portuguese" };
    uint32_t i;
    if (!s || !*s)
        return 1;
    if (*s >= '1' && *s <= '9' && !s[1])
        return (uint32_t)(*s - '0');
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *a = names[i], *b = s;
        while (*a && *b && *a == (*b | 0x20)) a++, b++;
        if (!*a && !*b)
            return i + 1;
    }
    return 1;
}

static inline void xbox_eeprom_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* The user-settings block (XBOX_EEPROM_USER_SIZE bytes) with the values
 * ExQueryNonVolatileSetting gives one by one. The checksum stays 0: XAPI
 * does not check it on reads. */
static inline void xbox_eeprom_user_settings(uint8_t *us, uint32_t language,
                                             uint32_t video_flags, uint32_t audio_flags,
                                             int32_t tz_bias, uint32_t dvd_region)
{
    memset(us, 0, XBOX_EEPROM_USER_SIZE);
    xbox_eeprom_put32(us + XBOX_US_TZ_BIAS, (uint32_t)tz_bias);
    xbox_eeprom_put32(us + XBOX_US_LANGUAGE, language);
    xbox_eeprom_put32(us + XBOX_US_VIDEO, video_flags);
    xbox_eeprom_put32(us + XBOX_US_AUDIO, audio_flags);
    xbox_eeprom_put32(us + XBOX_US_DVD_REGION, dvd_region);
}

/* Fills `out` (`len` bytes) for index 0xFF (the block) or 0xFFFF (the whole
 * EEPROM); returns the bytes that make up the answer. */
static inline uint32_t xbox_eeprom_query(uint32_t index, uint8_t *out, uint32_t len,
                                         uint32_t language, uint32_t video_flags,
                                         uint32_t audio_flags, int32_t tz_bias,
                                         uint32_t dvd_region)
{
    uint8_t all[XBOX_EEPROM_SIZE];
    const uint8_t *src;
    uint32_t n;

    memset(all, 0, sizeof all);
    xbox_eeprom_put32(all + XBOX_EEPROM_VIDEO_STANDARD, XBOX_VIDEO_STANDARD_NTSC_M);
    xbox_eeprom_user_settings(all + XBOX_EEPROM_USER_OFFSET, language, video_flags,
                              audio_flags, tz_bias, dvd_region);
    if (index == 0xFFFFu) {
        src = all;
        n = XBOX_EEPROM_SIZE;
    } else {
        src = all + XBOX_EEPROM_USER_OFFSET;
        n = XBOX_EEPROM_USER_SIZE;
    }
    memset(out, 0, len);
    memcpy(out, src, len < n ? len : n);
    return n;
}

#endif /* XBOX_EEPROM_H */
