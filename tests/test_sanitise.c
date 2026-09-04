// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- UTF-8 name sanitiser tests.
//
// These exist because of a real bug: an earlier byte-wise copy clipped a
// multi-byte character in half when it hit the output limit, libnx's
// utf8_to_utf16 then rejected the whole string, and the MTP object came out
// with an empty name -- which a file manager shows as an unnamed folder.

#include <string.h>

#include "nexus_test.h"

// Mirrors the validity rule libnx's utf8_to_utf16 applies: any truncated or
// malformed sequence makes the whole string unusable.
static bool is_valid_utf8(const char *s)
{
    for (size_t i = 0; s[i] != '\0'; ) {
        const size_t seq = nexusUtf8SeqLen((unsigned char)s[i]);
        if (seq == 0) return false;

        for (size_t k = 1; k < seq; k++) {
            if (s[i + k] == '\0') return false;
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        }
        i += seq;
    }
    return true;
}

static void t_plain_ascii(void)
{
    char out[64];
    nexusSanitiseUtf8("Super Mario Odyssey", out, sizeof(out));
    CHECK_STR(out, "Super Mario Odyssey");
}

static void t_strips_forbidden(void)
{
    char out[64];
    nexusSanitiseUtf8("a/b\\c:d*e?f\"g<h>i|j", out, sizeof(out));
    CHECK_STR(out, "abcdefghij");

    // Control characters go too.
    nexusSanitiseUtf8("x\ty\nz", out, sizeof(out));
    CHECK_STR(out, "xyz");
}

static void t_trailing_space_and_dot(void)
{
    // Windows rejects names ending in a space or a dot.
    char out[64];
    nexusSanitiseUtf8("Name   ", out, sizeof(out));
    CHECK_STR(out, "Name");

    nexusSanitiseUtf8("Name...", out, sizeof(out));
    CHECK_STR(out, "Name");

    nexusSanitiseUtf8("Name. . ", out, sizeof(out));
    CHECK_STR(out, "Name");
}

static void t_multibyte_survives(void)
{
    char out[64];

    // "Pokémon" -- é is two bytes.
    nexusSanitiseUtf8("Pok\xc3\xa9mon", out, sizeof(out));
    CHECK_STR(out, "Pok\xc3\xa9mon");
    CHECK(is_valid_utf8(out), "output must stay valid UTF-8");

    // Three-byte (CJK) and four-byte (emoji) sequences.
    nexusSanitiseUtf8("\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0", out, sizeof(out));
    CHECK_STR(out, "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0");
    CHECK(is_valid_utf8(out), "CJK must survive");

    nexusSanitiseUtf8("hi \xf0\x9f\x98\x80", out, sizeof(out));
    CHECK(is_valid_utf8(out), "emoji must survive");
}

static void t_never_splits_a_character(void)
{
    // THE REGRESSION: truncating at every possible buffer size must always
    // yield valid UTF-8. A byte-wise copy fails this for most sizes.
    static const char *inputs[] = {
        "Pok\xc3\xa9mon Legends Arceus",
        "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0 \xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x88\xe3\x83\xab",
        "emoji \xf0\x9f\x98\x80\xf0\x9f\x8e\xae end",
        "mixed \xc3\xa9 \xe3\x82\xb2 \xf0\x9f\x98\x80 tail",
    };

    for (size_t s = 0; s < sizeof(inputs) / sizeof(inputs[0]); s++) {
        for (size_t cap = 1; cap <= 40; cap++) {
            char out[64];
            memset(out, 0x7F, sizeof(out));
            nexusSanitiseUtf8(inputs[s], out, cap);

            CHECK(strlen(out) < cap,
                  "input %zu cap %zu: wrote %zu bytes into %zu",
                  s, cap, strlen(out), cap);
            CHECK(is_valid_utf8(out),
                  "input %zu cap %zu: produced invalid UTF-8", s, cap);
        }
    }
}

static void t_rejects_invalid_input(void)
{
    char out[64];

    // A lone continuation byte is dropped rather than passed through.
    nexusSanitiseUtf8("ab\x80\x80" "cd", out, sizeof(out));
    CHECK_STR(out, "abcd");
    CHECK(is_valid_utf8(out), "must be valid");

    // A lead byte with its continuation missing ends the copy cleanly.
    nexusSanitiseUtf8("ab\xc3", out, sizeof(out));
    CHECK_STR(out, "ab");
    CHECK(is_valid_utf8(out), "must be valid");

    // A lead byte followed by a non-continuation byte.
    nexusSanitiseUtf8("ab\xc3zz", out, sizeof(out));
    CHECK(is_valid_utf8(out), "must be valid");
}

static void t_degenerate_buffers(void)
{
    char out[8];

    // Capacity 1 leaves room only for the terminator.
    nexusSanitiseUtf8("anything", out, 1);
    CHECK_STR(out, "");

    // A zero capacity must not be written to at all.
    memset(out, 0x5A, sizeof(out));
    nexusSanitiseUtf8("anything", out, 0);
    CHECK(out[0] == 0x5A, "a zero-capacity buffer must be left alone");

    nexusSanitiseUtf8(NULL, out, sizeof(out));
    CHECK_STR(out, "");
}

static void t_all_stripped_gives_empty(void)
{
    // A name made entirely of forbidden characters yields an empty string,
    // which callers detect and replace with a fallback rather than shipping.
    char out[32];
    nexusSanitiseUtf8("///???", out, sizeof(out));
    CHECK_STR(out, "");
}

static void t_seq_len(void)
{
    CHECK_U64(nexusUtf8SeqLen('A'), 1);
    CHECK_U64(nexusUtf8SeqLen(0xC3), 2);
    CHECK_U64(nexusUtf8SeqLen(0xE3), 3);
    CHECK_U64(nexusUtf8SeqLen(0xF0), 4);
    CHECK_U64(nexusUtf8SeqLen(0x80), 0);   // continuation byte
    CHECK_U64(nexusUtf8SeqLen(0xFF), 0);   // never valid
}

void test_sanitise(void)
{
    nexusTestRun("plain ascii",            t_plain_ascii);
    nexusTestRun("strips forbidden chars", t_strips_forbidden);
    nexusTestRun("trims trailing . and ",  t_trailing_space_and_dot);
    nexusTestRun("multibyte survives",     t_multibyte_survives);
    nexusTestRun("never splits a char",    t_never_splits_a_character);
    nexusTestRun("rejects invalid input",  t_rejects_invalid_input);
    nexusTestRun("degenerate buffers",     t_degenerate_buffers);
    nexusTestRun("all stripped is empty",  t_all_stripped_gives_empty);
    nexusTestRun("sequence lengths",       t_seq_len);
}
